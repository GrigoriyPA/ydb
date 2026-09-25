#include "dq_state_load_plan.h"
#include <ydb/core/fq/libs/checkpointing/events/events.h>
#include <ydb/core/fq/libs/state/dq_stage_state_recovery_info.h>
#include <ydb/library/yql/providers/pq/proto/dq_io_state.pb.h>
#include <yql/essentials/minikql/comp_nodes/mkql_saveload.h>
#include <yql/essentials/minikql/mkql_node_builder.h>
#include <yql/essentials/minikql/mkql_node_serialization.h>

#include <ydb/library/yql/providers/dq/api/protos/service.pb.h>
#include <ydb/library/yql/providers/pq/common/yql_names.h>
#include <ydb/library/yql/providers/pq/proto/dq_io.pb.h>
#include <ydb/library/yql/providers/pq/proto/dq_task_params.pb.h>
#include <ydb/library/yql/providers/pq/task_meta/task_meta.h>

#include <library/cpp/testing/unittest/registar.h>
#include <ydb/library/actors/testlib/test_runtime.h>
#include <ydb/library/testlib/pq_helpers/mock_pq_gateway.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor.h>

namespace NFq {

namespace {

struct TGraphBuilder;
struct TTaskBuilder;

struct TTaskInputBuilder {
    TTaskBuilder* Parent;
    NYql::NDqProto::TTaskInput* In;

    TTaskInputBuilder& Channel() {
        In->AddChannels();
        In->MutableUnionAll();
        return *this;
    }

    TTaskInputBuilder& Source() {
        In->MutableSource()->SetType("Unknown");
        return *this;
    }

    TTaskInputBuilder& TopicSource(const TString& topic, ui64 partitionsCount, ui64 dqPartitionsCount, ui64 eachPartition);

    TTaskBuilder& Build() {
        return *Parent;
    }
};

struct TTaskOutputBuilder {
    TTaskBuilder* Parent;
    NYql::NDqProto::TTaskOutput* Out;

    TTaskOutputBuilder& Channel() {
        Out->AddChannels();
        Out->MutableBroadcast();
        return *this;
    }

    TTaskOutputBuilder& Sink() {
        Out->MutableSink();
        return *this;
    }

    TTaskBuilder& Build() {
        return *Parent;
    }
};

struct TTaskBuilder {
    TGraphBuilder* Parent;
    NYql::NDqProto::TDqTask* Task;

    TTaskInputBuilder Input() {
        return TTaskInputBuilder{this, Task->AddInputs()};
    }

    TTaskOutputBuilder Output() {
        return TTaskOutputBuilder{this, Task->AddOutputs()};
    }

    TGraphBuilder& Build() {
        return *Parent;
    }
};

struct TGraphBuilder {
    google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> Graph;

    TTaskBuilder Task(ui64 id = 0) {
        auto* task = Graph.Add();
        task->SetId(id ? id : Graph.size());
        return TTaskBuilder{this, task};
    }

    google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> Build() {
        return std::move(Graph);
    }
};

TTaskInputBuilder& TTaskInputBuilder::TopicSource(const TString& topic, ui64 partitionsCount, ui64 dqPartitionsCount, ui64 eachPartition) {
    auto* src = In->MutableSource();
    src->SetType(TString(NYql::PqSource));

    NYql::NPq::NProto::TDqPqTopicSource topicSrcSettings;
    topicSrcSettings.SetDatabase("DB");
    topicSrcSettings.SetDatabaseId("DBID");
    topicSrcSettings.SetTopicPath(topic);
    src->MutableSettings()->PackFrom(topicSrcSettings);

    NYql::NPq::NProto::TDqReadTaskParams readTaskParams;
    auto* part = readTaskParams.AddPartitioningParams();
    part->SetTopicPartitionsCount(partitionsCount);
    part->SetDqPartitionsCount(dqPartitionsCount);
    part->SetEachTopicPartitionGroupId(eachPartition);
    TString readTaskParamsBytes;
    UNIT_ASSERT(readTaskParams.SerializeToString(&readTaskParamsBytes));
    Yql::DqsProto::TTaskMeta meta;
    (*meta.MutableTaskParams())["pq"] = readTaskParamsBytes;
    Parent->Task->MutableMeta()->PackFrom(meta);
    return *this;
}

ui64 SourcesCount(const NYql::NDqProto::TDqTask& task) {
    ui64 cnt = 0;
    for (const auto& input : task.GetInputs()) {
        if (input.GetTypeCase() == NYql::NDqProto::TTaskInput::kSource) {
            ++cnt;
        }
    }
    return cnt;
}

ui64 SinksCount(const NYql::NDqProto::TDqTask& task) {
    ui64 cnt = 0;
    for (const auto& output : task.GetOutputs()) {
        if (output.GetTypeCase() == NYql::NDqProto::TTaskOutput::kSink) {
            ++cnt;
        }
    }
    return cnt;
}

struct TTestCase : public NUnitTest::TBaseTestCase {
    TGraphBuilder SrcGraph;
    TGraphBuilder DstGraph;
    THashMap<ui64, NYql::NDqProto::NDqStateLoadPlan::TTaskPlan> Plan;
    NYql::TIssues Issues;

    bool MakePlan(bool force) {
        Plan.clear();
        Issues.Clear();
        const bool result = MakeContinueFromStreamingOffsetsPlan(SrcGraph.Graph, DstGraph.Graph, force, Plan, Issues);
        if (result) {
            ValidatePlan();
        } else {
            UNIT_ASSERT_UNEQUAL(Issues.Size(), 0);
        }
        return result;
    }

    void SwapGraphs() {
        SrcGraph.Graph.Swap(&DstGraph.Graph);
    }

    const NYql::NDqProto::TDqTask& FindSrcTask(ui64 taskId) const {
        for (const auto& task : SrcGraph.Graph) {
            if (task.GetId() == taskId) {
                return task;
            }
        }
        UNIT_ASSERT_C(false, "Task " << taskId << " was not found in src graph");
        // Make compiler happy
        return SrcGraph.Graph.Get(42);
    }

    void ValidatePlan() const {
        UNIT_ASSERT_VALUES_EQUAL(Plan.size(), DstGraph.Graph.size());
        for (const auto& task : DstGraph.Graph) {
            const auto taskPlanIt = Plan.find(task.GetId());
            UNIT_ASSERT_C(taskPlanIt != Plan.end(), "Task " << task.GetId() << " was not found in plan");
            const auto& taskPlan = taskPlanIt->second;
            UNIT_ASSERT_C(taskPlan.GetStateType() != NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_UNSPECIFIED, "Task " << task.GetId() << " plan: " << taskPlan);
            if (taskPlan.GetStateType() == NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY) {
                UNIT_ASSERT_C(!taskPlan.HasProgram(), "Task " << task.GetId() << " plan: " << taskPlan);
                UNIT_ASSERT_VALUES_EQUAL_C(taskPlan.SourcesSize(), 0, "Task " << task.GetId() << " plan: " << taskPlan);
                UNIT_ASSERT_VALUES_EQUAL_C(taskPlan.SinksSize(), 0, "Task " << task.GetId() << " plan: " << taskPlan);
            } else {
                UNIT_ASSERT_C(taskPlan.GetStateType() == NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_FOREIGN, "Task " << task.GetId() << " plan: " << taskPlan);
                UNIT_ASSERT_C(taskPlan.HasProgram(), "Task " << task.GetId() << " plan: " << taskPlan);
                UNIT_ASSERT_C(taskPlan.GetProgram().GetStateType() == NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY, "Task " << task.GetId() << " plan: " << taskPlan);
                UNIT_ASSERT_VALUES_EQUAL_C(taskPlan.SourcesSize(), SourcesCount(task), "Task " << task.GetId() << " plan: " << taskPlan);
                UNIT_ASSERT_VALUES_EQUAL_C(taskPlan.SinksSize(), SinksCount(task), "Task " << task.GetId() << " plan: " << taskPlan);
                for (const auto& sourcePlan : taskPlan.GetSources()) {
                    UNIT_ASSERT_C(sourcePlan.GetStateType() != NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_UNSPECIFIED, "Task " << task.GetId() << " plan: " << taskPlan);
                    UNIT_ASSERT_C(sourcePlan.GetStateType() == NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY || sourcePlan.GetStateType() == NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_FOREIGN, "Task " << task.GetId() << " plan: " << taskPlan);
                    UNIT_ASSERT_C(sourcePlan.GetInputIndex() < task.InputsSize(), "Task " << task.GetId() << " plan: " << taskPlan);
                    const auto& taskInput = task.GetInputs(sourcePlan.GetInputIndex());
                    UNIT_ASSERT_C(taskInput.GetTypeCase() == NYql::NDqProto::TTaskInput::kSource, "Task " << task.GetId() << " plan: " << taskPlan);
                    // State type is foreign => source type is pq
                    UNIT_ASSERT_C(sourcePlan.GetStateType() != NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_FOREIGN || taskInput.GetSource().GetType() == NYql::PqSource, "Task " << task.GetId() << " plan: " << taskPlan << ". Task input: " << taskInput);
                    if (sourcePlan.GetStateType() == NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_FOREIGN) {
                        UNIT_ASSERT_C(sourcePlan.ForeignTasksSourcesSize() > 0, "Task " << task.GetId() << " plan: " << taskPlan);
                        const TMaybe<NYql::NPq::TTopicPartitionsSet> partitionsSet = NYql::NPq::GetTopicPartitionsSet(task.GetMeta());
                        UNIT_ASSERT_C(partitionsSet, "Task " << task.GetId() << " plan: " << taskPlan);
                        for (const auto& taskSource : sourcePlan.GetForeignTasksSources()) {
                            const auto& srcTask = FindSrcTask(taskSource.GetTaskId()); // with assertion
                            UNIT_ASSERT_C(taskSource.GetInputIndex() < srcTask.InputsSize(), "Task " << srcTask.GetId() << " plan: " << taskPlan);
                            const auto& srcTaskInput = srcTask.GetInputs(taskSource.GetInputIndex());
                            UNIT_ASSERT_C(srcTaskInput.GetTypeCase() == NYql::NDqProto::TTaskInput::kSource, "Task " << srcTask.GetId() << " plan: " << taskPlan);
                            UNIT_ASSERT_C(srcTaskInput.GetSource().GetType() == NYql::PqSource, "Task " << srcTask.GetId() << " plan: " << taskPlan);
                            const TMaybe<NYql::NPq::TTopicPartitionsSet> srcTaskPartitionsSet = NYql::NPq::GetTopicPartitionsSet(task.GetMeta());
                            UNIT_ASSERT_C(srcTaskPartitionsSet, "Task " << srcTask.GetId() << " plan: " << taskPlan);
                            UNIT_ASSERT_C(partitionsSet->Intersects(*srcTaskPartitionsSet), "Task " << srcTask.GetId() << " plan: " << taskPlan);
                        }
                    }
                }
                for (const auto& sinkPlan : taskPlan.GetSinks()) {
                    UNIT_ASSERT_C(sinkPlan.GetStateType() == NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY, "Task " << task.GetId() << " plan: " << taskPlan);
                    UNIT_ASSERT_C(sinkPlan.GetOutputIndex() < task.OutputsSize(), "Task " << task.GetId() << " plan: " << taskPlan);
                    const auto& taskOutput = task.GetOutputs(sinkPlan.GetOutputIndex());
                    UNIT_ASSERT_C(taskOutput.GetTypeCase() == NYql::NDqProto::TTaskOutput::kSink, "Task " << task.GetId() << " plan: " << taskPlan);
                }
            }
        }
    }

    void AssertTaskPlanIsEmpty(ui64 taskId) const {
        const auto taskPlanIt = Plan.find(taskId);
        UNIT_ASSERT_C(taskPlanIt != Plan.end(), "Task " << taskId << " was not found in plan");
        UNIT_ASSERT_C(taskPlanIt->second.GetStateType() == NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_EMPTY, taskPlanIt->second);
    }

    void AssertTaskPlanSourceHasSourceTask(ui64 taskId, ui64 sourceIndex, ui64 srcTaskId, ui64 srcInputIndex) const {
        const auto taskPlanIt = Plan.find(taskId);
        UNIT_ASSERT_C(taskPlanIt != Plan.end(), "Task " << taskId << " was not found in plan");
        const auto& taskPlan = taskPlanIt->second;
        UNIT_ASSERT_C(taskPlan.GetStateType() == NYql::NDqProto::NDqStateLoadPlan::STATE_TYPE_FOREIGN, taskPlanIt->second);
        for (const auto& sourcePlan : taskPlan.GetSources()) {
            if (sourcePlan.GetInputIndex() == sourceIndex) {
                for (const auto& foreignTaskSource : sourcePlan.GetForeignTasksSources()) {
                    if (foreignTaskSource.GetTaskId() == srcTaskId) {
                        UNIT_ASSERT_VALUES_EQUAL_C(foreignTaskSource.GetInputIndex(), srcInputIndex, foreignTaskSource);
                        return;
                    }
                }
                UNIT_ASSERT_C(false, "Source task " << srcTaskId << " was not found in source plan for index " << sourceIndex);
            }
        }
        UNIT_ASSERT_C(false, "Source plan for index " << sourceIndex << " was not found");
    }

    TString IssuesStr() const {
        return Issues.ToString();
    }
};

} // namespace

Y_UNIT_TEST_SUITE_F(TContinueFromStreamingOffsetsPlanTest, TTestCase) {
    Y_UNIT_TEST(Empty) {
        UNIT_ASSERT(MakePlan(false));
        UNIT_ASSERT(MakePlan(true));
    }

    Y_UNIT_TEST(OneToOneMapping) {
        SrcGraph
            .Task()
                .Input().Channel().Build()
                .Output().Channel().Build()
                .Build()
            .Task()
                .Input().Channel().Build()
                .Input().TopicSource("t", 3, 3, 0).Build();
        DstGraph
            .Task()
                .Input().TopicSource("t", 3, 3, 0).Build();

        UNIT_ASSERT(MakePlan(false));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        UNIT_ASSERT(MakePlan(true));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        AssertTaskPlanSourceHasSourceTask(1, 0, 2, 1);

        SwapGraphs();
        UNIT_ASSERT(MakePlan(false));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        UNIT_ASSERT(MakePlan(true));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        AssertTaskPlanIsEmpty(1);
    }

    Y_UNIT_TEST(DifferentPartitioning) {
        SrcGraph
            .Task()
                .Input().Channel().Build()
                .Input().TopicSource("t", 4, 1, 0).Build();
        DstGraph
            .Task()
                .Input().TopicSource("t", 4, 2, 0).Build()
                .Build()
            .Task()
                .Input().TopicSource("t", 4, 2, 1).Build();

        UNIT_ASSERT(MakePlan(false));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        UNIT_ASSERT(MakePlan(true));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        AssertTaskPlanSourceHasSourceTask(1, 0, 1, 1);
        AssertTaskPlanSourceHasSourceTask(2, 0, 1, 1);

        SwapGraphs();
        UNIT_ASSERT(MakePlan(false));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        UNIT_ASSERT(MakePlan(true));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        AssertTaskPlanSourceHasSourceTask(1, 1, 1, 0);
        AssertTaskPlanSourceHasSourceTask(1, 1, 2, 0);
    }

    Y_UNIT_TEST(MultipleTopics) {
        SrcGraph
            .Task()
                .Input().TopicSource("t", 1, 1, 0).Build()
                .Build()
            .Task()
                .Input().TopicSource("p", 1, 1, 0).Build()
                .Build();

        DstGraph
            .Task()
                .Input().TopicSource("p", 1, 1, 0).Build()
                .Build()
            .Task()
                .Input().TopicSource("t", 1, 1, 0).Build()
                .Build();

        UNIT_ASSERT(MakePlan(false));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        UNIT_ASSERT(MakePlan(true));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        AssertTaskPlanSourceHasSourceTask(1, 0, 2, 0);
        AssertTaskPlanSourceHasSourceTask(2, 0, 1, 0);

        SwapGraphs();
        UNIT_ASSERT(MakePlan(false));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        UNIT_ASSERT(MakePlan(true));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
    }

    Y_UNIT_TEST(AllTopicsMustBeUsedInNonForceMode) {
        SrcGraph
            .Task()
                .Input().TopicSource("t", 1, 1, 0).Build()
                .Build()
            .Task()
                .Input().TopicSource("p", 1, 1, 0).Build()
                .Build();

        DstGraph
            .Task()
                .Input().TopicSource("t", 1, 1, 0).Build()
                .Build();

        UNIT_ASSERT(!MakePlan(false));
        UNIT_ASSERT(MakePlan(true));

        SwapGraphs();
        UNIT_ASSERT(!MakePlan(false));
        UNIT_ASSERT(MakePlan(true));
        AssertTaskPlanIsEmpty(2);
    }

    Y_UNIT_TEST(NotMappedAllPartitions) {
        SrcGraph
            .Task()
                .Input().TopicSource("t", 5, 1, 0).Build()
                .Build();

        DstGraph
            .Task()
                .Input().TopicSource("t", 10, 2, 0).Build()
                .Build()
            .Task()
                .Input().TopicSource("t", 10, 2, 1).Build()
                .Build();

        UNIT_ASSERT(!MakePlan(false));
        UNIT_ASSERT_UNEQUAL(Issues.Size(), 0);
        UNIT_ASSERT(MakePlan(true));
        UNIT_ASSERT_UNEQUAL(Issues.Size(), 0);
        AssertTaskPlanSourceHasSourceTask(1, 0, 1, 0);
        AssertTaskPlanSourceHasSourceTask(2, 0, 1, 0);

        SwapGraphs();
        UNIT_ASSERT(MakePlan(false));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        UNIT_ASSERT(MakePlan(true));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
    }

    Y_UNIT_TEST(ReadPartitionInSeveralPlacesIsOk) {
        SrcGraph
            .Task()
                .Input().TopicSource("t", 5, 1, 0).Build()
                .Build();

        DstGraph
            .Task()
                .Input().TopicSource("t", 5, 1, 0).Build()
                .Build()
            .Task()
                .Input().TopicSource("t", 5, 2, 0).Build()
                .Build()
            .Task()
                .Input().TopicSource("t", 5, 2, 1).Build()
                .Build();

        UNIT_ASSERT(MakePlan(false));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);
        UNIT_ASSERT(MakePlan(true));
        UNIT_ASSERT_VALUES_EQUAL(Issues.Size(), 0);

        AssertTaskPlanSourceHasSourceTask(1, 0, 1, 0);
        AssertTaskPlanSourceHasSourceTask(2, 0, 1, 0);
        AssertTaskPlanSourceHasSourceTask(3, 0, 1, 0);

        SwapGraphs();
        UNIT_ASSERT(!MakePlan(false));
    }

    Y_UNIT_TEST(MapSeveralReadingsToOneIsAllowedOnlyInForceMode) {
        SrcGraph
            .Task()
                .Input().TopicSource("t", 5, 1, 0).Build()
                .Build()
            .Task()
                .Input().TopicSource("t", 5, 1, 0).Build()
                .Build();

        DstGraph
            .Task()
                .Input().TopicSource("t", 5, 1, 0).Build()
                .Build()
            .Task()
                .Input().TopicSource("t", 5, 2, 0).Build()
                .Build()
            .Task()
                .Input().TopicSource("t", 5, 2, 1).Build()
                .Build();

        UNIT_ASSERT(!MakePlan(false));
        UNIT_ASSERT(MakePlan(true));
        UNIT_ASSERT_UNEQUAL(Issues.Size(), 0);
    }
}

namespace {

// These callables exercise program inspection; execution and save/load of the
// actual hopping graph are covered by TDqMultiHoppingSaveLoadTest.
void SetReplayProgram(NYql::NDqProto::TDqTask& task, ui64 hopUs = 0, ui64 windowUs = 0, TStringBuf otherOperator = {}, bool streamOperator = false, TMaybe<bool> checkMinWindowStart = true) {
    using namespace NKikimr::NMiniKQL;
    TScopedAlloc alloc(__LOCATION__);
    TTypeEnvironment env(alloc);
    const auto dataType = TDataType::Create(NYql::NUdf::TDataType<ui64>::Id, env);
    const auto literal = [&](ui64 value) {
        return TRuntimeNode(TDataLiteral::Create(NYql::NUdf::TUnboxedValuePod(value), dataType, env), true);
    };
    auto root = literal(0);
    if (task.InputsSize() && task.GetInputs(0).HasSource()) {
        TCallableBuilder generator(env, "DqWatermarkGenerator", dataType);
        root = TRuntimeNode(generator.Build(), false);
    }
    if (hopUs) {
        TCallableBuilder hopping(env, "MultiHoppingCore", dataType);
        for (ui32 i = 0; i <= (checkMinWindowStart ? 25U : 20U); ++i) {
            if (i == 0) {
                hopping.Add(root);
            } else if (i == 20 || i == 25) {
                const auto boolType = TDataType::Create(NYql::NUdf::TDataType<bool>::Id, env);
                hopping.Add(TRuntimeNode(TDataLiteral::Create(NYql::NUdf::TUnboxedValuePod(i == 20 || *checkMinWindowStart), boolType, env), true));
            } else {
                hopping.Add(literal(i == 16 ? hopUs : (i == 17 ? windowUs : 0)));
            }
        }
        root = TRuntimeNode(hopping.Build(), false);
    }
    if (otherOperator) {
        if (streamOperator) {
            TCallableBuilder flow(env, "ToFlow", TFlowType::Create(dataType, env));
            flow.Add(root);
            root = TRuntimeNode(flow.Build(), false);
        }
        TCallableBuilder other(env, otherOperator, dataType);
        other.Add(root);
        root = TRuntimeNode(other.Build(), false);
    }
    task.SetStageId(task.GetId());
    task.MutableProgram()->SetRuntimeVersion(NYql::NDqProto::RUNTIME_VERSION_YQL_1_0);
    task.MutableProgram()->SetRaw(SerializeRuntimeNode(root, env));
}

constexpr ui64 Second = 1000000;

struct TReplayTestGraph {
    TGraphBuilder Builder;
    TCheckpointTaskStates States;

    void Source(ui64 id = 1, ui64 partition = 0, ui64 partitions = 1) {
        auto builder = Builder.Task(id);
        builder.Input().TopicSource("topic", partitions, partitions, partition);
        SetReplayProgram(*builder.Task);
        auto& state = States[id];
        state.MiniKqlProgram.ConstructInPlace().Data.Version = 2;
        NYql::NPq::NProto::TDqPqTopicSourceState source;
        source.AddTopics()->SetTopicPath("topic");
        source.SetStartingMessageTimestampMs(1000);
        auto& saved = *source.AddPartitions();
        saved.SetPartition(partition);
        saved.SetOffset(100);
        saved.SetLastWriteTimeUs(660 * Second);
        state.Sources.emplace_back().Data.emplace_back(source.SerializeAsString(), 1);
    }

    void Hop(ui64 id, ui64 parent, ui64 step, ui64 window, ui64 checkpointTime, bool sink = true) {
        using namespace NKikimr::NMiniKQL;
        auto builder = Builder.Task(id);
        builder.Input().Channel().In->MutableChannels(0)->SetSrcTaskId(parent);
        if (sink) {
            builder.Output().Sink().Out->MutableSink()->SetType("SolomonSink");
        }
        SetReplayProgram(*builder.Task, step, window);
        TString state;
        WriteUi32(state, static_cast<ui32>(EMkqlStateType::SIMPLE_BLOB));
        WriteUi32(state, THoppingRecoveryState::StateVersion);
        UNIT_ASSERT_VALUES_EQUAL(checkpointTime % step, 0);
        WriteUi64(state, checkpointTime / step);
        WriteUi32(state, 0);
        WriteBool(state, false);
        auto& program = States[id].MiniKqlProgram.ConstructInPlace();
        program.Data.Version = 2;
        TNodeStateHelper::AddNodeState(program.Data.Blob, state);
    }
};

ui64 ReplayWindowStartIndex(const TStateLoadPlan& plan, ui64 taskId) {
    using namespace NKikimr::NMiniKQL;
    using namespace NYql::NDqProto::NDqStateLoadPlan;
    UNIT_ASSERT(plan.at(taskId).GetStateType() == STATE_TYPE_FOREIGN);
    UNIT_ASSERT(plan.at(taskId).GetProgram().GetStateType() == STATE_TYPE_FOREIGN);
    UNIT_ASSERT(plan.at(taskId).GetProgram().HasState());
    TStringBuf state(plan.at(taskId).GetProgram().GetState());
    const auto size = ReadUi64(state);
    UNIT_ASSERT_VALUES_EQUAL(size, state.size());
    return THoppingRecoveryState::Read(state).MinWindowStartIndex;
}

ui64 ReplayReadTime(const TStateLoadPlan& plan, ui64 taskId) {
    using namespace NYql::NDqProto::NDqStateLoadPlan;
    const auto& sourcePlan = plan.at(taskId).GetSources(0);
    UNIT_ASSERT(plan.at(taskId).GetStateType() == STATE_TYPE_FOREIGN);
    UNIT_ASSERT(sourcePlan.GetStateType() == STATE_TYPE_FOREIGN);
    UNIT_ASSERT(sourcePlan.HasState());
    UNIT_ASSERT(sourcePlan.GetForeignTasksSources().empty());
    UNIT_ASSERT_VALUES_EQUAL(sourcePlan.GetStateVersion(), 1);
    NYql::NPq::NProto::TDqPqTopicSourceState source;
    UNIT_ASSERT(source.ParseFromString(sourcePlan.GetState()));
    return source.GetStartingMessageTimestampMs() * 1000;
}

} // namespace

Y_UNIT_TEST_SUITE(THistoryReplayPlan) {
    Y_UNIT_TEST(RejectsHoppingWithoutReplayCapability) {
        for (const auto enabled : {TMaybe<bool>{}, TMaybe<bool>{false}}) {
            TReplayTestGraph old;
            old.Source();
            old.Hop(2, 1, 10 * Second, 60 * Second, 600 * Second);
            TReplayTestGraph next;
            next.Source();
            next.Hop(2, 1, 10 * Second, 30 * Second, 0);
            SetReplayProgram(*next.Builder.Graph.Mutable(1), 10 * Second, 30 * Second, {}, false, enabled);
            TStateLoadPlan plan;
            NYql::TIssues issues;
            UNIT_ASSERT(!MakeOutputStartTimeReplayPlan(next.Builder.Graph, 600 * Second, plan, issues));
            UNIT_ASSERT(plan.empty());
            UNIT_ASSERT_STRING_CONTAINS(issues.ToString(), "Hopping minimum window start checking is not enabled");
            UNIT_ASSERT(!MakeHistoryReplayPlan(old.Builder.Graph, next.Builder.Graph, old.States, plan, issues));
            UNIT_ASSERT(plan.empty());
            // Both the previous and replacement query must opt in.
            UNIT_ASSERT(!MakeHistoryReplayPlan(next.Builder.Graph, old.Builder.Graph, next.States, plan, issues));
            UNIT_ASSERT(plan.empty());
        }
    }

    Y_UNIT_TEST(RejectsLegacyCheckpointsEvenWithReplayCapability) {
        using namespace NKikimr::NMiniKQL;
        for (ui32 version : {1U, 2U}) {
            TReplayTestGraph graph;
            graph.Source();
            graph.Hop(2, 1, 10 * Second, 30 * Second, 600 * Second);
            TString state;
            WriteUi32(state, static_cast<ui32>(EMkqlStateType::SIMPLE_BLOB));
            WriteUi32(state, version);
            WriteUi32(state, 0);
            WriteBool(state, false);
            auto& blob = graph.States[2].MiniKqlProgram->Data.Blob;
            blob.clear();
            TNodeStateHelper::AddNodeState(blob, state);
            TStateLoadPlan plan;
            NYql::TIssues issues;
            UNIT_ASSERT(!MakeHistoryReplayPlan(graph.Builder.Graph, graph.Builder.Graph, graph.States, plan, issues));
            UNIT_ASSERT(plan.empty());
            UNIT_ASSERT_STRING_CONTAINS(issues.ToString(), "Hopping checkpoint has no recovery metadata");
        }
    }

    Y_UNIT_TEST(RejectsOverflowingCheckpointWindowStart) {
        using namespace NKikimr::NMiniKQL;
        TReplayTestGraph graph;
        graph.Source();
        graph.Hop(2, 1, 10 * Second, 30 * Second, 0);
        auto& blob = graph.States[2].MiniKqlProgram->Data.Blob;
        blob.clear();
        TNodeStateHelper::AddNodeState(blob, THoppingRecoveryState::MakeRecoveryState(Max<ui64>()));
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT(!MakeHistoryReplayPlan(graph.Builder.Graph, graph.Builder.Graph, graph.States, plan, issues));
        UNIT_ASSERT(plan.empty());
        UNIT_ASSERT_STRING_CONTAINS(issues.ToString(), "Hopping recovery time overflow");
    }

    Y_UNIT_TEST(ExplicitOutputStartTimeIsInclusiveAndRoundedUp) {
        TReplayTestGraph graph;
        graph.Source();
        graph.Hop(2, 1, 10 * Second, 30 * Second, 0);
        for (const auto time : {600 * Second, 600 * Second + 1}) {
            TStateLoadPlan plan;
            NYql::TIssues issues;
            UNIT_ASSERT_C(MakeOutputStartTimeReplayPlan(graph.Builder.Graph, time, plan, issues), issues.ToString());
            const auto start = time == 600 * Second ? 570 * Second : 580 * Second;
            UNIT_ASSERT_VALUES_EQUAL(ReplayWindowStartIndex(plan, 2), start / (10 * Second));
            UNIT_ASSERT_VALUES_EQUAL(ReplayReadTime(plan, 1), start - 300 * Second);
        }
    }

    Y_UNIT_TEST(ExplicitOutputStartTimeAlignsNestedHopsWithoutOldGraph) {
        TReplayTestGraph graph;
        graph.Source();
        graph.Hop(2, 1, 7 * Second, 21 * Second, 0, false);
        graph.Hop(3, 2, 10 * Second, 30 * Second, 0);
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT_C(MakeOutputStartTimeReplayPlan(graph.Builder.Graph, 601 * Second, plan, issues), issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(ReplayWindowStartIndex(plan, 3), 58);
        UNIT_ASSERT_VALUES_EQUAL(ReplayWindowStartIndex(plan, 2), 80);
        UNIT_ASSERT_VALUES_EQUAL(ReplayReadTime(plan, 1), 260 * Second);
    }

    Y_UNIT_TEST(ExplicitOutputStartTimeForStatelessQueryRoundsReadTimeDown) {
        TReplayTestGraph graph;
        graph.Source();
        graph.Builder.Graph.Mutable(0)->AddOutputs()->MutableSink()->SetType("PqSink");
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT_C(MakeOutputStartTimeReplayPlan(graph.Builder.Graph, 600 * Second + 123, plan, issues), issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(ReplayReadTime(plan, 1), 600 * Second);
    }

    Y_UNIT_TEST(ExplicitReadFromPreservesNestedHoppingBoundaries) {
        using namespace NYql::NDqProto::NDqStateLoadPlan;
        TReplayTestGraph graph;
        graph.Source();
        graph.Hop(2, 1, 7 * Second, 21 * Second, 0, false);
        graph.Hop(3, 2, 10 * Second, 30 * Second, 0);
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT_C(MakeOutputStartTimeReplayPlan(graph.Builder.Graph, 601 * Second, plan, issues,
            /* useSourceDisposition */ true), issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(ReplayWindowStartIndex(plan, 3), 58);
        UNIT_ASSERT_VALUES_EQUAL(ReplayWindowStartIndex(plan, 2), 80);
        const auto& source = plan.at(1).GetSources(0);
        UNIT_ASSERT(source.GetStateType() == STATE_TYPE_EMPTY);
        UNIT_ASSERT(!source.HasState());
        UNIT_ASSERT(source.GetForeignTasksSources().empty());
    }

    Y_UNIT_TEST(ExplicitOutputStartTimeRejectsOtherStatefulOperators) {
        TReplayTestGraph graph;
        graph.Source();
        graph.Hop(2, 1, 10 * Second, 30 * Second, 0);
        SetReplayProgram(*graph.Builder.Graph.Mutable(1), 10 * Second, 30 * Second, "Condense1");
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT(!MakeOutputStartTimeReplayPlan(graph.Builder.Graph, 600 * Second, plan, issues));
        UNIT_ASSERT(plan.empty());
        UNIT_ASSERT_STRING_CONTAINS(issues.ToString(), "Unsupported stateful operator");
    }

    Y_UNIT_TEST(ExplicitOutputStartTimeRejectsStatefulStreamOperations) {
        for (const TStringBuf operation : {"Chain1Map", "WideChain1Map", "MapNext", "Enumerate", "Take", "WideSkipWhileInclusive",
                "WideTakeBlocks", "WideTopSortBlocks", "WideSortWithSpilling", "Collect", "Reduce", "WideLastCombiner", "GraceJoin",
                "BlockCombineAll", "BlockCombineHashed", "BlockMergeFinalizeHashed", "BlockMergeManyFinalizeHashed"}) {
            TReplayTestGraph graph;
            graph.Source();
            graph.Hop(2, 1, 10 * Second, 30 * Second, 0);
            SetReplayProgram(*graph.Builder.Graph.Mutable(1), 10 * Second, 30 * Second, operation, true);
            TStateLoadPlan plan;
            NYql::TIssues issues;
            UNIT_ASSERT(!MakeOutputStartTimeReplayPlan(graph.Builder.Graph, 600 * Second, plan, issues));
            UNIT_ASSERT(plan.empty());
            UNIT_ASSERT_STRING_CONTAINS(issues.ToString(), "Unsupported stateful operator");
        }
    }

    Y_UNIT_TEST(ExplicitOutputStartTimeRejectsSharedHoppingAndRawOutput) {
        TReplayTestGraph graph;
        graph.Source();
        auto shared = graph.Builder.Task(2);
        shared.Input().Channel().In->MutableChannels(0)->SetSrcTaskId(1);
        shared.Output().Sink().Out->MutableSink()->SetType("SolomonSink");
        SetReplayProgram(*shared.Task);
        graph.Hop(3, 2, 10 * Second, 30 * Second, 0);
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT(!MakeOutputStartTimeReplayPlan(graph.Builder.Graph, 600 * Second, plan, issues));
        UNIT_ASSERT(plan.empty());
        UNIT_ASSERT_STRING_CONTAINS(issues.ToString(), "shared source");
    }

    Y_UNIT_TEST(AutomaticReplayToStatelessOutputKeepsRewindAllowance) {
        TReplayTestGraph old;
        old.Source();
        old.Hop(2, 1, 10 * Second, 30 * Second, 600 * Second);
        TReplayTestGraph next;
        next.Source();
        next.Builder.Graph.Mutable(0)->AddOutputs()->MutableSink()->SetType("SolomonSink");
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT_C(MakeHistoryReplayPlan(old.Builder.Graph, next.Builder.Graph, old.States, plan, issues), issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(ReplayReadTime(plan, 1), 300 * Second);
    }

    Y_UNIT_TEST(ChangedWindowReplaysHistoryAndInitializesCompleteWindows) {
        TReplayTestGraph old;
        old.Source();
        old.Hop(2, 1, 10 * Second, 60 * Second, 600 * Second);
        TReplayTestGraph next;
        next.Source();
        next.Hop(2, 1, 10 * Second, 30 * Second, 0);
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT_C(MakeHistoryReplayPlan(old.Builder.Graph, next.Builder.Graph, old.States, plan, issues), issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(ReplayWindowStartIndex(plan, 2), 56);
        UNIT_ASSERT_VALUES_EQUAL(ReplayReadTime(plan, 1), 260 * Second);
    }

    Y_UNIT_TEST(NestedWindowsAlignThroughBothQueryVersions) {
        TReplayTestGraph old;
        old.Source();
        old.Hop(2, 1, 10 * Second, 30 * Second, 600 * Second, false);
        old.Hop(3, 2, 20 * Second, 80 * Second, 560 * Second);
        TReplayTestGraph next;
        next.Source();
        next.Hop(2, 1, 7 * Second, 21 * Second, 0, false);
        next.Hop(3, 2, 10 * Second, 30 * Second, 0);
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT_C(MakeHistoryReplayPlan(old.Builder.Graph, next.Builder.Graph, old.States, plan, issues), issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(ReplayWindowStartIndex(plan, 3), 49);
        UNIT_ASSERT_VALUES_EQUAL(ReplayWindowStartIndex(plan, 2), 67);
        UNIT_ASSERT_VALUES_EQUAL(ReplayReadTime(plan, 1), 169 * Second);
    }

    Y_UNIT_TEST(PartitionsKeepIndependentRecoveryProgress) {
        TReplayTestGraph old;
        old.Source(1, 0, 2);
        old.Source(3, 1, 2);
        old.Hop(2, 1, 10 * Second, 60 * Second, 600 * Second);
        old.Hop(4, 3, 10 * Second, 60 * Second, 500 * Second);
        TReplayTestGraph next;
        next.Source(1, 0, 2);
        next.Source(3, 1, 2);
        next.Hop(2, 1, 10 * Second, 30 * Second, 0);
        next.Hop(4, 3, 10 * Second, 30 * Second, 0);
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT_C(MakeHistoryReplayPlan(old.Builder.Graph, next.Builder.Graph, old.States, plan, issues), issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(ReplayReadTime(plan, 1), 260 * Second);
        UNIT_ASSERT_VALUES_EQUAL(ReplayReadTime(plan, 3), 160 * Second);
    }

    Y_UNIT_TEST(SameTopicPathAtAnotherEndpointIsNotTheSameInput) {
        TReplayTestGraph old;
        old.Source();
        old.Hop(2, 1, 10 * Second, 60 * Second, 600 * Second);
        TReplayTestGraph next;
        next.Source();
        next.Hop(2, 1, 10 * Second, 30 * Second, 0);
        auto& settings = *next.Builder.Graph.Mutable(0)->MutableInputs(0)->MutableSource()->MutableSettings();
        NYql::NPq::NProto::TDqPqTopicSource source;
        UNIT_ASSERT(settings.UnpackTo(&source));
        source.SetEndpoint("another-cluster:2135");
        settings.PackFrom(source);
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT(!MakeHistoryReplayPlan(old.Builder.Graph, next.Builder.Graph, old.States, plan, issues));
        UNIT_ASSERT(plan.empty());
        UNIT_ASSERT_STRING_CONTAINS(issues.ToString(), "Input partition is absent from the previous query");
    }

    Y_UNIT_TEST(StatelessOldQueryRequiresItsWholeHistory) {
        TReplayTestGraph old;
        old.Source();
        old.Builder.Graph.Mutable(0)->AddOutputs()->MutableSink()->SetType("SolomonSink");
        TReplayTestGraph next;
        next.Source();
        next.Hop(2, 1, 10 * Second, 30 * Second, 0);
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT_C(MakeHistoryReplayPlan(old.Builder.Graph, next.Builder.Graph, old.States, plan, issues), issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(ReplayWindowStartIndex(plan, 2), 0);
        UNIT_ASSERT_VALUES_EQUAL(ReplayReadTime(plan, 1), 0);
    }

    Y_UNIT_TEST(RejectsSinkStateWithoutPublishingPartialPlan) {
        TReplayTestGraph graph;
        graph.Source();
        graph.Hop(2, 1, 10 * Second, 30 * Second, 600 * Second);
        graph.States[2].Sinks.emplace_back().Data.Blob = "producer state";
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT(!MakeHistoryReplayPlan(graph.Builder.Graph, graph.Builder.Graph, graph.States, plan, issues));
        UNIT_ASSERT(plan.empty());
        UNIT_ASSERT_STRING_CONTAINS(issues.ToString(), "sink has state");
    }

    Y_UNIT_TEST(RejectsOtherStatefulOperators) {
        TReplayTestGraph graph;
        graph.Source();
        graph.Hop(2, 1, 10 * Second, 30 * Second, 600 * Second);
        SetReplayProgram(*graph.Builder.Graph.Mutable(1), 10 * Second, 30 * Second, "Condense1");
        TStateLoadPlan plan;
        NYql::TIssues issues;
        UNIT_ASSERT(!MakeHistoryReplayPlan(graph.Builder.Graph, graph.Builder.Graph, graph.States, plan, issues));
        UNIT_ASSERT_STRING_CONTAINS(issues.ToString(), "Unsupported stateful operator");
    }
}

namespace {

class THistoryTestClient final : public NYql::ITopicClient {
public:
    bool Expired = false;
    TMaybe<ui64> FirstRetainedWriteTimeUs;
    ui64 Describes = 0;
    ui64 Reads = 0;
    std::shared_ptr<NYdb::TDriver> Driver;
    NTestUtils::IMockPqGateway::TPtr Gateway;

    NYdb::NTopic::TAsyncDescribePartitionResult DescribePartition(const TString&, i64 partitionId,
            const NYdb::NTopic::TDescribePartitionSettings&) override {
        ++Describes;
        Ydb::Topic::DescribePartitionResult description;
        auto* partition = description.mutable_partition();
        partition->set_partition_id(partitionId);
        auto* stats = partition->mutable_partition_stats();
        stats->mutable_partition_offsets()->set_start(FirstRetainedWriteTimeUs ? 10 : (Expired ? 100 : 0));
        stats->mutable_partition_offsets()->set_end(100);
        stats->mutable_last_write_time()->set_seconds(600);
        return NThreading::MakeFuture(NYdb::NTopic::TDescribePartitionResult(
            NYdb::TStatus(NYdb::EStatus::SUCCESS, {}), std::move(description)));
    }
    NYdb::NTopic::TAsyncDescribeTopicResult DescribeTopic(const TString&, const NYdb::NTopic::TDescribeTopicSettings&) override {
        ythrow yexception() << "Unexpected DescribeTopic";
    }
    NYdb::NTopic::TAsyncDescribeConsumerResult DescribeConsumer(const TString&, const TString&, const NYdb::NTopic::TDescribeConsumerSettings&) override {
        ythrow yexception() << "Unexpected DescribeConsumer";
    }
    std::shared_ptr<NYdb::NTopic::IReadSession> CreateReadSession(const NYdb::NTopic::TReadSessionSettings& settings) override {
        UNIT_ASSERT(FirstRetainedWriteTimeUs);
        UNIT_ASSERT(settings.WithoutConsumer_);
        ++Reads;
        Driver = std::make_shared<NYdb::TDriver>(NYdb::TDriverConfig{});
        Gateway = NTestUtils::CreateMockPqGateway();
        auto client = Gateway->GetTopicClient(*Driver, {});
        auto session = client->CreateReadSession(settings);
        Gateway->WaitReadSession("topic")->AddDataReceivedEvent(10, "unused", TInstant::MicroSeconds(*FirstRetainedWriteTimeUs));
        return session;
    }
    std::shared_ptr<NYdb::NTopic::ISimpleBlockingWriteSession> CreateSimpleBlockingWriteSession(const NYdb::NTopic::TWriteSessionSettings&) override {
        ythrow yexception() << "Unexpected write session";
    }
    std::shared_ptr<NYdb::NTopic::IWriteSession> CreateWriteSession(const NYdb::NTopic::TWriteSessionSettings&) override {
        ythrow yexception() << "Unexpected write session";
    }
    NYdb::TAsyncStatus CommitOffset(const TString&, ui64, const TString&, ui64, const NYdb::NTopic::TCommitOffsetSettings&) override {
        ythrow yexception() << "Replay validation must not commit consumer offsets";
    }
};

void CheckResolver(bool force, bool expired, bool continueOffsets, TMaybe<ui64> firstRetainedWriteTimeUs = {}, bool explicitOutputStartTime = false) {
    using namespace NYql::NDq;
    using namespace NYql::NDqProto::NDqStateLoadPlan;
    struct TRuntime : NActors::TTestActorRuntimeBase {
        TRuntime() { InitNodes(); }
    } runtime;
    const auto owner = runtime.AllocateEdgeActor();
    const auto storage = runtime.AllocateEdgeActor();
    auto client = MakeIntrusive<THistoryTestClient>();
    client->Expired = expired;
    client->FirstRetainedWriteTimeUs = firstRetainedWriteTimeUs;
    TReplayTestGraph old;
    old.Source();
    old.Hop(2, 1, 10 * Second, 60 * Second, 600 * Second);
    TReplayTestGraph next;
    next.Source();
    next.Hop(2, 1, 10 * Second, 30 * Second, 0);
    TStateLoadPlanResolverSettings settings;
    settings.StorageProxy = storage;
    settings.GraphId = "graph";
    settings.Checkpoint.SetId(5);
    settings.Checkpoint.SetGeneration(1);
    settings.CoordinatorGeneration = 2;
    settings.Replay = true;
    if (explicitOutputStartTime) {
        settings.OutputStartTimeUs = 590 * Second;
    }
    settings.Force = force;
    settings.ContinueFromOffsets = continueOffsets;
    settings.TopicClientFactory = [client](const auto&) { return client; };
    auto noCheckpointReads = runtime.AddObserver<TEvDqCompute::TEvGetTaskState>([&](auto&) {
        UNIT_ASSERT_C(!explicitOutputStartTime, "Explicit output start time must not read checkpoint state");
    });
    const auto resolver = runtime.Register(CreateStateLoadPlanResolver(
        explicitOutputStartTime ? google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>{} : old.Builder.Graph,
        next.Builder.Graph, settings),
        0, 0, NActors::TMailboxType::Simple, 0, owner);
    if (!explicitOutputStartTime) {
        const auto request = runtime.GrabEdgeEvent<TEvDqCompute::TEvGetTaskState>(storage);
        UNIT_ASSERT_VALUES_EQUAL(request->Get()->TaskIds.size(), 2);
        auto response = std::make_unique<TEvDqCompute::TEvGetTaskStateResult>(settings.Checkpoint, NYql::TIssues{}, 2);
        for (const auto taskId : request->Get()->TaskIds) {
            response->States.push_back(std::move(old.States.at(taskId)));
        }
        runtime.Send(new NActors::IEventHandle(resolver, storage, response.release()));
    }
    auto result = runtime.GrabEdgeEvent<TEvCheckpointCoordinator::TEvPrepareStateLoadPlanResult>(owner);
    UNIT_ASSERT_VALUES_EQUAL(client->Describes, 1);
    UNIT_ASSERT_VALUES_EQUAL(client->Reads, firstRetainedWriteTimeUs ? 1 : 0);
    UNIT_ASSERT_VALUES_EQUAL_C(result->Get()->Result, !expired || (force && !explicitOutputStartTime), result->Get()->Issues.ToString());
    if (!expired) {
        UNIT_ASSERT(result->Get()->Issues.Empty());
        UNIT_ASSERT(result->Get()->Plan.at(1).GetStateType() == STATE_TYPE_FOREIGN);
        UNIT_ASSERT_VALUES_EQUAL(ReplayWindowStartIndex(result->Get()->Plan, 2), 56);
    } else {
        UNIT_ASSERT_STRING_CONTAINS(result->Get()->Issues.ToString(), "Required history has expired");
        if (force && !explicitOutputStartTime) {
            UNIT_ASSERT(result->Get()->Plan.at(1).GetStateType() == (continueOffsets ? STATE_TYPE_FOREIGN : STATE_TYPE_EMPTY));
            UNIT_ASSERT_STRING_CONTAINS(result->Get()->Issues.ToString(), "FORCE=true");
        } else {
            UNIT_ASSERT(result->Get()->Plan.empty());
        }
    }
}

} // namespace

Y_UNIT_TEST_SUITE(THistoryReplayResolver) {
    Y_UNIT_TEST(ExplicitReadFromDoesNotReadCheckpointsOrValidateCalculatedHistory) {
        using namespace NYql::NDq;
        using namespace NYql::NDqProto::NDqStateLoadPlan;
        struct TRuntime : NActors::TTestActorRuntimeBase {
            TRuntime() { InitNodes(); }
        } runtime;
        const auto owner = runtime.AllocateEdgeActor();
        auto noCheckpointReads = runtime.AddObserver<TEvDqCompute::TEvGetTaskState>([](auto&) {
            UNIT_FAIL("Explicit input and output start times must not read checkpoint state");
        });
        TReplayTestGraph graph;
        graph.Source();
        graph.Hop(2, 1, 10 * Second, 30 * Second, 0);
        TStateLoadPlanResolverSettings settings;
        settings.OutputStartTimeUs = 590 * Second;
        settings.UseSourceDisposition = true;
        // No topic client: explicit READ_FROM must not probe the calculated history range.
        runtime.Register(CreateStateLoadPlanResolver({}, graph.Builder.Graph, settings),
            0, 0, NActors::TMailboxType::Simple, 0, owner);
        const auto result = runtime.GrabEdgeEvent<TEvCheckpointCoordinator::TEvPrepareStateLoadPlanResult>(owner);
        UNIT_ASSERT_C(result->Get()->Result, result->Get()->Issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(ReplayWindowStartIndex(result->Get()->Plan, 2), 56);
        UNIT_ASSERT(result->Get()->Plan.at(1).GetSources(0).GetStateType() == STATE_TYPE_EMPTY);
    }

    Y_UNIT_TEST(ExplicitOutputStartTimeDoesNotReadCheckpoints) { CheckResolver(false, false, false, {}, true); }
    Y_UNIT_TEST(ExplicitOutputStartTimeValidatesRetainedHistory) { CheckResolver(false, false, false, 250 * Second, true); }
    Y_UNIT_TEST(ExplicitOutputStartTimeDoesNotFallBackOnExpiredHistory) { CheckResolver(true, true, false, {}, true); }
    Y_UNIT_TEST(ForceStillAttemptsReplay) { CheckResolver(true, false, true); }
    Y_UNIT_TEST(RequiredHistoryRetainedAfterOlderDataExpired) { CheckResolver(false, false, true, 250 * Second); }
    Y_UNIT_TEST(RequiredHistoryIsOlderThanFirstRetainedMessage) { CheckResolver(false, true, true, 270 * Second); }
    Y_UNIT_TEST(FirstRetainedMessageAtReplayBoundIsAvailable) { CheckResolver(false, false, true, 260 * Second); }
    Y_UNIT_TEST(ExpiredHistoryFailsWithoutForce) { CheckResolver(false, true, true); }
    Y_UNIT_TEST(ExpiredHistoryFallsBackToOffsets) { CheckResolver(true, true, true); }
    Y_UNIT_TEST(ExpiredHistoryPreservesManualDisposition) { CheckResolver(true, true, false); }
}

} // namespace NFq
