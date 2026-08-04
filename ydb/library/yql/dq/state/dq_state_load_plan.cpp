#include "dq_state_load_plan.h"
#include "dq_stage_state_recovery_info.h"

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/log.h>
#include <ydb/library/services/services.pb.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor.h>
#include <ydb/library/yql/dq/proto/dq_state_load_plan.pb.h>
#include <ydb/library/yql/dq/proto/dq_tasks.pb.h>
#include <ydb/library/yql/providers/pq/proto/dq_io.pb.h>
#include <ydb/library/yql/providers/pq/task_meta/task_meta.h>
#include <ydb/library/yverify_stream/yverify_stream.h>

#include <yql/essentials/public/issue/protos/issue_id.pb.h>
#include <yql/essentials/public/issue/yql_issue.h>

#include <google/protobuf/util/time_util.h>

#include <util/digest/multi.h>
#include <util/generic/hash.h>
#include <util/generic/hash_multi_map.h>
#include <util/generic/hash_set.h>
#include <util/generic/string.h>
#include <util/string/builder.h>

#define YDB_LOG_THIS_FILE_COMPONENT ::NKikimrServices::KQP_COMPUTE

namespace NYql::NDq {

using namespace NActors;

namespace {

class TStateLoadPlanResolverActor final : public TActorBootstrapped<TStateLoadPlanResolverActor>, public IActorExceptionHandler {
    struct TTopic {
        TString DatabaseId;
        TString Database;
        TString TopicPath;

        bool operator==(const TTopic& t) const {
            return DatabaseId == t.DatabaseId && Database == t.Database && TopicPath == t.TopicPath;
        }

        struct THash {
            size_t operator()(const TTopic& t) const {
                return MultiHash(t.DatabaseId, t.Database, t.TopicPath);
            }
        };
    };

    struct TTaskSource {
        ui64 TaskId = 0;
        ui64 InputIndex = 0;

        bool operator==(const TTaskSource& t) const {
            return TaskId == t.TaskId && InputIndex == t.InputIndex;
        }

        struct THash {
            size_t operator()(const TTaskSource& t) const {
                return ::THash<std::tuple<ui64, ui64>>()(std::tie(t.TaskId, t.InputIndex));
            }
        };
    };

    struct TTopicMappingInfo {
        // Task can have multiple sources for one partition, so multimap.
        THashMultiMap<ui64, TTaskSource> PartitionsMapping;
        bool Used = false;
    };

    using TBase = TActorBootstrapped<TStateLoadPlanResolverActor>;
    using TDqTasks = google::protobuf::RepeatedPtrField<NDqProto::TDqTask>;
    using TTopicsMapping = THashMap<TTopic, TTopicMappingInfo, TTopic::THash>;

public:
    TStateLoadPlanResolverActor(TDqTasks src, TDqTasks dst, TStateLoadPlanResolverSettings settings)
        : Src(std::move(src))
        , Dst(std::move(dst))
        , Settings(std::move(settings))
    {}

    static constexpr char ActorName[] = "DQ_CHECKPOINT_STATE_LOAD_PLAN_RESOLVER";

    void Bootstrap() {
        YDB_LOG_INFO("Bootstrap, calculating basic reading offsets.",
            {"logPrefix", LogPrefix()});
        MakeContinueFromStreamingOffsetsPlan();

        YDB_LOG_INFO("Bootstrap, calculating timestamps for state recovery.",
            {"logPrefix", LogPrefix()});
        CalculateStateRecoveryTimestamp();

        Finish();
    }

private:
    void Registered(TActorSystem* sys, const TActorId& owner) final {
        TBase::Registered(sys, owner);
        Owner = owner;
    }

    bool OnUnhandledException(const std::exception& e) final {
        Finish(TStringBuilder() << "Unhandled exception: " << e.what());
        return true;
    }

    static bool IsTopicInput(const NDqProto::TTaskInput& taskInput) {
        return taskInput.GetTypeCase() == NDqProto::TTaskInput::kSource && taskInput.GetSource().GetType() == "PqSource";
    }

    static NDqProto::NDqStateLoadPlan::TSourcePlan& FindSourcePlan(NDqProto::NDqStateLoadPlan::TTaskPlan& taskPlan, const ui64 inputIndex) {
        for (auto& plan : *taskPlan.MutableSources()) {
            if (plan.GetInputIndex() == inputIndex) {
                return plan;
            }
        }
        Y_VALIDATE(false, "Source plan for input index " << inputIndex << " was not found");
    }

    static void InitForeignPlan(const NDqProto::TDqTask& task, NDqProto::NDqStateLoadPlan::TTaskPlan& taskPlan) {
        taskPlan.SetStateType(NDqProto::NDqStateLoadPlan::STATE_TYPE_FOREIGN);
        taskPlan.MutableProgram()->SetStateType(NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY);

        for (ui64 inputIndex = 0; inputIndex < task.InputsSize(); ++inputIndex) {
            const auto& taskInput = task.GetInputs(inputIndex);
            if (taskInput.GetTypeCase() == NDqProto::TTaskInput::kSource) {
                auto& sourcePlan = *taskPlan.AddSources();
                sourcePlan.SetStateType(NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY);
                sourcePlan.SetInputIndex(inputIndex);
            }
        }

        for (ui64 outputIndex = 0; outputIndex < task.OutputsSize(); ++outputIndex) {
            const auto& taskOutput = task.GetOutputs(outputIndex);
            if (taskOutput.GetTypeCase() == NDqProto::TTaskOutput::kSink) {
                auto& sinkPlan = *taskPlan.AddSinks();
                sinkPlan.SetStateType(NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY);
                sinkPlan.SetOutputIndex(outputIndex);
            }
        }
    }

    static void AddToMapping(
        const NPq::NProto::TDqPqTopicSource& srcDesc,
        const std::vector<NPq::TTopicPartitionsSet>& partitionsSets,
        const ui64 taskId,
        const ui64 inputIndex,
        TTopicsMapping& mapping)
    {
        TTopicMappingInfo& info = mapping[TTopic{srcDesc.GetDatabaseId(), srcDesc.GetDatabase(), srcDesc.GetTopicPath()}];
        for (const auto& partitionsSet : partitionsSets) {
            ui64 currentPartition = partitionsSet.EachTopicPartitionGroupId;
            do {
                info.PartitionsMapping.emplace(currentPartition, TTaskSource{.TaskId = taskId, .InputIndex = inputIndex});
                currentPartition += partitionsSet.DqPartitionsCount;
            } while (currentPartition < partitionsSet.TopicPartitionsCount);
        }
    }

    bool ParseTopicInput(
        const NDqProto::TDqTask& task,
        const NDqProto::TTaskInput& taskInput,
        const ui64 inputIndex,
        const bool isSourceGraph,
        NPq::NProto::TDqPqTopicSource& srcDesc,
        std::vector<NPq::TTopicPartitionsSet>& partitionsSets)
    {
        const char* queryKindStr = isSourceGraph ? "source" : "destination";

        const auto& settingsAny = taskInput.GetSource().GetSettings();
        if (!settingsAny.Is<NPq::NProto::TDqPqTopicSource>()) {
            RaiseIssue(TStringBuilder() << "Can't read " << queryKindStr << " query params: input " << inputIndex << " of task " << task.GetId() << " has incorrect type");
            return false;
        }

        if (!settingsAny.UnpackTo(&srcDesc)) {
            RaiseIssue(TStringBuilder() << "Can't read " << queryKindStr << " query params: failed to unpack input " << inputIndex << " of task " << task.GetId());
            return false;
        }

        partitionsSets = NPq::GetTopicPartitionsSets(task);
        if (partitionsSets.empty()) {
            RaiseIssue(TStringBuilder() << "Can't read " << queryKindStr << " query params: failed to load partitions of topic `" << srcDesc.GetTopicPath() << "` from input " << inputIndex << " of task " << task.GetId());
            return false;
        }

        return true;
    }

    void MakeContinueFromStreamingOffsetsPlan() {
        // Build src mapping
        TTopicsMapping srcMapping;
        for (ui64 i = 0; i < static_cast<ui64>(Src.size()); ++i) {
            const auto& task = Src[i];
            bool savedToIndexMap = false;

            for (ui64 inputIndex = 0; inputIndex < task.InputsSize(); ++inputIndex) {
                const auto& taskInput = task.GetInputs(inputIndex);
                if (!IsTopicInput(taskInput)) {
                    continue;
                }

                NPq::NProto::TDqPqTopicSource srcDesc;
                std::vector<NPq::TTopicPartitionsSet> partitionsSets;
                if (ParseTopicInput(task, taskInput, inputIndex, /* isSourceGraph */ true, srcDesc, partitionsSets)) {
                    const auto taskId = task.GetId();
                    AddToMapping(srcDesc, partitionsSets, taskId, inputIndex, srcMapping);

                    if (!savedToIndexMap) {
                        savedToIndexMap = true;
                        Y_VALIDATE(SrcTaskIndices.emplace(taskId, i).second, "Found ambiguous task ID: " << taskId);
                    }
                }
            }
        }

        // Watch dst query and build plan
        for (const auto& task : Dst) {
            NDqProto::NDqStateLoadPlan::TTaskPlan& taskPlan = Plan[task.GetId()];
            taskPlan.SetStateType(NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY); // default if no topic sources
            bool foreignStatePlanInitted = false;

            for (ui64 inputIndex = 0; inputIndex < task.InputsSize(); ++inputIndex) {
                const auto& taskInput = task.GetInputs(inputIndex);
                if (IsTopicInput(taskInput)) {
                    NPq::NProto::TDqPqTopicSource srcDesc;
                    std::vector<NPq::TTopicPartitionsSet> partitionsSets;
                    if (!ParseTopicInput(task, taskInput, inputIndex, /* isSourceGraph */ false, srcDesc, partitionsSets)) {
                        continue;
                    }

                    const auto mappingInfoIt = srcMapping.find(TTopic{srcDesc.GetDatabaseId(), srcDesc.GetDatabase(), srcDesc.GetTopicPath()});
                    if (mappingInfoIt == srcMapping.end()) {
                        RaiseIssue(TStringBuilder() << "Topic `" << srcDesc.GetTopicPath() << "` is not found in previous query", "Query will use fresh offsets for its partitions");
                        continue;
                    }

                    TTopicMappingInfo& mappingInfo = mappingInfoIt->second;
                    mappingInfo.Used = true;

                    THashSet<TTaskSource, TTaskSource::THash> tasksSet;

                    // Process all partitions
                    for (const auto& partitionsSet : partitionsSets) {
                        ui64 currentPartition = partitionsSet.EachTopicPartitionGroupId;
                        do {
                            auto [taskBegin, taskEnd] = mappingInfo.PartitionsMapping.equal_range(currentPartition);
                            if (taskBegin == taskEnd) {
                                RaiseIssue(TStringBuilder() << "Topic `" << srcDesc.GetTopicPath() << "` partition " << currentPartition << " is not found in previous query", "Query will use fresh offsets for it");
                            } else {
                                if (std::distance(taskBegin, taskEnd) > 1) {
                                    RaiseIssue(TStringBuilder() << "Topic `" << srcDesc.GetTopicPath() << "` partition " << currentPartition << " has ambiguous offsets source in previous query checkpoint", "Query will use minimum offset to avoid skipping data");
                                }
                                for (; taskBegin != taskEnd; ++taskBegin) {
                                    tasksSet.insert(taskBegin->second);
                                }
                            }
                            currentPartition += partitionsSet.DqPartitionsCount;
                        } while (currentPartition < partitionsSet.TopicPartitionsCount);
                    }

                    if (!tasksSet.empty()) {
                        if (!foreignStatePlanInitted) {
                            foreignStatePlanInitted = true;
                            InitForeignPlan(task, taskPlan);
                        }

                        auto& sourcePlan = FindSourcePlan(taskPlan, inputIndex);
                        sourcePlan.SetStateType(NDqProto::NDqStateLoadPlan::STATE_TYPE_FOREIGN);
                        for (const auto& taskSource : tasksSet) {
                            auto& taskSourceProto = *sourcePlan.AddForeignTasksSources();
                            taskSourceProto.SetTaskId(taskSource.TaskId);
                            taskSourceProto.SetInputIndex(taskSource.InputIndex);
                        }
                    }
                }
            }
        }

        for (const auto& [topic, mappingInfo] : srcMapping) {
            if (!mappingInfo.Used) {
                RaiseIssue(TStringBuilder() << "Topic `" << topic.TopicPath << "` is read in previous query but is not read in new query", "Reading offsets will be lost in next checkpoint");
            }
        }
    }

    const TStageStateRecoveryInfo& GetTaskRecoveryInfo(const ui64 taskId) {
        const auto taskIdxIt = SrcTaskIndices.find(taskId);
        Y_VALIDATE(taskIdxIt != SrcTaskIndices.end(), "Task " << taskId << " is not found in previous query");

        if (const auto recoveryInfoIt = StageStateRecoveryInfo.find(Src[taskIdxIt->second].GetStageId()); recoveryInfoIt != StageStateRecoveryInfo.end()) {
            return recoveryInfoIt->second;
        }
    }

    // TODO: validate sources + watermark generator
    // TODO: validate sinks
    void CalculateStateRecoveryTimestamp() {
        for (auto& [taskId, taskPlan] : Plan) {
            for (auto& sourcePlan : *taskPlan.MutableSources()) {
                std::optional<TInstant> recoveryTimestamp;

                for (const auto& foreignTaskSource : sourcePlan.GetForeignTasksSources()) {
                    if (const auto recoveryInfo = GetTaskRecoveryInfo(foreignTaskSource.GetTaskId()).Recovery) {
                        if (!recoveryTimestamp) {
                            recoveryTimestamp = recoveryInfo->Timestamp;
                        } else if (*recoveryTimestamp != recoveryInfo->Timestamp) {
                            // TODO: produce common timestamp
                            RaiseIssue(TStringBuilder() << "Input " << sourcePlan.GetInputIndex() << " of task " << taskId << " has ambiguous state recovery timestamp in previous query checkpoint", "Query will use minimum timestamp to avoid skipping data");
                            recoveryTimestamp = std::min(*recoveryTimestamp, recoveryInfo->Timestamp);
                        }
                    }
                }

                if (recoveryTimestamp) {
                    if (recoveryTimestamp->MicroSeconds() <= static_cast<ui64>(google::protobuf::util::TimeUtil::kTimestampMinSeconds * 1000000)) {
                        *sourcePlan.MutableForeignTasksRecoveryTimestamp() = google::protobuf::util::TimeUtil::MicrosecondsToTimestamp(recoveryTimestamp->MicroSeconds());
                    } else {
                        RaiseIssue(TStringBuilder() << "Input " << sourcePlan.GetInputIndex() << " of task " << taskId << " has too large recovery timestamp in previous query checkpoint", "Query will read data from fresh offsets");
                    }
                }
            }
        }
    }

    void RaiseIssue(const TString& message, const TString& forceMessage = {}) {
        TIssue issue(TStringBuilder() << message << ". " << (Settings.Force && forceMessage ? forceMessage : "Use force mode to ignore this issue."));

        if (Settings.Force) {
            issue.SetCode(TIssuesIds::WARNING, TSeverityIds::S_WARNING);
        } else {
            Result = false;
        }

        Issues.AddIssue(issue);
    }

    void Finish(const TString& message) {
        Finish({TIssue(message)});
    }

    void Finish(const TIssues& issues = {}) {
        Issues.AddIssues(issues);

        if (Result) {
            YDB_LOG_INFO("State successfully recovered.",
                {"logPrefix", LogPrefix()},
                {"planSize", Plan.size()});
        } else {
            YDB_LOG_WARN("State recovery failed.",
                {"logPrefix", LogPrefix()},
                {"planSize", Plan.size()},
                {"issues", Issues.ToOneLineString()});
        }

        Send(Owner, new TEvDqCompute::TEvPrepareStateLoadPlanResult(Result, std::move(Plan), std::move(Issues)));
        PassAway();
    }

    const TString LogPrefix() {
        return TStringBuilder() << "[" << ActorName << "] SelfId: " << SelfId() << ". OwnerId: " << Owner << ". ";
    }

    const TDqTasks Src;
    const TDqTasks Dst;
    const TStateLoadPlanResolverSettings Settings;
    TActorId Owner;
    THashMap<ui64, ui64> SrcTaskIndices; // Src task ID -> Index
    THashMap<ui64, TStageStateRecoveryInfo> StageStateRecoveryInfo; // Src stage ID -> Info
    THashMap<ui64, NDqProto::NDqStateLoadPlan::TTaskPlan> Plan; // Dst task ID -> Plan
    TIssues Issues;
    bool Result = true;
};

} // anonymous namespace

IActor* CreateStateLoadPlanResolver(google::protobuf::RepeatedPtrField<NDqProto::TDqTask> src, google::protobuf::RepeatedPtrField<NDqProto::TDqTask> dst, TStateLoadPlanResolverSettings settings) {
    return new TStateLoadPlanResolverActor(std::move(src), std::move(dst), std::move(settings));
}

} // namespace NYql::NDq
