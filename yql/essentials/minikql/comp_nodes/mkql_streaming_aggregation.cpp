#include "mkql_streaming_aggregation.h"

#include <yql/essentials/minikql/computation/mkql_compute_actor_async_resume.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_pack.h>
#include <yql/essentials/minikql/mkql_node_cast.h>

#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/actorsystem.h>
#include <ydb/library/query_actor/query_actor.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/params/params.h>

#include <library/cpp/threading/future/future.h>

#include <util/generic/hash.h>
#include <util/generic/maybe.h>
#include <util/generic/string.h>

#include <list>

namespace NKikimr::NMiniKQL {

namespace {

// Hard cap on cached per-key entries before the oldest entry is offloaded to the
// state table. No TTL — entries stay in cache until they fall off the LRU end.
constexpr size_t STREAMING_AGGREGATION_CACHE_MAX_SIZE = 1;

// Minimal LRU cache for the streaming aggregation state. Distinct from
// TUnboxedKeyValueLruCacheWithTtl because we (a) have no TTL, (b) need to
// surface the evicted entry on overflow so we can write it back to the table.
class TStreamingStateLruCache {
public:
    struct TEntry {
        TString Key;
        NUdf::TUnboxedValue State;
    };

    static constexpr size_t MaxSize = STREAMING_AGGREGATION_CACHE_MAX_SIZE;

    NUdf::TUnboxedValue* Get(const TString& key) {
        auto it = Map_.find(key);
        if (it == Map_.end()) {
            return nullptr;
        }
        Usage_.splice(Usage_.end(), Usage_, it->second);
        return &it->second->State;
    }

    // Inserts (key, state) at the most-recently-used end. If the cache was at
    // capacity, returns the evicted least-recently-used entry so the caller can
    // persist it. Caller must guarantee the key is not already present.
    //
    // Special case MaxSize == 0: the cache is fully disabled — every Insert
    // returns its own argument as "evicted" (caller persists immediately) and
    // nothing is stored. This keeps the state-machine path uniform regardless
    // of whether the cache is on or off.
    TMaybe<TEntry> Insert(TString key, NUdf::TUnboxedValue state) {
        if constexpr (MaxSize == 0) {
            return TEntry{std::move(key), std::move(state)};
        }
        TMaybe<TEntry> evicted;
        if (Map_.size() >= MaxSize && !Usage_.empty()) {
            evicted.ConstructInPlace(std::move(Usage_.front()));
            Map_.erase(evicted->Key);
            Usage_.pop_front();
        }
        Usage_.emplace_back(TEntry{key, std::move(state)});
        auto last = std::prev(Usage_.end());
        Map_.emplace(std::move(key), last);
        return evicted;
    }

private:
    std::list<TEntry> Usage_;
    THashMap<TString, std::list<TEntry>::iterator> Map_;
};

class TSelectStateActor: public TQueryBase {
public:
    TSelectStateActor(const TString& tablePath, const TString& key,
                      NThreading::TPromise<TMaybe<TString>> promise)
        : TQueryBase(/*logComponent=*/0)
        , TablePath_(tablePath)
        , Key_(key)
        , Promise_(std::move(promise))
    {
        SetOperationInfo("StreamingAggregationSelect", "");
    }

    void OnRunQuery() override {
        TString sql = TStringBuilder()
            << "DECLARE $key AS String;\n"
            << "SELECT state FROM `" << TablePath_ << "` WHERE key = $key;";

        NYdb::TParamsBuilder params;
        params.AddParam("$key").String(Key_).Build();

        RunDataQuery(sql, &params);
    }

    void OnQueryResult() override {
        TMaybe<TString> result;
        if (!ResultSets.empty()) {
            NYdb::TResultSetParser parser(ResultSets.front());
            if (parser.TryNextRow()) {
                auto& stateColumn = parser.ColumnParser("state");
                if (!stateColumn.IsNull()) {
                    result = TString(stateColumn.GetOptionalString().value_or(""));
                }
            }
        }
        ResolvedResult_ = std::move(result);
        Finish();
    }

    void OnFinish(Ydb::StatusIds::StatusCode status, NYql::TIssues&&) override {
        if (status == Ydb::StatusIds::SUCCESS) {
            Promise_.SetValue(std::move(ResolvedResult_));
        } else {
            Promise_.SetException(TStringBuilder() << "SELECT for key failed, status=" << static_cast<int>(status));
        }
    }

private:
    const TString TablePath_;
    const TString Key_;
    NThreading::TPromise<TMaybe<TString>> Promise_;
    TMaybe<TString> ResolvedResult_;
};

class TUpsertStateActor: public TQueryBase {
public:
    TUpsertStateActor(const TString& tablePath, const TString& key, const TString& state,
                      NThreading::TPromise<void> promise)
        : TQueryBase(/*logComponent=*/0)
        , TablePath_(tablePath)
        , Key_(key)
        , State_(state)
        , Promise_(std::move(promise))
    {
        SetOperationInfo("StreamingAggregationUpsert", "");
    }

    void OnRunQuery() override {
        TString sql = TStringBuilder()
            << "DECLARE $key AS String;\n"
            << "DECLARE $state AS String;\n"
            << "UPSERT INTO `" << TablePath_ << "` (key, state) VALUES ($key, $state);";

        NYdb::TParamsBuilder params;
        params.AddParam("$key").String(Key_).Build();
        params.AddParam("$state").String(State_).Build();

        RunDataQuery(sql, &params);
    }

    void OnQueryResult() override {
        Finish();
    }

    void OnFinish(Ydb::StatusIds::StatusCode status, NYql::TIssues&&) override {
        if (status == Ydb::StatusIds::SUCCESS) {
            Promise_.SetValue();
        } else {
            Promise_.SetException(TStringBuilder() << "UPSERT for key failed, status=" << static_cast<int>(status));
        }
    }

private:
    const TString TablePath_;
    const TString Key_;
    const TString State_;
    NThreading::TPromise<void> Promise_;
};

enum class EStreamingStep : ui8 {
    Fetch = 0,
    AwaitSelect,
    AwaitUpsert,
};

class TStreamingAggregationState: public TComputationValue<TStreamingAggregationState> {
public:
    using TBase = TComputationValue<TStreamingAggregationState>;

    explicit TStreamingAggregationState(TMemoryUsageInfo* memInfo)
        : TBase(memInfo)
    {}

    THashMap<TString, NUdf::TUnboxedValue>& GetMap() {
        return Map_;
    }

    TStreamingStateLruCache& GetCache() {
        return Cache_;
    }

    EStreamingStep Step = EStreamingStep::Fetch;
    NUdf::TUnboxedValue PendingItem;
    NUdf::TUnboxedValue PendingKey;
    NUdf::TUnboxedValue PendingNewState;
    TString PendingKeyBytes;
    NThreading::TFuture<TMaybe<TString>> SelectFuture;
    NThreading::TFuture<void> UpsertFuture;

private:
    THashMap<TString, NUdf::TUnboxedValue> Map_;
    TStreamingStateLruCache Cache_;
};

class TStreamingAggregationFlowWrapper: public TStatefulFlowComputationNode<TStreamingAggregationFlowWrapper> {
    using TBaseComputation = TStatefulFlowComputationNode<TStreamingAggregationFlowWrapper>;

public:
    TStreamingAggregationFlowWrapper(
        TComputationMutables& mutables,
        EValueRepresentation kind,
        IComputationNode* flow,
        IComputationExternalNode* itemArg,
        IComputationExternalNode* stateArg,
        IComputationExternalNode* keyArg,
        IComputationNode* outKey,
        IComputationNode* outInit,
        IComputationNode* outUpdate,
        IComputationNode* outFinish,
        TType* keyType,
        TType* stateValueType,
        TString stateTablePath)
        : TBaseComputation(mutables, flow, kind, EValueRepresentation::Boxed)
        , Flow(flow)
        , ItemArg(itemArg)
        , StateArg(stateArg)
        , KeyArg(keyArg)
        , OutKey(outKey)
        , OutInit(outInit)
        , OutUpdate(outUpdate)
        , OutFinish(outFinish)
        , KeyType(keyType)
        , StateValueType(stateValueType)
        , StateTablePath(std::move(stateTablePath))
        , KeyPacker(mutables)
        , StateValuePacker(mutables)
    {
    }

    NUdf::TUnboxedValue DoCalculate(NUdf::TUnboxedValue& stateValue, TComputationContext& ctx) const {
        if (!stateValue.HasValue()) {
            stateValue = ctx.HolderFactory.Create<TStreamingAggregationState>();
        }
        auto& state = *static_cast<TStreamingAggregationState*>(stateValue.AsBoxed().Get());

        if (StateTablePath.empty()) {
            return DoCalculateInMemory(state, ctx);
        }
        return DoCalculateTable(state, ctx);
    }

private:
    NUdf::TUnboxedValue DoCalculateInMemory(TStreamingAggregationState& state, TComputationContext& ctx) const {
        auto item = Flow->GetValue(ctx);
        if (item.IsSpecial()) {
            return item;
        }

        ItemArg->SetValue(ctx, NUdf::TUnboxedValue(item));
        auto key = OutKey->GetValue(ctx);

        auto& packer = KeyPacker.RefMutableObject(ctx, true, KeyType);
        TString keyBytes(packer.Pack(key));

        auto& slot = state.GetMap()[keyBytes];

        if (!slot.HasValue()) {
            slot = OutInit->GetValue(ctx);
        } else {
            StateArg->SetValue(ctx, NUdf::TUnboxedValue(slot));
            slot = OutUpdate->GetValue(ctx);
        }

        StateArg->SetValue(ctx, NUdf::TUnboxedValue(slot));
        KeyArg->SetValue(ctx, std::move(key));
        return OutFinish->GetValue(ctx);
    }

    NUdf::TUnboxedValue DoCalculateTable(TStreamingAggregationState& state, TComputationContext& ctx) const {
        MKQL_ENSURE(NActors::TlsActivationContext, "StreamingAggregation table backend requires actor TLS context");
        MKQL_ENSURE(ctx.AsyncResume,
                    "StreamingAggregation table backend requires ctx.AsyncResume to be plumbed by the compute actor");
        auto* actorSystem = NActors::TlsActivationContext->ActorSystem();
        const auto& asyncResume = *ctx.AsyncResume;

        const auto clearPending = [&]() {
            state.PendingItem = NUdf::TUnboxedValue();
            state.PendingKey = NUdf::TUnboxedValue();
            state.PendingNewState = NUdf::TUnboxedValue();
            state.PendingKeyBytes.clear();
        };

        const auto emitFinish = [&](const NUdf::TUnboxedValue& key, const NUdf::TUnboxedValue& curState) {
            StateArg->SetValue(ctx, NUdf::TUnboxedValue(curState));
            KeyArg->SetValue(ctx, NUdf::TUnboxedValue(key));
            return OutFinish->GetValue(ctx);
        };

        for (;;) {
            switch (state.Step) {
                case EStreamingStep::Fetch: {
                    auto item = Flow->GetValue(ctx);
                    if (item.IsSpecial()) {
                        return item;
                    }
                    ItemArg->SetValue(ctx, NUdf::TUnboxedValue(item));
                    auto key = OutKey->GetValue(ctx);

                    auto& keyPacker = KeyPacker.RefMutableObject(ctx, true, KeyType);
                    TString keyBytes(keyPacker.Pack(key));

                    if (auto* slot = state.GetCache().Get(keyBytes)) {
                        // Cache hit: in-place update, emit, no I/O, no yield.
                        StateArg->SetValue(ctx, NUdf::TUnboxedValue(*slot));
                        *slot = OutUpdate->GetValue(ctx);
                        return emitFinish(key, *slot);
                    }

                    // Cache miss: load previous state from the table.
                    state.PendingItem = std::move(item);
                    state.PendingKey = std::move(key);
                    state.PendingKeyBytes = keyBytes;

                    auto promise = NThreading::NewPromise<TMaybe<TString>>();
                    auto future = promise.GetFuture();
                    actorSystem->Register(new TSelectStateActor(StateTablePath, keyBytes, std::move(promise)));
                    asyncResume.SubscribeWakeUp(future);
                    state.SelectFuture = std::move(future);
                    state.Step = EStreamingStep::AwaitSelect;
                    return NUdf::TUnboxedValuePod::MakeYield();
                }
                case EStreamingStep::AwaitSelect: {
                    if (!state.SelectFuture.HasValue()) {
                        return NUdf::TUnboxedValuePod::MakeYield();
                    }
                    auto packedPrev = state.SelectFuture.ExtractValue();
                    state.SelectFuture = {};

                    auto& stateValuePacker = StateValuePacker.RefMutableObject(ctx, true, StateValueType);

                    NUdf::TUnboxedValue newState;
                    ItemArg->SetValue(ctx, NUdf::TUnboxedValue(state.PendingItem));
                    if (packedPrev) {
                        StateArg->SetValue(ctx, stateValuePacker.Unpack(*packedPrev, ctx.HolderFactory));
                        newState = OutUpdate->GetValue(ctx);
                    } else {
                        newState = OutInit->GetValue(ctx);
                    }
                    state.PendingNewState = newState;

                    auto evicted = state.GetCache().Insert(state.PendingKeyBytes, NUdf::TUnboxedValue(newState));
                    if (evicted) {
                        // Cache full: persist the LRU entry we just kicked out, then
                        // emit current row when that write completes.
                        TString packedEvicted(stateValuePacker.Pack(evicted->State));
                        auto promise = NThreading::NewPromise<void>();
                        auto future = promise.GetFuture();
                        actorSystem->Register(new TUpsertStateActor(StateTablePath, evicted->Key, packedEvicted, std::move(promise)));
                        asyncResume.SubscribeWakeUp(future);
                        state.UpsertFuture = std::move(future);
                        state.Step = EStreamingStep::AwaitUpsert;
                        return NUdf::TUnboxedValuePod::MakeYield();
                    }

                    auto out = emitFinish(state.PendingKey, state.PendingNewState);
                    clearPending();
                    state.Step = EStreamingStep::Fetch;
                    return out;
                }
                case EStreamingStep::AwaitUpsert: {
                    if (!state.UpsertFuture.HasValue()) {
                        return NUdf::TUnboxedValuePod::MakeYield();
                    }
                    state.UpsertFuture.GetValue();
                    state.UpsertFuture = {};

                    auto out = emitFinish(state.PendingKey, state.PendingNewState);
                    clearPending();
                    state.Step = EStreamingStep::Fetch;
                    return out;
                }
            }
        }
    }

    void RegisterDependencies() const final {
        if (const auto flow = FlowDependsOn(Flow)) {
            Own(flow, ItemArg);
            Own(flow, StateArg);
            Own(flow, KeyArg);
            DependsOn(flow, OutKey);
            DependsOn(flow, OutInit);
            DependsOn(flow, OutUpdate);
            DependsOn(flow, OutFinish);
        }
    }

    IComputationNode* const Flow;
    IComputationExternalNode* const ItemArg;
    IComputationExternalNode* const StateArg;
    IComputationExternalNode* const KeyArg;
    IComputationNode* const OutKey;
    IComputationNode* const OutInit;
    IComputationNode* const OutUpdate;
    IComputationNode* const OutFinish;
    TType* const KeyType;
    TType* const StateValueType;
    const TString StateTablePath;
    TMutableObjectOverBoxedValue<TValuePackerBoxed> KeyPacker;
    TMutableObjectOverBoxedValue<TValuePackerBoxed> StateValuePacker;
};

} // anonymous namespace

IComputationNode* WrapStreamingAggregation(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 9, "StreamingAggregation expected 9 args, got " << callable.GetInputsCount());

    const auto returnType = callable.GetType()->GetReturnType();
    MKQL_ENSURE(returnType->IsFlow(), "StreamingAggregation expects flow return type");

    const auto flow = LocateNode(ctx.NodeLocator, callable, 0);
    const auto itemArg = LocateExternalNode(ctx.NodeLocator, callable, 1);
    const auto stateArg = LocateExternalNode(ctx.NodeLocator, callable, 2);
    const auto keyArg = LocateExternalNode(ctx.NodeLocator, callable, 3);
    const auto outKey = LocateNode(ctx.NodeLocator, callable, 4);
    const auto outInit = LocateNode(ctx.NodeLocator, callable, 5);
    const auto outUpdate = LocateNode(ctx.NodeLocator, callable, 6);
    const auto outFinish = LocateNode(ctx.NodeLocator, callable, 7);

    const auto keyType = callable.GetInput(3).GetStaticType();
    const auto stateValueType = callable.GetInput(2).GetStaticType();

    const auto& stateTablePathLiteral = AS_VALUE(TDataLiteral, callable.GetInput(8));
    TString stateTablePath(stateTablePathLiteral->AsValue().AsStringRef());

    return new TStreamingAggregationFlowWrapper(
        ctx.Mutables, GetValueRepresentation(returnType), flow,
        itemArg, stateArg, keyArg,
        outKey, outInit, outUpdate, outFinish, keyType, stateValueType,
        std::move(stateTablePath));
}

} // namespace NKikimr::NMiniKQL
