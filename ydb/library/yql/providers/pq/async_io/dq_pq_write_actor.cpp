#include "dq_pq_write_actor.h"
#include "probes.h"

#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/event_local.h>
#include <ydb/library/actors/core/events.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>
#include <ydb/library/yql/dq/actors/compute/dq_checkpoints_states.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h>
#include <ydb/library/yql/dq/actors/protos/dq_events.pb.h>
#include <ydb/library/yql/dq/common/dq_common.h>
#include <ydb/library/yql/providers/pq/common/pq_events_processor.h>
#include <ydb/library/yql/providers/pq/proto/dq_io_state.pb.h>
#include <ydb/library/yverify_stream/yverify_stream.h>
#include <ydb/public/sdk/cpp/adapters/issue/issue.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/topic/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/federated_topic/federated_topic.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/credentials/credentials.h>

#include <yql/essentials/minikql/comp_nodes/mkql_saveload.h>
#include <yql/essentials/minikql/mkql_alloc.h>
#include <yql/essentials/minikql/mkql_string_util.h>
#include <yql/essentials/utils/log/log.h>
#include <yql/essentials/utils/yql_panic.h>

#include <library/cpp/lwtrace/mon/mon_lwtrace.h>

#include <util/generic/algorithm.h>
#include <util/generic/hash.h>
#include <util/string/builder.h>

#include <algorithm>
#include <queue>
#include <variant>

#define SINK_LOG_T(s) LOG_TRACE_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, LogPrefix() << s)
#define SINK_LOG_D(s) LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, LogPrefix() << s)
#define SINK_LOG_I(s) LOG_INFO_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, LogPrefix() << s)
#define SINK_LOG_N(s) LOG_NOTICE_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, LogPrefix() << s)
#define SINK_LOG_W(s) LOG_WARN_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, LogPrefix() << s)
#define SINK_LOG_E(s) LOG_ERROR_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, LogPrefix() << s)
#define SINK_LOG_C(s) LOG_CRIT_S(*TlsActivationContext,  NKikimrServices::KQP_COMPUTE, LogPrefix() << s)

namespace NYql::NDq {

using namespace NActors;
using namespace NKikimr::NMiniKQL;

namespace {

LWTRACE_USING(DQ_PQ_PROVIDER);

struct TEvPrivate {
    // Event ids
    enum EEv : ui32 {
        EvBegin = EventSpaceBegin(TEvents::ES_PRIVATE),

        EvPqEventsReady = EvBegin,
        EvExecuteTopicEvent,
        EvDeferredPublicationCreated,
        EvDeferredPublicationCommitted,

        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE), "expect EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE)");

    // Events

    struct TEvPqEventsReady : public TEventLocal<TEvPqEventsReady, EvPqEventsReady> {
    };

    struct TEvExecuteTopicEvent : public TTopicEventBase<TEvExecuteTopicEvent, EvExecuteTopicEvent> {
        using TTopicEventBase::TTopicEventBase;
    };

    template <typename TResult, ui32 EEventId>
    struct TEvApiResult : public TEventLocal<TEvApiResult<TResult, EEventId>, EEventId> {
        explicit TEvApiResult(TResult result)
            : Result(std::move(result))
        {}

        const TResult Result;
    };

    using TEvDeferredPublicationCreated = TEvApiResult<NYdb::NTopic::TBeginPublicationResult, EvDeferredPublicationCreated>;
    using TEvDeferredPublicationCommitted = TEvApiResult<NYdb::NTopic::TPublishResult, EvDeferredPublicationCommitted>;
};

} // anonymous namespace

class TDqPqWriteActor final : public TActor<TDqPqWriteActor>, public IActorExceptionHandler, public IDqComputeActorAsyncOutput, TTopicEventProcessor<TEvPrivate::TEvExecuteTopicEvent> {
    static constexpr ui32 STATE_VERSION = 1;
    static constexpr ui32 MAX_MESSAGE_SIZE = 1_MB;

    using TBase = TActor<TDqPqWriteActor>;

    struct TMetrics {
        TMetrics(
            const TTxId& txId, const ui64 taskId, NMonitoring::TDynamicCounterPtr counters, const bool enableStreamingQueriesCounters,
            const bool enableCountersPerTask, const bool enableDeferredPublication
        )
            : TxId(std::visit([](const auto& arg) {
                return ToString(arg);
            }, txId))
            , Counters(std::move(counters))
            , SubGroup(Counters ? Counters->GetSubgroup("sink", "PqSink") : MakeIntrusive<NMonitoring::TDynamicCounters>())
        {
            auto task = SubGroup;

            if (enableStreamingQueriesCounters) {
                task = task->GetSubgroup("tx_id", TxId);

                if (enableCountersPerTask) {
                    task = task->GetSubgroup("task_id", ToString(taskId));
                }
            }

            LastAckLatency = task->GetCounter("LastAckLatencyMs");
            InFlyCheckpoints = task->GetCounter("InFlyCheckpoints");
            InFlyData = task->GetCounter("InFlyData");
            AlreadyWritten = task->GetCounter("AlreadyWritten");
            FirstContinuationTokenMs = task->GetCounter("FirstContinuationTokenMs");
            EgressDataRate = task->GetCounter("EgressDataRate", true);

            if (enableDeferredPublication) {
                LastBeginPublicationLatency = task->GetCounter("DeferredPublication/LastBeginLatencyMs");
                LastPublishLatency = task->GetCounter("DeferredPublication/LastPublishLatencyMs");
                LastPublicationActiveDuration = task->GetCounter("DeferredPublication/LastActiveDurationMs");
                LastPublicationPendingCommitDuration = task->GetCounter("DeferredPublication/LastPendingCommitDurationMs");
                InFlyActivePublications = task->GetCounter("DeferredPublication/InFlyActive");
                InFlyPendingCommitPublications = task->GetCounter("DeferredPublication/InFlyPendingCommit");
                UnpublishedDataSize = task->GetCounter("DeferredPublication/UnpublishedDataSize");
            }
        }

        ~TMetrics() {
            SubGroup->RemoveSubgroup("tx_id", TxId);
        }

        void ReportFirstContinuationToken() const {
            if (*FirstContinuationTokenMs == 0) {
                FirstContinuationTokenMs->Set((TInstant::Now() - StartTime).MilliSeconds());
            }
        }

        // Common counters
        NMonitoring::TDynamicCounters::TCounterPtr LastAckLatency;
        NMonitoring::TDynamicCounters::TCounterPtr InFlyCheckpoints;
        NMonitoring::TDynamicCounters::TCounterPtr InFlyData;
        NMonitoring::TDynamicCounters::TCounterPtr AlreadyWritten;
        NMonitoring::TDynamicCounters::TCounterPtr EgressDataRate;

        // Deferred publication counters
        NMonitoring::TDynamicCounters::TCounterPtr LastBeginPublicationLatency;
        NMonitoring::TDynamicCounters::TCounterPtr LastPublishLatency;
        NMonitoring::TDynamicCounters::TCounterPtr LastPublicationActiveDuration;
        NMonitoring::TDynamicCounters::TCounterPtr LastPublicationPendingCommitDuration;
        NMonitoring::TDynamicCounters::TCounterPtr InFlyActivePublications;
        NMonitoring::TDynamicCounters::TCounterPtr InFlyPendingCommitPublications;
        NMonitoring::TDynamicCounters::TCounterPtr UnpublishedDataSize;

    private:
        const TInstant StartTime = TInstant::Now();
        const TString TxId;
        const NMonitoring::TDynamicCounterPtr Counters;
        const NMonitoring::TDynamicCounterPtr SubGroup;

        NMonitoring::TDynamicCounters::TCounterPtr FirstContinuationTokenMs;
    };

    struct TDeferredPublishInfo {
        TDeferredPublishInfo(const i64 currentExecutionGeneration, const NPq::NProto::TDqPqTopicSink& sinkParams)
            : WriterIdentity(sinkParams.GetDeferredPublicationExtIdPrefix())
            , DeferredPublicationExtIdPrefix(TStringBuilder() << WriterIdentity << ":" << currentExecutionGeneration)
        {}

        operator bool() const {
            return !WriterIdentity.empty();
        }

        const TString WriterIdentity;
        const TString DeferredPublicationExtIdPrefix;
    };

    class TDataBuffer {
        struct TAckInfo {
            TAckInfo(const i64 messageSize, const TInstant startTime, const ui64 seqNo)
                : MessageSize(messageSize)
                , StartTime(startTime)
                , SeqNo(seqNo)
            {}

            const i64 MessageSize = 0;
            const TInstant StartTime;
            const ui64 SeqNo = 0;
        };

    public:
        ui64 NotSentSize() const {
            return NotSentMessages.size();
        }

        ui64 InflightSize() const {
            return InflightMessages.size();
        }

        ui64 GetLastMessageSeqNo() const {
            Y_VALIDATE(NextMessageSeqNo > 0, "Unexpected next message seq no: " << NextMessageSeqNo);
            return NotSentMessages.size() + NextMessageSeqNo - 1;
        }

        void PushMessage(TString&& message) {
            NotSentMessages.emplace(std::move(message));
        }

        ui64 PopInflightMessage(i64& freeSpace, const TMetrics& metrics) {
            Y_VALIDATE(!InflightMessages.empty(), "Unexpected empty inflight messages queue");
            const auto& ackInfo = InflightMessages.front();

            metrics.LastAckLatency->Set((TInstant::Now() - ackInfo.StartTime).MilliSeconds());
            metrics.InFlyData->Dec();
            freeSpace += ackInfo.MessageSize;

            // Use seqNo stored on our side because without deduplication we do not specify SeqNo on Write().
            // We expecting that acks comes from server in strictly same order as sent messages.
            ConfirmedSeqNo = ackInfo.SeqNo;
            InflightMessages.pop();

            return ConfirmedSeqNo;
        }

    private:
        std::queue<TString> NotSentMessages;
        std::queue<TAckInfo> InflightMessages;
        ui64 NextMessageSeqNo = 1;
        ui64 ConfirmedSeqNo = 0;
    };

    class TCheckpointsState {
        struct TCheckpointInfo {
            TCheckpointInfo(const ui64 waitConfirmedSeqNo, NDqProto::TCheckpoint checkpoint)
                : WaitConfirmedSeqNo(waitConfirmedSeqNo)
                , Checkpoint(std::move(checkpoint))
            {}

            const ui64 WaitConfirmedSeqNo = 0;
            const NDqProto::TCheckpoint Checkpoint;
        };

    public:
        void PushCheckpoint(const ui64 waitConfirmedSeqNo, NDqProto::TCheckpoint checkpoint) {
            PendingSendCheckpoints.emplace(waitConfirmedSeqNo, std::move(checkpoint));
        }

    private:
        std::queue<TCheckpointInfo> PendingSendCheckpoints; // Checkpoints for which data still not sent
    };

    struct TTopicEventProcessor {
        explicit TTopicEventProcessor(TDqPqWriteActor& self)
            : Self(self)
        {}

        bool operator()(const NYdb::NTopic::TSessionClosedEvent& ev) {
            Self.Fail(ev, TStringBuilder() << "Write session to topic \"" << Self.SinkParams.GetTopicPath() << "\" was closed");
            return false;
        }

        bool operator()(NYdb::NTopic::TWriteSessionEvent::TReadyToAcceptEvent& ev) {
            SINK_LOG_T("Received continuation token, buffer size: " << Self.Buffer.NotSentSize());
            Self.Metrics.ReportFirstContinuationToken();
            Self.ContinuationToken = std::move(ev.ContinuationToken);
            return true;
        }

        bool operator()(const NYdb::NTopic::TWriteSessionEvent::TAcksEvent& ev) {
            const auto& acks = ev.Acks;
            SINK_LOG_T("Got acks #" << acks.size());

            for (auto it = ev.Acks.begin(); it != ev.Acks.end(); ++it) {
                const auto sdkAckSeqNo = it->SeqNo;
                const auto state = it->State;
                SINK_LOG_T("Ack seq no (from TAcksEvent) " << sdkAckSeqNo << ", state: " << state);

                if (state == NYdb::NTopic::TWriteSessionEvent::TWriteAck::EEventState::EES_DISCARDED) {
                    Self.Fail(TStringBuilder() << "Message with seqNo " << sdkAckSeqNo << " was discarded");
                    return false;
                }

                if (state == NYdb::NTopic::TWriteSessionEvent::TWriteAck::EEventState::EES_ALREADY_WRITTEN) {
                    Self.Metrics.AlreadyWritten->Inc();
                }

                Y_VALIDATE(Self.Buffer.InflightSize() > 0, "Got unexpected ack with seq no: " << sdkAckSeqNo);
                const auto seqNo = Self.Buffer.PopInflightMessage(Self.FreeSpace, Self.Metrics);
                SINK_LOG_T("Ack seq no (from InflightMessages) " << seqNo);
            }

            return true;
        }

    private:
        TString LogPrefix() const {
            return TStringBuilder() << Self.LogPrefix() << "[TTopicEventProcessor] ";
        }

        TDqPqWriteActor& Self;
    };

public:
    static constexpr char ActorName[] = "DQ_PQ_WRITE_ACTOR";

    TDqPqWriteActor(
        const ui64 outputIndex, const TCollectStatsLevel statsLevel, TTxId txId, const ui64 taskId, NPq::NProto::TDqPqTopicSink&& sinkParams,
        NYdb::TDriver driver, std::shared_ptr<NYdb::ICredentialsProviderFactory> credentialsProviderFactory, IDqComputeActorAsyncOutput::ICallbacks* const callbacks,
        ::NMonitoring::TDynamicCounterPtr counters, const i64 freeSpace, const i64 currentExecutionGeneration, IPqStaticGateway::TPtr pqGateway,
        const bool enableStreamingQueriesCounters, const bool enableStreamingQueriesPqSinkDeduplicationFeatureFlag
    )
        : TActor<TDqPqWriteActor>(&TDqPqWriteActor::StateFunc)
        , OutputIndex(outputIndex)
        , TaskId(taskId)
        , TxId(std::move(txId))
        , SinkParams(std::move(sinkParams))
        , Callbacks(callbacks)
        , PqGateway(std::move(pqGateway))
        , CredentialsProviderFactory(std::move(credentialsProviderFactory))
        , Driver(std::move(driver))
        , Metrics(TxId, TaskId, std::move(counters), enableStreamingQueriesCounters, /* enableCountersPerTask */ false, !SinkParams.GetDeferredPublicationExtIdPrefix().empty())
        , EnableDeduplication(enableStreamingQueriesPqSinkDeduplicationFeatureFlag && SinkParams.GetEnableDeduplication())
        , DeferredPublishInfo(currentExecutionGeneration, SinkParams)
        , FreeSpace(freeSpace)
    {
        Y_VALIDATE(Callbacks, "Missing callbacks");
        Y_VALIDATE(!EnableDeduplication || !DeferredPublishInfo, "Deferred publications can not be used with deduplication");

        EgressStats.Level = statsLevel;
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvPrivate::TEvExecuteTopicEvent, HandleTopicEvent);
        hFunc(TEvPrivate::TEvPqEventsReady, Handle);
        hFunc(TEvPrivate::TEvDeferredPublicationCreated, Handle);
        hFunc(TEvPrivate::TEvDeferredPublicationCommitted, Handle);
    )

private:
    // IActor

    void Registered(TActorSystem* sys, const TActorId& owner) final {
        TBase::Registered(sys, owner);
        OwnerId = owner;
    }

    void PassAway() final { // Is called from Compute Actor, implements IActor & IDqComputeActorAsyncOutput
        if (WriteSession) {
            WriteSession->Close(/* closeTimeout */ TDuration::Zero());
        }

        TBase::PassAway();
    }

    // IActorExceptionHandler

    bool OnUnhandledException(const std::exception& e) final {
        Fail(NDqProto::StatusIds::INTERNAL_ERROR, TStringBuilder() << "Unexpected exception: " << e.what());
        return true;
    }

    // IDqComputeActorAsyncOutput

    ui64 GetOutputIndex() const final {
        return OutputIndex;
    }

    i64 GetFreeSpace() const final {
        return FreeSpace;
    }

    const TDqAsyncStats& GetEgressStats() const final {
        return EgressStats;
    }

    void SendData(TUnboxedValueBatch&& batch, const i64 dataSize, const TMaybe<NDqProto::TCheckpoint>& checkpoint, const bool finished) final {
        SINK_LOG_T("SendData. Batch: " << batch.RowCount() << ". Has checkpoint: " << checkpoint.Defined() << ". Finished: " << finished);
        Y_UNUSED(dataSize);

        if (finished) {
            Finished = true;
        }

        Y_VALIDATE(!batch.IsWide(), "Wide batch is not supported");
        if (!batch.ForEachRow([&](const auto& value) {
            if (!value.IsBoxed()) {
                Fail("Struct with single field was expected");
                return false;
            }

            const NUdf::TUnboxedValue dataCol = value.GetElement(0);
            if (!dataCol.IsString() && !dataCol.IsEmbedded()) {
                Fail("Non string value could not be written to YDS stream");
                return false;
            }

            TString data(dataCol.AsStringRef());

            LWPROBE(PqWriteDataToSend, ToString(TxId), SinkParams.GetTopicPath(), data);
            SINK_LOG_T("Received data for sending: " << data);

            const auto messageSize = GetItemSize(data);
            if (messageSize > MAX_MESSAGE_SIZE) {
                Fail(TStringBuilder() << "Max message size for YDS is " << MAX_MESSAGE_SIZE
                    << " bytes but received message with size of " << messageSize << " bytes");
                return false;
            }

            FreeSpace -= messageSize;
            Metrics.InFlyData->Inc();
            Buffer.PushMessage(std::move(data));
            return true;
        })) {
            return;
        }

        if (checkpoint) {
            const auto seqNo = Buffer.GetLastMessageSeqNo();
            SINK_LOG_D(CheckpointLogString(*checkpoint) << "Register checkpoint, seq no: " << seqNo);
            CheckpointsState.PushCheckpoint(seqNo, *checkpoint);
        }

        Process();
    }

    void CommitState(const NDqProto::TCheckpoint& checkpoint) final {
        // TODO
    }

    void LoadState(const TSinkState& state) final {
        // TODO
    }

    // Events

    void Handle(TEvPrivate::TEvPqEventsReady::TPtr&) {
        SINK_LOG_T("New PQ write session events arrived");

        Process();
        SubscribeOnNextEvent();
    }

    void Handle(TEvPrivate::TEvDeferredPublicationCreated::TPtr& ev) {
        // TODO

        const auto& result = ev->Get()->Result;
        if (!result.IsSuccess()) {
            Fail(result, "Failed to create deferred publication");
            return;
        }

        const auto intPublicationId = result.GetIntPublicationId();
        SINK_LOG_D("Publication created: " << result << ", internal id: " << intPublicationId);

        // TODO
    }

    void Handle(TEvPrivate::TEvDeferredPublicationCommitted::TPtr& ev) {
        // TODO

        const auto& result = ev->Get()->Result;
        if (!result.IsSuccess()) {
            Fail(result, "Failed to commit deferred publication");
            return;
        }

        SINK_LOG_D("Publication committed: " << result);

        // TODO
    }

    // Initialization

    NYdb::NFederatedTopic::TFederatedTopicClientSettings GetFederatedTopicClientSettings() { // Must be called after actor registration
        NYdb::NFederatedTopic::TFederatedTopicClientSettings opts = PqGateway->GetFederatedTopicClientSettings();

        if (SinkParams.GetUseActorSystemThreadsInTopicClient()) {
            SetupTopicClientSettings(ActorContext().ActorSystem(), SelfId(), opts);
        }

        opts.Database(SinkParams.GetDatabase())
            .DiscoveryEndpoint(SinkParams.GetEndpoint())
            .SslCredentials(NYdb::TSslCredentials(SinkParams.GetUseSsl()))
            .CredentialsProviderFactory(CredentialsProviderFactory);

        return opts;
    }

    IFederatedTopicClient& GetFederatedTopicClient() {
        if (!FederatedTopicClient) {
            FederatedTopicClient = PqGateway->GetFederatedTopicClient(Driver, GetFederatedTopicClientSettings());
        }

        return *FederatedTopicClient;
    }

    const TString& GetSourceId() { // Must be called after state loading
        if (!SourceId) {
            SourceId = CreateGuidAsString(); // Not loaded from state, so this is the first run.
        }

        return SourceId;
    }

    NYdb::NTopic::TWriteSessionSettings GetWriteSessionSettings() {
        auto settings = NYdb::NTopic::TWriteSessionSettings()
            .Path(SinkParams.GetTopicPath())
            .TraceId(LogPrefix())
            .MaxMemoryUsage(FreeSpace)
            .Codec(SinkParams.GetClusterType() == NPq::NProto::DataStreams
                ? NYdb::NTopic::ECodec::RAW
                : NYdb::NTopic::ECodec::GZIP);

        settings.DeduplicationEnabled(EnableDeduplication);

        if (EnableDeduplication) {
            const auto& sourceId = GetSourceId();
            settings.ProducerId(sourceId);
            settings.MessageGroupId(sourceId);
        }

        return settings;
    }

    void CreateSessionIfNotExists() {
        if (!WriteSession) {
            SINK_LOG_T("Create new PQ write session");
            WriteSession = GetFederatedTopicClient().CreateWriteSession(GetWriteSessionSettings());
            SubscribeOnNextEvent();
        }
    }

    // Message processing

    void Process() {
        if (Failed) {
            return;
        }

        CreateSessionIfNotExists();

        if (!HandleNewPQEvents()) {
            // Write session returned error
            return;
        }

        // TODO: advance checkpoints

        if (!Buffer.NotSentSize()) {
            CheckFinished();
            return;
        }

        // TODO: create publication

        if (ContinuationToken) {
            WriteNextMessage(std::move(*ContinuationToken));
            ContinuationToken.reset();

            // TODO: advance checkpoints
        }
    }

    void SubscribeOnNextEvent() const {
        if (!WriteSession) {
            return;
        }

        SINK_LOG_T("Subscribe on next event");
        WriteSession->WaitEvent().Subscribe([actorSystem = TActivationContext::ActorSystem(), selfId = SelfId()](const auto&){
            actorSystem->Send(selfId, new TEvPrivate::TEvPqEventsReady());
        });
    }

    bool HandleNewPQEvents() {
        if (!WriteSession) {
            return false;
        }

        auto events = WriteSession->GetEvents();
        const auto initialFreeSpace = FreeSpace;
        SINK_LOG_T("Extracted #" << events.size() << " PQ write session events, free space: " << initialFreeSpace);

        for (auto& event : events) {
            if (!std::visit(TTopicEventProcessor{*this}, event)) {
                return false;
            }
        }

        if (initialFreeSpace <= 0 && FreeSpace > 0) {
            SINK_LOG_T("Free space increased: " << FreeSpace << " -> " << initialFreeSpace);
            Callbacks->ResumeExecution();
        }

        return true;
    }

    void WriteNextMessage(NYdb::NTopic::TContinuationToken&& token) {
        // TODO
    }

    void CheckFinished() {
        // TODO
    }

    // Common

    void Fail(const NDqProto::StatusIds::StatusCode status, const TIssues& issues) {
        Failed = true;

        if (WriteSession) {
            WriteSession->Close(TDuration::Zero());
            WriteSession.reset();
        }

        SINK_LOG_W("Fail. Status: " << status << ". Issues: " << issues.ToOneLineString());
        Callbacks->OnAsyncOutputError(OutputIndex, issues, status);
    }

    void Fail(const NDqProto::StatusIds::StatusCode status, const TString& message) {
        Fail(status, {TIssue(message)});
    }

    void Fail(const TString& message) {
        Fail(NDqProto::StatusIds::EXTERNAL_ERROR, message);
    }

    void Fail(const NYdb::TStatus& status, const TString& message) {
        Y_VALIDATE(!status.IsSuccess(), "Unexpected success status for fail");

        TIssue rootIssue(TStringBuilder() << message << ". Status: " << status.GetStatus());
        for (const auto& issue : status.GetIssues()) {
            rootIssue.AddSubIssue(MakeIntrusive<TIssue>(NYdb::NAdapters::ToYqlIssue(issue)));
        }

        Fail(NDqProto::StatusIds::EXTERNAL_ERROR, {rootIssue});
    }

    TString LogPrefix() const {
        auto prefix = TStringBuilder() << "[" << ActorName << "] ";

        if (OwnerId) {
            prefix << "OwnerId: " << *OwnerId << ". ActorId: " << SelfId() << ". ";
        }

        return prefix << "TxId: " << TxId << ". TaskId: " << TaskId << ". ";
    }

    static i64 GetItemSize(const TString& item) {
        return std::max(static_cast<i64>(item.size()), static_cast<i64>(1));
    }

    static TString CheckpointLogString(const NDqProto::TCheckpoint& checkpoint) {
        return TStringBuilder() << "[Checkpoint " << checkpoint.GetGeneration() << "." << checkpoint.GetId() << "] ";
    }

    const ui64 OutputIndex = 0;
    const ui64 TaskId;
    const TTxId TxId;
    const NPq::NProto::TDqPqTopicSink SinkParams;
    IDqComputeActorAsyncOutput::ICallbacks* const Callbacks = nullptr;
    const IPqStaticGateway::TPtr PqGateway;
    const NYdb::TCredentialsProviderFactoryPtr CredentialsProviderFactory;
    const NYdb::TDriver Driver;
    const TMetrics Metrics;
    const bool EnableDeduplication = false;
    const TDeferredPublishInfo DeferredPublishInfo;

    std::optional<TActorId> OwnerId;
    TDqAsyncStats EgressStats;
    TString SourceId;
    IFederatedTopicClient::TPtr FederatedTopicClient;
    std::shared_ptr<NYdb::NTopic::IWriteSession> WriteSession;

    i64 FreeSpace = 0;
    bool Finished = false;
    bool Failed = false;
    TDataBuffer Buffer;
    TCheckpointsState CheckpointsState;
    std::optional<NYdb::NTopic::TContinuationToken> ContinuationToken;
/*
    void LoadState(const TSinkState& state) override {
        Y_ABORT_UNLESS(NextSeqNo == 1);
        const auto& data = state.Data;
        if (data.Version == STATE_VERSION) { // Current version
            NPq::NProto::TDqPqTopicSinkState stateProto;
            YQL_ENSURE(stateProto.ParseFromString(data.Blob), "Serialized state is corrupted");
            SINK_LOG_D("Load state: " << stateProto);
            SourceId = stateProto.GetSourceId();
            ConfirmedSeqNo = stateProto.GetConfirmedSeqNo();
            NextSeqNo = ConfirmedSeqNo + 1;
            EgressStats.Bytes = stateProto.GetEgressBytes();
            return;
        }
        ythrow yexception() << "Invalid state version " << data.Version;
    }

    TSinkState BuildState(const NDqProto::TCheckpoint& checkpoint) {
        NPq::NProto::TDqPqTopicSinkState stateProto;
        stateProto.SetSourceId(GetSourceId());
        stateProto.SetConfirmedSeqNo(ConfirmedSeqNo);
        stateProto.SetEgressBytes(EgressStats.Bytes);
        TString serializedState;
        YQL_ENSURE(stateProto.SerializeToString(&serializedState));

        TSinkState sinkState;
        auto& data = sinkState.Data;
        data.Version = STATE_VERSION;
        data.Blob = serializedState;
        SINK_LOG_T("Save checkpoint " << checkpoint << " state: " << stateProto);
        return sinkState;
    }

    void WriteNextMessage(NYdb::NTopic::TContinuationToken&& token) {
        std::optional<uint64_t> seqNo;
        if (EnableDeduplication) {
            seqNo = NextSeqNo;
        }
        SINK_LOG_T("Write message into PQ session: " << Buffer.front());
        WriteSession->Write(std::move(token), Buffer.front(), seqNo);
        auto itemSize = GetItemSize(Buffer.front());
        WaitingAcks.emplace(itemSize, TInstant::Now(), NextSeqNo);
        NextSeqNo++;
        EgressStats.Bytes += itemSize;
        Metrics.EgressDataRate->Add(itemSize);
        Buffer.pop();
    }

    struct TTopicEventProcessor {
        std::optional<TIssues> operator()(NYdb::NTopic::TWriteSessionEvent::TAcksEvent& ev) {
            if (ev.Acks.empty()) {
                LOG_D(Self.LogPrefix << "Empty ack");
                return std::nullopt;
            }

            //Y_ABORT_UNLESS(Self.ConfirmedSeqNo == 0 || ev.Acks.front().SeqNo == Self.ConfirmedSeqNo + 1);

            for (auto it = ev.Acks.begin(); it != ev.Acks.end(); ++it) {
                //Y_ABORT_UNLESS(it == ev.Acks.begin() || it->SeqNo == std::prev(it)->SeqNo + 1);
                LOG_T(Self.LogPrefix << "Ack seq no (from TAcksEvent) " << it->SeqNo);
                if (it->State == NYdb::NTopic::TWriteSessionEvent::TWriteAck::EEventState::EES_DISCARDED) {
                    TIssues issues;
                    issues.AddIssue(TStringBuilder() << "Message with seqNo " << it->SeqNo << " was discarded");
                    return issues;
                }

                if (it->State == NYdb::NTopic::TWriteSessionEvent::TWriteAck::EEventState::EES_ALREADY_WRITTEN) {
                    Self.Metrics.AlreadyWritten->Inc();
                }

                Y_VALIDATE(!Self.WaitingAcks.empty(), "Got unexpected ack with seq no: " << it->SeqNo);
                const auto& ackInfo = Self.WaitingAcks.front();
                Self.Metrics.LastAckLatency->Set((TInstant::Now() - ackInfo.StartTime).MilliSeconds());
                Self.Metrics.InFlyData->Dec();
                Self.FreeSpace += ackInfo.MessageSize;
                ui64 seqNo = ackInfo.SeqNo;        // use seqNo stored on our side because without deduplication we do not specify SeqNo on Write().
                LOG_T(Self.LogPrefix << "Ack seq no (from WaitingAcks) " << seqNo);
                Self.WaitingAcks.pop();

                if (!Self.DeferredCheckpoints.empty() && std::get<0>(Self.DeferredCheckpoints.front()) == seqNo) {
                    Self.ConfirmedSeqNo = seqNo;
                    const auto& checkpoint = std::get<1>(Self.DeferredCheckpoints.front());
                    LOG_D(Self.LogPrefix << CheckpointLogString(checkpoint) << "Send a deferred checkpoint, seqNo: " << seqNo);
                    Self.Callbacks->OnAsyncOutputStateSaved(Self.BuildState(checkpoint), Self.OutputIndex, checkpoint);
                    Self.DeferredCheckpoints.pop();
                    Self.Metrics.InFlyCheckpoints->Dec();
                }
            }
            Self.ConfirmedSeqNo = ev.Acks.back().SeqNo;

            return std::nullopt;
        }

        TDqPqWriteActor& Self;
    };

    void CheckFinished() {
        if (Finished && Buffer.empty() && WaitingAcks.empty()) {
            SINK_LOG_T("Notify PQ sink finished");
            Callbacks->OnAsyncOutputFinished(OutputIndex);
        }
    }
*/
};

std::pair<IDqComputeActorAsyncOutput*, IActor*> CreateDqPqWriteActor(
    NPq::NProto::TDqPqTopicSink&& settings,
    ui64 outputIndex,
    TCollectStatsLevel statsLevel,
    TTxId txId,
    ui64 taskId,
    const THashMap<TString, TString>& secureParams,
    NYdb::TDriver driver,
    IStructuredTokenCredentialsFactory::TPtr credentialsFactory,
    IDqComputeActorAsyncOutput::ICallbacks* callbacks,
    const ::NMonitoring::TDynamicCounterPtr& counters,
    IPqStaticGateway::TPtr pqGateway,
    bool enableStreamingQueriesCounters,
    i64 freeSpace,
    i64 currentExecutionGeneration,
    bool enableStreamingQueriesPqSinkDeduplicationFeatureFlag)
{
    const TString& tokenName = settings.GetToken().GetName();
    const TString token = secureParams.Value(tokenName, TString());
    const bool addBearerToToken = settings.GetAddBearerToToken();

    TDqPqWriteActor* actor = new TDqPqWriteActor(
        outputIndex,
        statsLevel,
        txId,
        taskId,
        std::move(settings),
        std::move(driver),
        credentialsFactory->Create(token, addBearerToToken),
        callbacks,
        counters,
        freeSpace,
        currentExecutionGeneration,
        pqGateway,
        enableStreamingQueriesCounters,
        enableStreamingQueriesPqSinkDeduplicationFeatureFlag);
    return {actor, actor};
}

void RegisterDqPqWriteActorFactory(TDqAsyncIoFactory& factory, NYdb::TDriver driver, IStructuredTokenCredentialsFactory::TPtr credentialsFactory, const IPqStaticGateway::TPtr& pqGateway, const ::NMonitoring::TDynamicCounterPtr& counters, bool enableStreamingQueriesCounters, bool enableStreamingQueriesPqSinkDeduplicationFeatureFlag) {
    factory.RegisterSink<NPq::NProto::TDqPqTopicSink>("PqSink",
        [driver = std::move(driver), credentialsFactory = std::move(credentialsFactory), counters, pqGateway, enableStreamingQueriesCounters, enableStreamingQueriesPqSinkDeduplicationFeatureFlag](
            NPq::NProto::TDqPqTopicSink&& settings,
            IDqAsyncIoFactory::TSinkArguments&& args)
        {
            auto txId = args.TxId;
            if (const auto it = args.TaskParams.find("query_path"); it != args.TaskParams.end()) {
                txId = it->second;
            }

            i64 currentExecutionGeneration = 0;
            if (const auto it = args.TaskParams.find("current_execution_generation"); it != args.TaskParams.end()) {
                currentExecutionGeneration = FromString<i64>(it->second);
            }

            NLwTraceMonPage::ProbeRegistry().AddProbesList(LWTRACE_GET_PROBES(DQ_PQ_PROVIDER));
            return CreateDqPqWriteActor(
                std::move(settings),
                args.OutputIndex,
                args.StatsLevel,
                txId,
                args.TaskId,
                args.SecureParams,
                driver,
                credentialsFactory,
                args.Callback,
                counters ? counters : args.TaskCounters,
                pqGateway,
                enableStreamingQueriesCounters,
                DqPqDefaultFreeSpace,
                currentExecutionGeneration,
                enableStreamingQueriesPqSinkDeduplicationFeatureFlag
            );
        });
}

} // namespace NYql::NDq
