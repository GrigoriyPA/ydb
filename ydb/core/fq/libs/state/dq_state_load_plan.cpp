#include "dq_state_load_plan.h"
#include "dq_stage_state_recovery_info.h"

#include <yql/essentials/minikql/comp_nodes/mkql_saveload.h>
#include <yql/essentials/public/issue/protos/issue_id.pb.h>
#include <yql/essentials/utils/yql_panic.h>
#include <ydb/library/yql/providers/pq/common/yql_names.h>
#include <ydb/library/yql/providers/pq/proto/dq_io.pb.h>
#include <ydb/library/yql/providers/pq/proto/dq_io_state.pb.h>
#include <ydb/library/yql/providers/pq/task_meta/task_meta.h>

#include <util/digest/multi.h>
#include <util/generic/hash.h>
#include <util/generic/hash_multi_map.h>
#include <util/generic/hash_set.h>
#include <util/string/builder.h>

#include <algorithm>

namespace NFq {

namespace {
// Pq specific
// TODO: rewrite this code to not depend on concrete providers (now it is only pq)
struct TTopic {
    TString DatabaseId;
    TString Database;
    TString TopicPath;

    bool operator==(const TTopic& t) const {
        return DatabaseId == t.DatabaseId && Database == t.Database && TopicPath == t.TopicPath;
    }
};

struct TTopicHash {
    size_t operator()(const TTopic& t) const {
        return MultiHash(t.DatabaseId, t.Database, t.TopicPath);
    }
};

struct TTaskSource {
    ui64 TaskId = 0;
    ui64 InputIndex = 0;

    bool operator==(const TTaskSource& t) const {
        return TaskId == t.TaskId && InputIndex == t.InputIndex;
    }
};

struct TTaskSourceHash {
    size_t operator()(const TTaskSource& t) const {
        return THash<std::tuple<ui64, ui64>>()(std::tie(t.TaskId, t.InputIndex));
    }
};

using TPartitionsMapping = THashMultiMap<ui64, TTaskSource>; // Task can have multiple sources for one partition, so multimap.

struct TTopicMappingInfo {
    TPartitionsMapping PartitionsMapping;
    bool Used = false;
};

using TTopicsMapping = THashMap<TTopic, TTopicMappingInfo, TTopicHash>;

// Error in case of normal mode and warning if force one is on.
#define ISSUE(stream)                                                   \
    AddForceWarningOrError(TStringBuilder() << stream, issues, force);  \
    if (!force) {                                                       \
        result = false;                                                 \
    }                                                                   \
    /**/

void AddForceWarningOrError(const TString& message, NYql::TIssues& issues, bool force) {
    NYql::TIssue issue(message);
    if (force) {
        issue.SetCode(NYql::TIssuesIds::WARNING, NYql::TSeverityIds::S_WARNING);
    }
    issues.AddIssue(std::move(issue));
}

bool IsTopicInput(const NYql::NDqProto::TTaskInput& taskInput) {
    return taskInput.GetTypeCase() == NYql::NDqProto::TTaskInput::kSource && taskInput.GetSource().GetType() == NYql::PqSource;
}

bool ParseTopicInput(
    const NYql::NDqProto::TDqTask& task,
    const NYql::NDqProto::TTaskInput& taskInput,
    ui64 inputIndex,
    bool force,
    bool isSourceGraph,
    NYql::NPq::NProto::TDqPqTopicSource& srcDesc,
    std::vector<NYql::NPq::TTopicPartitionsSet>& partitionsSets,
    NYql::TIssues& issues)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
    bool result = true;
#pragma clang diagnostic pop
    const char* queryKindStr = isSourceGraph ? "source" : "destination";
    const google::protobuf::Any& settingsAny = taskInput.GetSource().GetSettings();
    if (!settingsAny.Is<NYql::NPq::NProto::TDqPqTopicSource>()) {
        ISSUE("Can't read " << queryKindStr << " query params: input " << inputIndex << " of task " << task.GetId() << " has incorrect type");
        return false;
    }
    if (!settingsAny.UnpackTo(&srcDesc)) {
        ISSUE("Can't read " << queryKindStr << " query params: failed to unpack input " << inputIndex << " of task " << task.GetId());
        return false;
    }

    partitionsSets = NYql::NPq::GetTopicPartitionsSets(task);
    if (partitionsSets.empty()) {
        ISSUE("Can't read " << queryKindStr << " query params: failed to load partitions of topic `" << srcDesc.GetTopicPath() << "` from input " << inputIndex << " of task " << task.GetId());
        return false;
    }

    return true;
}

void AddToMapping(
    const NYql::NPq::NProto::TDqPqTopicSource& srcDesc,
    const std::vector<NYql::NPq::TTopicPartitionsSet>& partitionsSets,
    ui64 taskId,
    ui64 inputIndex,
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

void InitForeignPlan(const NYql::NDqProto::TDqTask& task, NYql::NDqProto::NDqStateLoadPlan::TTaskPlan& taskPlan) {
    taskPlan.SetStateType(NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_FOREIGN);
    taskPlan.MutableProgram()->SetStateType(NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY);
    for (ui64 inputIndex = 0; inputIndex < task.InputsSize(); ++inputIndex) {
        const NYql::NDqProto::TTaskInput& taskInput = task.GetInputs(inputIndex);
        if (taskInput.GetTypeCase() == NYql::NDqProto::TTaskInput::kSource) {
            NYql::NDqProto::NDqStateLoadPlan::TSourcePlan& sourcePlan = *taskPlan.AddSources();
            sourcePlan.SetStateType(NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY);
            sourcePlan.SetInputIndex(inputIndex);
        }
    }
    for (ui64 outputIndex = 0; outputIndex < task.OutputsSize(); ++outputIndex) {
        const NYql::NDqProto::TTaskOutput& taskOutput = task.GetOutputs(outputIndex);
        if (taskOutput.GetTypeCase() == NYql::NDqProto::TTaskOutput::kSink) {
            NYql::NDqProto::NDqStateLoadPlan::TSinkPlan& sinkPlan = *taskPlan.AddSinks();
            sinkPlan.SetStateType(NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY);
            sinkPlan.SetOutputIndex(outputIndex);
        }
    }
}

NYql::NDqProto::NDqStateLoadPlan::TSourcePlan& FindSourcePlan(NYql::NDqProto::NDqStateLoadPlan::TTaskPlan& taskPlan, ui64 inputIndex) {
    for (NYql::NDqProto::NDqStateLoadPlan::TSourcePlan& plan : *taskPlan.MutableSources()) {
        if (plan.GetInputIndex() == inputIndex) {
            return plan;
        }
    }
    Y_ABORT("Source plan for input index %lu was not found", inputIndex);
}

} // namespace

bool MakeContinueFromStreamingOffsetsPlan(
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& src,
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& dst,
    const bool force,
    THashMap<ui64, NYql::NDqProto::NDqStateLoadPlan::TTaskPlan>& plan,
    NYql::TIssues& issues)
{
#define FORCE_MSG(msg) (force ? ". " msg : ". Use force mode to ignore this issue")

    bool result = true;
    // Build src mapping
    TTopicsMapping srcMapping;
    for (const NYql::NDqProto::TDqTask& task : src) {
        for (ui64 inputIndex = 0; inputIndex < task.InputsSize(); ++inputIndex) {
            const NYql::NDqProto::TTaskInput& taskInput = task.GetInputs(inputIndex);
            if (IsTopicInput(taskInput)) {
                NYql::NPq::NProto::TDqPqTopicSource srcDesc;
                std::vector<NYql::NPq::TTopicPartitionsSet> partitionsSets;
                if (!ParseTopicInput(task, taskInput, inputIndex, force, true, srcDesc, partitionsSets, issues)) {
                    if (!force) {
                        result = false;
                    }
                    continue;
                }

                AddToMapping(srcDesc, partitionsSets, task.GetId(), inputIndex, srcMapping);
            }
        }
    }

    // Watch dst query and build plan
    for (const NYql::NDqProto::TDqTask& task : dst) {
        NYql::NDqProto::NDqStateLoadPlan::TTaskPlan& taskPlan = plan[task.GetId()];
        taskPlan.SetStateType(NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY); // default if no topic sources
        bool foreignStatePlanInited = false;
        for (ui64 inputIndex = 0; inputIndex < task.InputsSize(); ++inputIndex) {
            const NYql::NDqProto::TTaskInput& taskInput = task.GetInputs(inputIndex);
            if (IsTopicInput(taskInput)) {
                NYql::NPq::NProto::TDqPqTopicSource srcDesc;
                std::vector<NYql::NPq::TTopicPartitionsSet> partitionsSets;
                if (!ParseTopicInput(task, taskInput, inputIndex, force, false, srcDesc, partitionsSets, issues)) {
                    if (!force) {
                        result = false;
                    }
                    continue;
                }
                const auto mappingInfoIt = srcMapping.find(TTopic{srcDesc.GetDatabaseId(), srcDesc.GetDatabase(), srcDesc.GetTopicPath()});
                if (mappingInfoIt == srcMapping.end()) {
                    ISSUE("Topic `" << srcDesc.GetTopicPath() << "` is not found in previous query" << FORCE_MSG("Query will use fresh offsets for its partitions"));
                    continue;
                }
                TTopicMappingInfo& mappingInfo = mappingInfoIt->second;
                mappingInfo.Used = true;

                THashSet<TTaskSource, TTaskSourceHash> tasksSet;

                // Process all partitions
                for (const auto& partitionsSet : partitionsSets) {
                    ui64 currentPartition = partitionsSet.EachTopicPartitionGroupId;
                    do {
                        auto [taskBegin, taskEnd] = mappingInfo.PartitionsMapping.equal_range(currentPartition);
                        if (taskBegin == taskEnd) {
                            ISSUE("Topic `" << srcDesc.GetTopicPath() << "` partition " << currentPartition << " is not found in previous query" << FORCE_MSG("Query will use fresh offsets for it"));
                        } else {
                            if (std::distance(taskBegin, taskEnd) > 1) {
                                ISSUE("Topic `" << srcDesc.GetTopicPath() << "` partition " << currentPartition << " has ambiguous offsets source in previous query checkpoint" << FORCE_MSG("Query will use minimum offset to avoid skipping data"));
                            }
                            for (; taskBegin != taskEnd; ++taskBegin) {
                                tasksSet.insert(taskBegin->second);
                            }
                        }
                        currentPartition += partitionsSet.DqPartitionsCount;
                    } while (currentPartition < partitionsSet.TopicPartitionsCount);
                }

                if (!tasksSet.empty()) {
                    if (!foreignStatePlanInited) {
                        foreignStatePlanInited = true;
                        InitForeignPlan(task, taskPlan);
                    }
                    NYql::NDqProto::NDqStateLoadPlan::TSourcePlan& sourcePlan = FindSourcePlan(taskPlan, inputIndex);
                    sourcePlan.SetStateType(NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_FOREIGN);
                    for (const TTaskSource& taskSource : tasksSet) {
                        NYql::NDqProto::NDqStateLoadPlan::TSourcePlan::TForeignTaskSource& taskSourceProto = *sourcePlan.AddForeignTasksSources();
                        taskSourceProto.SetTaskId(taskSource.TaskId);
                        taskSourceProto.SetInputIndex(taskSource.InputIndex);
                    }
                }
            }
        }
    }
    for (const auto& [topic, mappingInfo] : srcMapping) {
        if (!mappingInfo.Used) {
            ISSUE("Topic `" << topic.TopicPath << "` is read in previous query but is not read in new query" << FORCE_MSG("Reading offsets will be lost in next checkpoint"));
        }
    }
    return result;

#undef FORCE_MSG
}

namespace {

struct TReplayPartition {
    TTopic Topic;
    TString Endpoint;
    ui64 Id = 0;

    bool operator==(const TReplayPartition&) const = default;

    struct THash {
        size_t operator()(const TReplayPartition& partition) const {
            return MultiHash(TTopicHash()(partition.Topic), partition.Endpoint, partition.Id);
        }
    };
};

struct TReplaySource {
    ui64 InputIndex = 0;
    NYql::NPq::NProto::TDqPqTopicSource Description;
    TVector<TReplayPartition> Partitions;
};

struct TReplayTask {
    const NYql::NDqProto::TDqTask* Task = nullptr;
    TStageStateRecoveryInfo Info;
    TVector<ui64> Parents;
    TVector<TReplaySource> Sources;
    THashSet<TReplayPartition, TReplayPartition::THash> Ancestors;
    TMaybe<ui64> InputBound;
    TMaybe<ui64> DirectOutputBound;
    bool NeedsEventTimeRewind = false;
    bool WatermarkAvailable = false;
    bool Visiting = false;
    bool Visited = false;
};

class TReplayGraph {
public:
    explicit TReplayGraph(const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& graph) {
        THashMap<ui32, TStageStateRecoveryInfo> stages;
        for (const auto& task : graph) {
            auto [it, inserted] = Tasks.emplace(task.GetId(), TReplayTask{});
            YQL_ENSURE(inserted, "Duplicate task ID in history replay graph");
            auto& info = it->second;
            info.Task = &task;
            auto stage = stages.find(task.GetStageId());
            if (stage == stages.end()) {
                stage = stages.emplace(task.GetStageId(), TStageStateRecoveryInfo::FromProgram(task.GetProgram())).first;
            }
            info.Info = stage->second;
            for (ui64 inputIndex = 0; inputIndex < task.InputsSize(); ++inputIndex) {
                const auto& input = task.GetInputs(inputIndex);
                YQL_ENSURE(!input.HasTransform(), "Input transforms do not support history replay");
                if (input.HasSource()) {
                    YQL_ENSURE(IsTopicInput(input), "History replay requires topic inputs");
                    TReplaySource source;
                    source.InputIndex = inputIndex;
                    std::vector<NYql::NPq::TTopicPartitionsSet> partitionSets;
                    NYql::TIssues issues;
                    YQL_ENSURE(ParseTopicInput(task, input, inputIndex, false, true, source.Description, partitionSets, issues), "Invalid topic input: " << issues.ToOneLineString());
                    YQL_ENSURE(source.Description.GetFederatedClusters().empty(), "Federated topic history replay is not supported");
                    YQL_ENSURE(!source.Description.GetSharedReading(), "Shared topic readers do not support history replay");
                    const TTopic topic{source.Description.GetDatabaseId(), source.Description.GetDatabase(), source.Description.GetTopicPath()};
                    for (const auto& set : partitionSets) {
                        YQL_ENSURE(set.DqPartitionsCount, "Invalid topic partition mapping");
                        for (ui64 p = set.EachTopicPartitionGroupId; p < set.TopicPartitionsCount; p += set.DqPartitionsCount) {
                            source.Partitions.push_back({topic, source.Description.GetEndpoint(), p});
                        }
                    }
                    info.Sources.push_back(std::move(source));
                } else {
                    YQL_ENSURE(input.HasUnionAll(), "History replay requires union-all channels");
                    for (const auto& channel : input.GetChannels()) {
                        info.Parents.push_back(channel.GetSrcTaskId());
                    }
                }
            }
            for (const auto& output : task.GetOutputs()) {
                YQL_ENSURE(!output.HasTransform(), "Output transforms do not support history replay");
                YQL_ENSURE(!output.HasEffects(), "Stateful effects do not support history replay");
            }
        }
        for (const auto& [taskId, task] : Tasks) {
            FindAncestors(taskId);
        }
    }

    TReplayTask& Get(ui64 taskId) {
        const auto it = Tasks.find(taskId);
        YQL_ENSURE(it != Tasks.end(), "Missing task " << taskId << " in history replay graph");
        return it->second;
    }

    // Translate a bound on this task's input through upstream HOP_END values.
    // Every path is visited; shared upstream tasks retain the earliest bound.
    void RequireInput(ui64 taskId, ui64 timeUs) {
        auto& task = Get(taskId);
        const bool alreadyNeedsRewind = task.NeedsEventTimeRewind;
        task.NeedsEventTimeRewind = true;
        if (task.InputBound && *task.InputBound <= timeUs && alreadyNeedsRewind) {
            return;
        }
        task.InputBound = Min(task.InputBound.GetOrElse(timeUs), timeUs);
        for (ui64 parentId : task.Parents) {
            RequireInput(parentId, Get(parentId).Info.InputStartForOutput(*task.InputBound));
        }
    }

    void RequireOutputBefore(ui64 taskId, ui64 timeUs) {
        auto& task = Get(taskId);
        if (task.Info.Hopping) {
            RequireInput(taskId, task.Info.WindowStartBefore(timeUs));
            return;
        }
        task.InputBound = Min(task.InputBound.GetOrElse(timeUs), timeUs);
        for (ui64 parentId : task.Parents) {
            RequireOutputBefore(parentId, timeUs);
        }
    }

    // An explicit output start time is inclusive. Unlike automatic recovery it
    // does not need an overlapping window before a previous graph's frontier.
    void RequireOutputFrom(ui64 taskId, ui64 timeUs) {
        auto& task = Get(taskId);
        if (task.Info.Hopping) {
            RequireInput(taskId, task.Info.InputStartForOutput(timeUs));
            return;
        }
        task.InputBound = Min(task.InputBound.GetOrElse(timeUs), timeUs);
        if (!task.Sources.empty()) {
            task.DirectOutputBound = timeUs;
        }
        for (const auto parentId : task.Parents) {
            RequireOutputFrom(parentId, timeUs);
        }
    }

    THashMap<ui64, TReplayTask> Tasks;

private:
    void FindAncestors(ui64 taskId) {
        auto& task = Get(taskId);
        if (task.Visited) {
            return;
        }
        YQL_ENSURE(!task.Visiting, "Cycle in history replay graph");
        task.Visiting = true;
        for (const auto& source : task.Sources) {
            task.Ancestors.insert(source.Partitions.begin(), source.Partitions.end());
        }
        for (ui64 parentId : task.Parents) {
            FindAncestors(parentId);
            const auto& parent = Get(parentId);
            task.Ancestors.insert(parent.Ancestors.begin(), parent.Ancestors.end());
        }
        task.WatermarkAvailable = task.Info.HasWatermarkGenerator;
        if (!task.WatermarkAvailable && task.Sources.empty()) {
            task.WatermarkAvailable = std::all_of(task.Parents.begin(), task.Parents.end(),
                [&](ui64 parentId) { return Get(parentId).WatermarkAvailable; });
        }
        YQL_ENSURE(!task.Info.Hopping || task.WatermarkAvailable,
            "History replay requires a watermark generator before each hopping operator");
        task.Visiting = false;
        task.Visited = true;
        YQL_ENSURE(!task.Ancestors.empty(), "History replay requires a topic on every input path");
    }
};

struct TReplayPartitionProgress {
    TString Cluster;
    ui64 TimeUs = 0;
};

using TReplayProgress = THashMap<TReplayPartition, TReplayPartitionProgress, TReplayPartition::THash>;

TReplayProgress ReadReplayProgress(TReplayGraph& graph, const TCheckpointTaskStates& states) {
    using namespace NKikimr::NMiniKQL;
    for (auto& [taskId, task] : graph.Tasks) {
        const auto* state = states.FindPtr(taskId);
        YQL_ENSURE(state && state->MiniKqlProgram, "Missing checkpoint state for task " << taskId);
        for (const auto& sink : state->Sinks) {
            YQL_ENSURE(sink.Data.Blob.empty() && !sink.Data.Version, "A sink has state and does not support history replay");
        }
        TStringBuf programState(state->MiniKqlProgram->Data.Blob);
        if (task.Info.Hopping) {
            const auto size = ReadUi64(programState);
            YQL_ENSURE(size != Max<ui64>() && size == programState.size(),
                "Hopping checkpoint is uninitialized or has additional operator state");
            const auto info = THoppingRecoveryState::Read(programState);
            const auto hopTimeUs = task.Info.Hopping->HopTimeUs;
            YQL_ENSURE(info.MinWindowStartIndex <= Max<ui64>() / hopTimeUs, "Hopping recovery time overflow");
            graph.RequireInput(taskId, info.MinWindowStartIndex * hopTimeUs);
        } else {
            YQL_ENSURE(programState.empty(), "Checkpoint contains state outside hopping operators");
        }
    }

    TReplayProgress result;
    for (const auto& [taskId, task] : graph.Tasks) {
        const auto& state = states.at(taskId);
        for (const auto& source : task.Sources) {
            const NYql::NDq::TSourceState* sourceState = nullptr;
            for (const auto& candidate : state.Sources) {
                if (candidate.InputIndex == source.InputIndex) {
                    sourceState = &candidate;
                    break;
                }
            }
            YQL_ENSURE(sourceState, "Missing topic checkpoint for history replay");
            THashMap<ui64, TReplayPartitionProgress> partitions;
            for (const auto& data : sourceState->Data) {
                YQL_ENSURE(data.Version == 1, "Unsupported topic checkpoint version");
                NYql::NPq::NProto::TDqPqTopicSourceState saved;
                YQL_ENSURE(saved.ParseFromString(data.Blob), "Invalid topic checkpoint");
                for (const auto& partition : saved.GetPartitions()) {
                    const auto time = partition.HasLastWriteTimeUs() ? partition.GetLastWriteTimeUs()
                        : saved.GetStartingMessageTimestampMs() * 1000;
                    YQL_ENSURE(partitions.emplace(partition.GetPartition(), TReplayPartitionProgress{partition.GetCluster(), time}).second,
                        "Ambiguous topic partition checkpoint");
                }
            }
            for (const auto& partition : source.Partitions) {
                auto* progress = partitions.FindPtr(partition.Id);
                YQL_ENSURE(progress, "Missing checkpoint for topic partition " << partition.Id);
                // A stateless old path has no event-time frontier in its
                // checkpoint. Replay its whole history instead of treating a
                // message write time as an event-time boundary.
                progress->TimeUs = task.InputBound ? Min(progress->TimeUs, *task.InputBound) : 0;
                YQL_ENSURE(result.emplace(partition, *progress).second,
                    "History replay requires unique inputs in the previous query");
            }
        }
    }
    return result;
}

TStateLoadPlan BuildReplayTaskPlans(const TReplayGraph& graph, const TReplayProgress* progress = nullptr, bool useSourceDisposition = false) {
    using namespace NKikimr::NMiniKQL;
    using namespace NYql::NDqProto::NDqStateLoadPlan;
    TStateLoadPlan plan;
    for (const auto& [taskId, task] : graph.Tasks) {
        YQL_ENSURE(task.InputBound, "Task has no history replay output boundary");
        auto& taskPlan = plan[taskId];
        taskPlan.SetStateType(STATE_TYPE_FOREIGN);
        auto& programPlan = *taskPlan.MutableProgram();
        programPlan.SetStateType(task.Info.Hopping ? STATE_TYPE_FOREIGN : STATE_TYPE_EMPTY);
        if (task.Info.Hopping) {
            TString state;
            const auto hopTimeUs = task.Info.Hopping->HopTimeUs;
            YQL_ENSURE(*task.InputBound % hopTimeUs == 0, "Unaligned hopping recovery time");
            TNodeStateHelper::AddNodeState(state, THoppingRecoveryState::MakeRecoveryState(*task.InputBound / hopTimeUs));
            programPlan.SetState(std::move(state));
        }
        for (const auto& source : task.Sources) {
            auto& sourcePlan = *taskPlan.AddSources();
            sourcePlan.SetInputIndex(source.InputIndex);
            if (useSourceDisposition) {
                // The source's explicit READ_FROM handles timestamp seeks and consumer rewind.
                sourcePlan.SetStateType(STATE_TYPE_EMPTY);
                continue;
            }
            sourcePlan.SetStateType(STATE_TYPE_FOREIGN);
            sourcePlan.SetStateVersion(1);
            NYql::NPq::NProto::TDqPqTopicSourceState state;
            auto& topic = *state.AddTopics();
            topic.SetDatabaseId(source.Description.GetDatabaseId());
            topic.SetDatabase(source.Description.GetDatabase());
            topic.SetTopicPath(source.Description.GetTopicPath());
            topic.SetEndpoint(source.Description.GetEndpoint());
            const auto earlyLimitUs = (progress || task.NeedsEventTimeRewind) ? TDuration::Minutes(5).MicroSeconds() : 0;
            const auto readFromUs = *task.InputBound > earlyLimitUs ? *task.InputBound - earlyLimitUs : 0;
            YQL_ENSURE(!task.DirectOutputBound || readFromUs >= *task.DirectOutputBound,
                "OUTPUT_START_TIME cannot combine hopping and non-hopping outputs through a shared source");
            // Reading slightly earlier is safe; hopping state carries the exact window boundary.
            state.SetStartingMessageTimestampMs(readFromUs / 1000);
            for (const auto& partition : source.Partitions) {
                auto& saved = *state.AddPartitions();
                saved.SetPartition(partition.Id);
                if (progress) {
                    saved.SetCluster(progress->at(partition).Cluster);
                }
                saved.SetOffset(0); // Explicitly override the old consumer offset.
            }
            sourcePlan.SetState(state.SerializeAsString());
        }
    }
    return plan;
}

TStateLoadPlan BuildHistoryReplayPlan(TReplayGraph& graph, const TReplayProgress& progress) {
    using namespace NKikimr::NMiniKQL;
    using namespace NYql::NDqProto::NDqStateLoadPlan;
    THashSet<TReplayPartition, TReplayPartition::THash> used;
    for (auto& [taskId, task] : graph.Tasks) {
        for (const auto& partition : task.Ancestors) {
            YQL_ENSURE(progress.contains(partition), "Input partition is absent from the previous query");
            used.insert(partition);
        }
        for (const auto& output : task.Task->GetOutputs()) {
            if (!output.HasSink()) {
                continue;
            }
            // These writers save empty checkpoint state. Topic writers carry
            // producer sequence numbers and require a different recovery protocol.
            const auto& sinkType = output.GetSink().GetType();
            YQL_ENSURE(sinkType == "SolomonSink" || sinkType == "S3Sink" || sinkType == "KqpTableSink",
                "Sink does not support history replay");
            YQL_ENSURE(task.WatermarkAvailable, "History replay requires an input watermark generator");
            ui64 boundary = Max<ui64>();
            for (const auto& partition : task.Ancestors) {
                boundary = Min(boundary, progress.at(partition).TimeUs);
            }
            graph.RequireOutputBefore(taskId, boundary);
        }
    }
    YQL_ENSURE(used.size() == progress.size(), "History replay requires the same input partitions");

    return BuildReplayTaskPlans(graph, &progress);
}

} // namespace

bool MakeHistoryReplayPlan(
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& src,
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& dst,
    const TCheckpointTaskStates& states, TStateLoadPlan& plan, NYql::TIssues& issues)
{
    try {
        YQL_ENSURE(!src.empty() && !dst.empty(), "History replay requires both query graphs");
        TReplayGraph previous(src);
        const auto progress = ReadReplayProgress(previous, states);
        TReplayGraph next(dst);
        plan = BuildHistoryReplayPlan(next, progress);
        return true;
    } catch (const std::exception& e) {
        issues.AddIssue(NYql::TIssue(TStringBuilder() << "Cannot replay streaming query history: " << e.what()));
        return false;
    }
}

bool MakeOutputStartTimeReplayPlan(
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& tasks,
    ui64 outputStartTimeUs, TStateLoadPlan& plan, NYql::TIssues& issues, bool useSourceDisposition)
{
    try {
        YQL_ENSURE(!tasks.empty(), "Replay from OUTPUT_START_TIME requires a query graph");
        TReplayGraph graph(tasks);
        for (const auto& [taskId, task] : graph.Tasks) {
            for (const auto& output : task.Task->GetOutputs()) {
                if (output.HasSink()) {
                    YQL_ENSURE(task.WatermarkAvailable, "Replay from OUTPUT_START_TIME requires an input watermark generator");
                    // Sinks start with empty state; no previous producer state
                    // or checkpoint is transferred in this mode.
                    graph.RequireOutputFrom(taskId, outputStartTimeUs);
                }
            }
        }
        plan = BuildReplayTaskPlans(graph, nullptr, useSourceDisposition);
        return true;
    } catch (const std::exception& e) {
        issues.AddIssue(NYql::TIssue(TStringBuilder() << "Cannot start from OUTPUT_START_TIME: " << e.what()));
        return false;
    }
}

} // namespace NFq
