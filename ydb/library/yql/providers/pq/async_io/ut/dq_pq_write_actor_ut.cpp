#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/testlib/common/test_with_actor_system.h>
#include <ydb/library/testlib/pq_helpers/mock_pq_gateway.h>
#include <ydb/library/yql/dq/actors/compute/dq_checkpoints_states.h>
#include <ydb/library/yql/providers/pq/async_io/dq_pq_write_actor.h>
#include <ydb/library/yql/providers/pq/proto/dq_io_state.pb.h>

#include <yql/essentials/minikql/mkql_alloc.h>
#include <yql/essentials/minikql/mkql_string_util.h>

namespace NYql::NDq {
namespace {

using namespace NActors;
using namespace NYdb;
using namespace NYdb::NTopic;
using namespace NKikimr::NMiniKQL;

struct TEvRequest : TEventLocal<TEvRequest, EventSpaceBegin(TEvents::ES_PRIVATE) + 100> {
    TString Method;
    ui64 PublicationId = 0;
    TString ExternalId;
    std::optional<std::string> WriterIdentity;
    std::function<void(EStatus, std::vector<TPublicationSummary>)> Reply;
};

struct TEvWrite : TEventLocal<TEvWrite, EventSpaceBegin(TEvents::ES_PRIVATE) + 101> {
    explicit TEvWrite(ui64 checkpointId)
        : CheckpointId(checkpointId)
    {}

    ui64 CheckpointId;
};

struct TEvSaved : TEventLocal<TEvSaved, EventSpaceBegin(TEvents::ES_PRIVATE) + 102> {};
struct TEvStopped : TEventLocal<TEvStopped, EventSpaceBegin(TEvents::ES_PRIVATE) + 103> {};

struct TEvError : TEventLocal<TEvError, EventSpaceBegin(TEvents::ES_PRIVATE) + 104> {
    explicit TEvError(TString issues)
        : Issues(std::move(issues))
    {}

    TString Issues;
};

class TDeferredClient : public IDeferredPublishClient {
public:
    TDeferredClient(TActorSystem* actorSystem, TActorId recipient)
        : ActorSystem(actorSystem)
        , Recipient(recipient)
    {}

    TAsyncListPublicationsResult ListPublications(const TListPublicationsSettings& settings) override {
        UNIT_ASSERT(!settings.WriterIdentity_);
        auto promise = NThreading::NewPromise<TListPublicationsResult>();
        auto request = MakeHolder<TEvRequest>();
        request->Method = "List";
        request->Reply = [promise](EStatus status, std::vector<TPublicationSummary> publications) mutable {
            promise.SetValue(TListPublicationsResult(NYdb::TStatus(status, {}), std::move(publications)));
        };
        ActorSystem->Send(Recipient, request.Release());
        return promise.GetFuture();
    }

    TAsyncCancelPublicationResult CancelPublication(const TDeferredPublication& publication, const TCancelPublicationSettings&) override {
        auto promise = NThreading::NewPromise<TCancelPublicationResult>();
        auto request = MakeHolder<TEvRequest>();
        request->Method = "Cancel";
        request->PublicationId = publication.IntPublicationId;
        request->Reply = [promise](EStatus status, std::vector<TPublicationSummary>) mutable {
            promise.SetValue(TCancelPublicationResult(NYdb::TStatus(status, {})));
        };
        ActorSystem->Send(Recipient, request.Release());
        return promise.GetFuture();
    }

    TAsyncBeginPublicationResult BeginPublication(const TString& externalId, const TBeginPublicationSettings& settings) override {
        auto promise = NThreading::NewPromise<TBeginPublicationResult>();
        auto request = MakeHolder<TEvRequest>();
        request->Method = "Begin";
        request->PublicationId = NextPublicationId++;
        request->ExternalId = externalId;
        request->WriterIdentity = settings.WriterIdentity_;
        request->Reply = [promise, externalId, id = request->PublicationId](EStatus status, std::vector<TPublicationSummary>) mutable {
            promise.SetValue(TBeginPublicationResult(NYdb::TStatus(status, {}), TDeferredPublication(id, externalId)));
        };
        ActorSystem->Send(Recipient, request.Release());
        return promise.GetFuture();
    }

    TAsyncPublishResult Publish(const TDeferredPublication&, const TPublishSettings&) override {
        UNIT_FAIL("Unexpected publication commit");
        return {};
    }

private:
    TActorSystem* const ActorSystem;
    const TActorId Recipient;
    ui64 NextPublicationId = 100;
};

class TGateway : public IPqStaticGateway {
public:
    explicit TGateway(IDeferredPublishClient::TPtr client)
        : Client(std::move(client))
    {}

    ITopicClient::TPtr GetTopicClient(const TDriver& driver, const TTopicClientSettings& settings) override {
        return Mock->GetTopicClient(driver, settings);
    }

    IFederatedTopicClient::TPtr GetFederatedTopicClient(const TDriver& driver, const NFederatedTopic::TFederatedTopicClientSettings& settings) override {
        return Mock->GetFederatedTopicClient(driver, settings);
    }

    IDeferredPublishClient::TPtr GetDeferredPublishClient(const TDriver&, const TCommonClientSettings&) override {
        return Client;
    }

    TTopicClientSettings GetTopicClientSettings() const override {
        return {};
    }

    NFederatedTopic::TFederatedTopicClientSettings GetFederatedTopicClientSettings() const override {
        return {};
    }

private:
    const IDeferredPublishClient::TPtr Client;
    const NTestUtils::IMockPqGateway::TPtr Mock = NTestUtils::CreateMockPqGateway();
};

class TWriterTestActor : public TActorBootstrapped<TWriterTestActor>, public IDqComputeActorAsyncOutput::ICallbacks {
public:
    TWriterTestActor(TActorId recipient, IPqStaticGateway::TPtr gateway, ui64 restoredPublicationId)
        : Recipient(recipient)
        , Gateway(std::move(gateway))
        , RestoredPublicationId(restoredPublicationId)
    {}

    void Bootstrap() {
        NPq::NProto::TDqPqTopicSink settings;
        settings.SetTopicPath("topic");
        settings.SetDatabase("/Root");
        settings.SetDeferredPublicationExtIdPrefix("query:execution");
        auto [sink, actor] = CreateDqPqWriteActor(std::move(settings), 0, TCollectStatsLevel::None, TString("tx"), 7,
            {}, TDriver(TDriverConfig()), CreateStructuredTokenCredentialsFactory(), this,
            MakeIntrusive<NMonitoring::TDynamicCounters>(), Gateway, false, DqPqDefaultFreeSpace, 3, false, true);
        Sink = sink;
        RegisterWithSameMailbox(actor);

        if (RestoredPublicationId) {
            NPq::NProto::TDqPqTopicSinkState proto;
            proto.SetSourceId("source");
            proto.SetDeferredPublicationIntId(RestoredPublicationId);
            TSinkState state;
            state.Data.Version = 1;
            UNIT_ASSERT(proto.SerializeToString(&state.Data.Blob));
            Sink->LoadState(state, {});
        }
        Become(&TWriterTestActor::StateFunc);
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvWrite, Handle);
        sFunc(TEvents::TEvPoison, PassAway);
    )

    void Handle(TEvWrite::TPtr& event) {
        TScopedAlloc alloc(__LOCATION__);
        TMemoryUsageInfo memoryUsage("test");
        THolderFactory holderFactory(alloc.Ref(), memoryUsage);
        NUdf::TUnboxedValue* items = nullptr;
        auto row = holderFactory.CreateDirectArrayHolder(1, items);
        items[0] = MakeString("message");
        TUnboxedValueBatch batch;
        batch.emplace_back(std::move(row));
        TMaybe<NDqProto::TCheckpoint> checkpoint;
        if (event->Get()->CheckpointId) {
            checkpoint.ConstructInPlace();
            checkpoint->SetGeneration(1);
            checkpoint->SetId(event->Get()->CheckpointId);
        }
        Sink->SendData(std::move(batch), 7, checkpoint, false);
    }

    void ResumeExecution(EResumeSource) override {}

    void OnAsyncOutputError(ui64, const TIssues& issues, NDqProto::StatusIds::StatusCode) override {
        Send(Recipient, new TEvError(issues.ToOneLineString()));
    }

    void OnAsyncOutputStateSaved(TSinkState&&, ui64, const NDqProto::TCheckpoint&) override {
        Send(Recipient, new TEvSaved());
    }

    void OnAsyncOutputStateCommitted(ui64, const NDqProto::TCheckpoint&) override {}
    void OnAsyncOutputFinished(ui64) override {}

    void PassAway() override {
        Sink->PassAway();
        Send(Recipient, new TEvStopped());
        TActorBootstrapped::PassAway();
    }

private:
    const TActorId Recipient;
    const IPqStaticGateway::TPtr Gateway;
    const ui64 RestoredPublicationId;
    IDqComputeActorAsyncOutput* Sink = nullptr;
};

class TWriteActorFixture : public NTestUtils::TTestWithActorSystemFixture {
public:
    void Start(ui64 restoredPublicationId = 0) {
        Edge = Runtime.AllocateEdgeActor();
        auto client = MakeIntrusive<TDeferredClient>(Runtime.GetActorSystem(0), Edge);
        Writer = Runtime.Register(new TWriterTestActor(Edge, MakeIntrusive<TGateway>(client), restoredPublicationId));
        Write(1);
    }

    void Write(ui64 checkpointId = 0) {
        Runtime.Send(Writer, Edge, new TEvWrite(checkpointId));
    }

    TEvRequest::TPtr Request(const TString& method) {
        auto event = Runtime.GrabEdgeEvent<TEvRequest>(Edge, Settings.WaitTimeout);
        UNIT_ASSERT(event);
        UNIT_ASSERT_VALUES_EQUAL(event->Get()->Method, method);
        return event;
    }

    void AssertError(const TString& message) {
        auto event = Runtime.GrabEdgeEvent<TEvError>(Edge, Settings.WaitTimeout);
        UNIT_ASSERT(event);
        UNIT_ASSERT_STRING_CONTAINS(event->Get()->Issues, message);
    }

    void TearDown(NUnitTest::TTestContext& ctx) override {
        if (Writer) {
            Runtime.Send(Writer, Edge, new TEvents::TEvPoison());
            UNIT_ASSERT(Runtime.GrabEdgeEvent<TEvStopped>(Edge, Settings.WaitTimeout));
        }
        TTestWithActorSystemFixture::TearDown(ctx);
    }

    TActorId Edge;
    TActorId Writer;
};

} // namespace

Y_UNIT_TEST_SUITE(TDqPqWriteActor) {
    Y_UNIT_TEST_F(CleansOnlyStalePublicationsBeforeFirstCreation, TWriteActorFixture) {
        Start(/* restoredPublicationId */ 3);
        auto list = Request("List");
        Write(); // More input while listing must not start another cleanup or publication.
        list->Get()->Reply(EStatus::SUCCESS, {
            {1, "stale-1", "query:execution:7:0:1"},
            {2, "stale-2", "query:execution:7:0:2"},
            {3, "restored", "query:execution:7:0:1"},
            {4, "current", "query:execution:7:0:3"},
            {5, "future", "query:execution:7:0:4"},
            {6, "other-task", "query:execution:70:0:1"},
            {7, "other-output", "query:execution:7:1:1"},
            {8, "other-execution", "query:other:7:0:1"},
            {9, "without-writer", std::nullopt},
            {10, "malformed-writer", "query:execution:7:0:1:extra"},
        });
        auto cancel = Request("Cancel");
        UNIT_ASSERT_VALUES_EQUAL(cancel->Get()->PublicationId, 2);
        Write(); // New input must still wait for cancellation.
        cancel->Get()->Reply(EStatus::NOT_FOUND, {});
        cancel = Request("Cancel");
        UNIT_ASSERT_VALUES_EQUAL(cancel->Get()->PublicationId, 1);
        cancel->Get()->Reply(EStatus::SUCCESS, {});

        auto begin = Request("Begin");
        UNIT_ASSERT_VALUES_EQUAL(begin->Get()->ExternalId, "query:execution:7:0:3:0");
        UNIT_ASSERT_VALUES_EQUAL(begin->Get()->WriterIdentity, "query:execution:7:0:3");
        begin->Get()->Reply(EStatus::SUCCESS, {});
        UNIT_ASSERT(Runtime.GrabEdgeEvent<TEvSaved>(Edge, Settings.WaitTimeout));

        // Advancing the checkpoint creates the next publication without listing again.
        begin = Request("Begin");
        UNIT_ASSERT_VALUES_EQUAL(begin->Get()->ExternalId, "query:execution:7:0:3:1");
        begin->Get()->Reply(EStatus::SUCCESS, {});
    }

    Y_UNIT_TEST_F(CreatesPublicationAfterEmptyList, TWriteActorFixture) {
        Start();
        Request("List")->Get()->Reply(EStatus::SUCCESS, {});
        Request("Begin")->Get()->Reply(EStatus::SUCCESS, {});
        UNIT_ASSERT(Runtime.GrabEdgeEvent<TEvSaved>(Edge, Settings.WaitTimeout));
    }

    Y_UNIT_TEST_F(ListFailurePreventsPublicationCreation, TWriteActorFixture) {
        Start();
        Request("List")->Get()->Reply(EStatus::UNAVAILABLE, {});
        AssertError("Failed to list stale deferred publications. Status: UNAVAILABLE");
    }

    Y_UNIT_TEST_F(CancelFailurePreventsPublicationCreation, TWriteActorFixture) {
        Start();
        Request("List")->Get()->Reply(EStatus::SUCCESS, {{1, "stale", "query:execution:7:0:1"}});
        Request("Cancel")->Get()->Reply(EStatus::UNAUTHORIZED, {});
        AssertError("Failed to cancel stale deferred publication #1. Status: UNAUTHORIZED");
    }
}

} // namespace NYql::NDq
