#include "dq_state_load_plan.h"

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
    using TDqTasks = google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>;
    using TTopicsMapping = THashMap<TTopic, TTopicMappingInfo, TTopic::THash>;

public:
    TStateLoadPlanResolverActor(const TDqTasks& src, const TDqTasks& dst, const bool force)
        : Src(src)
        , Dst(dst)
        , Force(force)
    {}

    static constexpr char ActorName[] = "DQ_CHECKPOINT_STATE_LOAD_PLAN_RESOLVER";

    void Bootstrap() {
        YDB_LOG_INFO("Bootstrap.",
            {"logPrefix", LogPrefix()});

        MakeContinueFromStreamingOffsetsPlan();

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

    static bool IsTopicInput(const NYql::NDqProto::TTaskInput& taskInput) {
        return taskInput.GetTypeCase() == NYql::NDqProto::TTaskInput::kSource && taskInput.GetSource().GetType() == "PqSource";
    }

    static NDqProto::NDqStateLoadPlan::TSourcePlan& FindSourcePlan(NDqProto::NDqStateLoadPlan::TTaskPlan& taskPlan, const ui64 inputIndex) {
        for (auto& plan : *taskPlan.MutableSources()) {
            if (plan.GetInputIndex() == inputIndex) {
                return plan;
            }
        }
        Y_VALIDATE(false, "Source plan for input index " << inputIndex << " was not found");
    }

    static void InitForeignPlan(const NYql::NDqProto::TDqTask& task, NDqProto::NDqStateLoadPlan::TTaskPlan& taskPlan) {
        taskPlan.SetStateType(NDqProto::NDqStateLoadPlan::STATE_TYPE_FOREIGN);
        taskPlan.MutableProgram()->SetStateType(NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY);

        for (ui64 inputIndex = 0; inputIndex < task.InputsSize(); ++inputIndex) {
            const auto& taskInput = task.GetInputs(inputIndex);
            if (taskInput.GetTypeCase() == NYql::NDqProto::TTaskInput::kSource) {
                auto& sourcePlan = *taskPlan.AddSources();
                sourcePlan.SetStateType(NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY);
                sourcePlan.SetInputIndex(inputIndex);
            }
        }

        for (ui64 outputIndex = 0; outputIndex < task.OutputsSize(); ++outputIndex) {
            const auto& taskOutput = task.GetOutputs(outputIndex);
            if (taskOutput.GetTypeCase() == NYql::NDqProto::TTaskOutput::kSink) {
                auto& sinkPlan = *taskPlan.AddSinks();
                sinkPlan.SetStateType(NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY);
                sinkPlan.SetOutputIndex(outputIndex);
            }
        }
    }

    static void AddToMapping(
        const NYql::NPq::NProto::TDqPqTopicSource& srcDesc,
        const std::vector<NPq::TTopicPartitionsSet>& partitionsSets,
        const ui64 taskId,
        const ui64 inputIndex,
        TTopicsMapping& mapping)
    {
        TTopicMappingInfo& info = mapping[TTopic{srcDesc.GetDatabaseId(), srcDesc.GetDatabase(), srcDesc.GetTopicPath()}];
        for (const auto& partitionsSet : partitionsSets) {
            ui64 currentPartition = partitionsSet.EachTopicPartitionGroupId;
            do {
                info.PartitionsMapping.emplace(currentPartition, TTaskSource{taskId, inputIndex});
                currentPartition += partitionsSet.DqPartitionsCount;
            } while (currentPartition < partitionsSet.TopicPartitionsCount);
        }
    }

    bool ParseTopicInput(
        const NYql::NDqProto::TDqTask& task,
        const NYql::NDqProto::TTaskInput& taskInput,
        const ui64 inputIndex,
        const bool isSourceGraph,
        NYql::NPq::NProto::TDqPqTopicSource& srcDesc,
        std::vector<NPq::TTopicPartitionsSet>& partitionsSets)
    {
        const char* queryKindStr = isSourceGraph ? "source" : "destination";

        const auto& settingsAny = taskInput.GetSource().GetSettings();
        if (!settingsAny.Is<NYql::NPq::NProto::TDqPqTopicSource>()) {
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
        for (const auto& task : Src) {
            for (ui64 inputIndex = 0; inputIndex < task.InputsSize(); ++inputIndex) {
                const auto& taskInput = task.GetInputs(inputIndex);
                if (IsTopicInput(taskInput)) {
                    NYql::NPq::NProto::TDqPqTopicSource srcDesc;
                    std::vector<NPq::TTopicPartitionsSet> partitionsSets;
                    if (ParseTopicInput(task, taskInput, inputIndex, /* isSourceGraph */ true, srcDesc, partitionsSets)) {
                        AddToMapping(srcDesc, partitionsSets, task.GetId(), inputIndex, srcMapping);
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
                    NYql::NPq::NProto::TDqPqTopicSource srcDesc;
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

    void RaiseIssue(const TString& message, const TString& forceMessage = {}) {
        TIssue issue(TStringBuilder() << message << ". " << (Force && forceMessage ? forceMessage : "Use force mode to ignore this issue."));

        if (Force) {
            issue.SetCode(TIssuesIds::WARNING, TSeverityIds::S_WARNING);
        } else {
            Result = false;
        }

        Issues.AddIssue(issue);
    }

    void Finish(const TString& message) {
        Finish({NYql::TIssue(message)});
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
    const bool Force = false;
    TActorId Owner;
    THashMap<ui64, NDqProto::NDqStateLoadPlan::TTaskPlan> Plan;
    TIssues Issues;
    bool Result = true;
};

} // anonymous namespace

IActor* CreateStateLoadPlanResolver(const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& src, const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& dst, bool force) {
    return new TStateLoadPlanResolverActor(src, dst, force);
}

} // namespace NYql::NDq
