#pragma once

#include <ydb/library/actors/core/actor.h>
#include <ydb/library/yql/dq/actors/compute/dq_checkpoints_states.h>
#include <ydb/library/yql/dq/actors/protos/dq_events.pb.h>
#include <ydb/library/yql/dq/proto/dq_state_load_plan.pb.h>
#include <ydb/library/yql/dq/proto/dq_tasks.pb.h>
#include <ydb/library/yql/providers/pq/gateway/abstract/yql_pq_topic_client.h>
#include <ydb/library/yql/providers/pq/proto/dq_io.pb.h>
#include <yql/essentials/public/issue/yql_issue.h>

#include <util/generic/hash.h>
#include <functional>

namespace NFq {

using TStateLoadPlan = THashMap<ui64, NYql::NDqProto::NDqStateLoadPlan::TTaskPlan>;
using TCheckpointTaskStates = THashMap<ui64, NYql::NDq::TComputeActorState>;
using TReplayTopicClientFactory = std::function<NYql::ITopicClient::TPtr(const NYql::NPq::NProto::TDqPqTopicSource&)>;

// Make plan for loading streaming offsets from an old graph.
bool MakeContinueFromStreamingOffsetsPlan(
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& src,
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& dst,
    bool force, TStateLoadPlan& plan, NYql::TIssues& issues);

bool MakeHistoryReplayPlan(
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& src,
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& dst,
    const TCheckpointTaskStates& states, TStateLoadPlan& plan, NYql::TIssues& issues);

bool MakeOutputStartTimeReplayPlan(
    const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& graph,
    ui64 outputStartTimeUs, TStateLoadPlan& plan, NYql::TIssues& issues, bool useSourceDisposition = false);

struct TStateLoadPlanResolverSettings {
    NActors::TActorId StorageProxy;
    TString GraphId;
    NYql::NDqProto::TCheckpoint Checkpoint;
    ui64 CoordinatorGeneration = 0;
    bool Replay = false;
    TMaybe<ui64> OutputStartTimeUs;
    // Keep explicit input positioning (READ_FROM) while setting hopping boundaries.
    bool UseSourceDisposition = false;
    bool Force = false;
    bool ContinueFromOffsets = true;
    TReplayTopicClientFactory TopicClientFactory;
};

NActors::IActor* CreateStateLoadPlanResolver(
    google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> src,
    google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> dst,
    TStateLoadPlanResolverSettings settings);

} // namespace NFq
