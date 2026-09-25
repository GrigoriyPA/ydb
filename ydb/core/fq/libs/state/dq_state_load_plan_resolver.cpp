#include "dq_state_load_plan.h"

#include <ydb/core/fq/libs/checkpointing/events/events.h>

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor.h>
#include <yql/essentials/public/issue/protos/issue_id.pb.h>
#include <yql/essentials/utils/yql_panic.h>
#include <ydb/library/yql/providers/pq/proto/dq_io_state.pb.h>

#include <algorithm>

namespace NFq {
namespace {

// Read checkpoints and validate retained history before publishing any task plan.
// FORCE selects a fallback for the entire graph if either step fails.
class TStateLoadPlanResolverActor final : public NActors::TActorBootstrapped<TStateLoadPlanResolverActor> {
public:
    TStateLoadPlanResolverActor(
        google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> src,
        google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> dst,
        TStateLoadPlanResolverSettings settings)
        : Src(std::move(src))
        , Dst(std::move(dst))
        , Settings(std::move(settings))
    {}

    void Bootstrap() {
        if (Settings.OutputStartTimeUs) {
            Become(&TThis::StateWork);
            Schedule(TDuration::Seconds(30), new NActors::TEvents::TEvWakeup());
            try {
                if (MakeOutputStartTimeReplayPlan(Dst, *Settings.OutputStartTimeUs, Plan, Issues, Settings.UseSourceDisposition)) {
                    if (Settings.UseSourceDisposition) {
                        // READ_FROM explicitly chooses the input range instead of a calculated rewind.
                        Finish(true);
                    } else {
                        ValidateHistory();
                    }
                } else {
                    ReplayFailed();
                }
            } catch (const std::exception& e) {
                OnException(e);
            }
            return;
        }
        if (!Settings.Replay) {
            Finish(MakeFallbackPlan());
            return;
        }
        for (const auto& task : Src) {
            TaskIds.push_back(task.GetId());
        }
        std::sort(TaskIds.begin(), TaskIds.end());
        Become(&TThis::StateWork);
        Schedule(TDuration::Seconds(30), new NActors::TEvents::TEvWakeup());
        Send(Settings.StorageProxy, new NYql::NDq::TEvDqCompute::TEvGetTaskState(
            Settings.GraphId, TaskIds, Settings.Checkpoint, Settings.CoordinatorGeneration));
    }

private:
    void Registered(NActors::TActorSystem* system, const NActors::TActorId& owner) override {
        TActorBootstrapped::Registered(system, owner);
        Owner = owner;
    }

    struct TEvPartitionDescription : NActors::TEventLocal<TEvPartitionDescription, EventSpaceBegin(NActors::TEvents::ES_PRIVATE) + 1> {
        size_t Index;
        NYdb::NTopic::TAsyncDescribePartitionResult Future;
        TEvPartitionDescription(size_t index, NYdb::NTopic::TAsyncDescribePartitionResult future)
            : Index(index), Future(std::move(future)) {}
    };
    struct TEvReadReady : NActors::TEventLocal<TEvReadReady, EventSpaceBegin(NActors::TEvents::ES_PRIVATE) + 2> {
        size_t Index;
        explicit TEvReadReady(size_t index) : Index(index) {}
    };
    struct TProbe {
        NYql::NPq::NProto::TDqPqTopicSource Source;
        ui64 Partition;
        ui64 ReadFromUs;
        NYql::ITopicClient::TPtr Client;
        std::shared_ptr<NYdb::NTopic::IReadSession> Session;
        ui64 StartOffset = 0;
        bool Done = false;
    };

    STRICT_STFUNC_EXC(StateWork,
        hFunc(NYql::NDq::TEvDqCompute::TEvGetTaskStateResult, Handle)
        hFunc(TEvPartitionDescription, Handle)
        hFunc(TEvReadReady, Handle)
        sFunc(NActors::TEvents::TEvWakeup, Timeout)
        sFunc(NActors::TEvents::TEvPoison, PassAway)
        , ExceptionFunc(std::exception, OnException)
    )

    void Handle(NYql::NDq::TEvDqCompute::TEvGetTaskStateResult::TPtr& ev) {
        bool success = false;
        if (!ev->Get()->Issues.Empty()) {
            Issues.AddIssues(ev->Get()->Issues);
        } else if (ev->Get()->States.size() != TaskIds.size()) {
            Issues.AddIssue("Incomplete checkpoint while preparing history replay");
        } else {
            TCheckpointTaskStates states;
            for (size_t i = 0; i < TaskIds.size(); ++i) {
                states.emplace(TaskIds[i], std::move(ev->Get()->States[i]));
            }
            success = MakeHistoryReplayPlan(Src, Dst, states, Plan, Issues);
        }
        if (success) {
            ValidateHistory();
        } else {
            ReplayFailed();
        }
    }

    void ReplayFailed() {
        bool success = false;
        if (Settings.Force && !Settings.OutputStartTimeUs) {
            NYql::TIssue warning("History replay is unavailable; FORCE=true resumes with the requested streaming disposition");
            warning.SetCode(NYql::TIssuesIds::WARNING, NYql::TSeverityIds::S_WARNING);
            for (const auto& issue : Issues) {
                auto detail = MakeIntrusive<NYql::TIssue>(issue);
                detail->SetCode(NYql::TIssuesIds::WARNING, NYql::TSeverityIds::S_WARNING);
                warning.AddSubIssue(detail);
            }
            Issues.Clear();
            Issues.AddIssue(std::move(warning));
            success = MakeFallbackPlan();
        }
        Finish(success);
    }

    void ValidateHistory() {
        YQL_ENSURE(Settings.TopicClientFactory, "Topic history validation is unavailable");
        for (const auto& task : Dst) {
            for (const auto& sourcePlan : Plan.at(task.GetId()).GetSources()) {
                NYql::NPq::NProto::TDqPqTopicSource source;
                YQL_ENSURE(task.GetInputs(sourcePlan.GetInputIndex()).GetSource().GetSettings().UnpackTo(&source),
                    "Invalid topic source for history validation");
                NYql::NPq::NProto::TDqPqTopicSourceState state;
                YQL_ENSURE(state.ParseFromString(sourcePlan.GetState()), "Invalid replay source state");
                auto client = Settings.TopicClientFactory(source);
                for (const auto& partition : state.GetPartitions()) {
                    Probes.push_back({source, partition.GetPartition(), state.GetStartingMessageTimestampMs() * 1000, client, {}});
                }
            }
        }
        YQL_ENSURE(!Probes.empty(), "No topic partitions to replay");
        for (size_t index = 0; index < Probes.size(); ++index) {
            auto& probe = Probes[index];
            probe.Client->DescribePartition(probe.Source.GetTopicPath(), probe.Partition,
                NYdb::NTopic::TDescribePartitionSettings().IncludeStats(true)).Subscribe(
                [system = ActorContext().ActorSystem(), self = SelfId(), index](const auto& future) {
                    system->Send(self, new TEvPartitionDescription(index, future));
                });
        }
    }

    void Handle(TEvPartitionDescription::TPtr& ev) {
        const auto index = ev->Get()->Index;
        auto& probe = Probes[index];
        const auto result = ev->Get()->Future.ExtractValue();
        YQL_ENSURE(result.IsSuccess(), "Cannot check topic history: " << result.GetIssues().ToOneLineString());
        const auto& stats = result.GetPartitionDescription().GetPartition().GetPartitionStats();
        YQL_ENSURE(stats, "Topic partition statistics are unavailable");
        probe.StartOffset = stats->GetStartOffset();
        if (!probe.StartOffset || (probe.StartOffset == stats->GetEndOffset()
                && stats->GetLastWriteTime() && stats->GetLastWriteTime().MicroSeconds() < probe.ReadFromUs)) {
            CompleteProbe(index);
            return;
        }
        YQL_ENSURE(probe.StartOffset < stats->GetEndOffset(),
            "Required history has expired for topic " << probe.Source.GetTopicPath() << " partition " << probe.Partition);
        // Timestamp seeks silently round up to the first retained message. Read
        // that message explicitly to prove the requested history is available.
        probe.Session = probe.Client->CreateReadSession(NYdb::NTopic::TReadSessionSettings()
            .WithoutConsumer().MaxMemoryUsageBytes(1_MB)
            .AppendTopics(NYdb::NTopic::TTopicReadSettings(probe.Source.GetTopicPath()).AppendPartitionIds(probe.Partition)));
        WaitForProbe(index);
    }

    void WaitForProbe(size_t index) {
        Probes[index].Session->WaitEvent().Subscribe([system = ActorContext().ActorSystem(), self = SelfId(), index](const auto&) {
            system->Send(self, new TEvReadReady(index));
        });
    }

    void Handle(TEvReadReady::TPtr& ev) {
        const auto index = ev->Get()->Index;
        auto& probe = Probes[index];
        for (auto& event : probe.Session->GetEvents(false)) {
            using TEvent = NYdb::NTopic::TReadSessionEvent;
            if (auto* start = std::get_if<TEvent::TStartPartitionSessionEvent>(&event)) {
                start->Confirm(probe.StartOffset, std::nullopt, probe.StartOffset + 1);
            } else if (auto* data = std::get_if<TEvent::TDataReceivedEvent>(&event)) {
                if (data->GetMessages().empty()) {
                    continue;
                }
                const auto& first = data->GetMessages().front();
                YQL_ENSURE(first.GetWriteTime().MicroSeconds() <= probe.ReadFromUs,
                    "Required history has expired for topic " << probe.Source.GetTopicPath() << " partition " << probe.Partition);
                CompleteProbe(index);
                return;
            } else if (auto* stop = std::get_if<TEvent::TStopPartitionSessionEvent>(&event)) {
                stop->Confirm();
            } else if (auto* closed = std::get_if<NYdb::NTopic::TSessionClosedEvent>(&event)) {
                ythrow yexception() << "Cannot check topic history: " << closed->DebugString();
            }
        }
        WaitForProbe(index);
    }

    void CompleteProbe(size_t index) {
        auto& probe = Probes[index];
        probe.Done = true;
        if (probe.Session) {
            probe.Session->Close(TDuration::Zero());
        }
        if (++CompletedProbes == Probes.size()) {
            Finish(true);
        }
    }

    void Timeout() {
        Issues.AddIssue("Timed out preparing history replay");
        ReplayFailed();
    }

    void OnException(const std::exception& e) {
        Issues.AddIssue(NYql::TIssue(TStringBuilder() << "Cannot prepare history replay: " << e.what()));
        ReplayFailed();
    }

    void PassAway() override {
        for (auto& probe : Probes) {
            if (probe.Session && !probe.Done) {
                probe.Session->Close(TDuration::Zero());
            }
        }
        TActorBootstrapped::PassAway();
    }

    bool MakeFallbackPlan() {
        Plan.clear();
        if (Settings.ContinueFromOffsets) {
            return MakeContinueFromStreamingOffsetsPlan(Src, Dst, Settings.Force, Plan, Issues);
        }
        for (const auto& task : Dst) {
            Plan[task.GetId()].SetStateType(NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY);
        }
        return true;
    }

    void Finish(bool success) {
        if (!success) {
            Plan.clear();
        }
        Send(Owner, new TEvCheckpointCoordinator::TEvPrepareStateLoadPlanResult(success, std::move(Plan), std::move(Issues)));
        PassAway();
    }

    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> Src;
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> Dst;
    const TStateLoadPlanResolverSettings Settings;
    NActors::TActorId Owner;
    std::vector<ui64> TaskIds;
    TStateLoadPlan Plan;
    NYql::TIssues Issues;
    TVector<TProbe> Probes;
    size_t CompletedProbes = 0;
};

} // namespace

NActors::IActor* CreateStateLoadPlanResolver(
    google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> src,
    google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> dst,
    TStateLoadPlanResolverSettings settings)
{
    return new TStateLoadPlanResolverActor(std::move(src), std::move(dst), std::move(settings));
}

} // namespace NFq
