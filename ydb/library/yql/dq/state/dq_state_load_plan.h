#pragma once

#include <ydb/library/actors/core/actorsystem_fwd.h>

#include <google/protobuf/message.h>

namespace NYql {

namespace NDqProto {

class TDqTask;

} // namespace NDqProto

namespace NDq {

struct TStateLoadPlanResolverSettings {
    bool Force = false;
    bool StrictStateRecovery = false; // In not-force mode enforce to recover all states including operators and sinks
};

// Make plan for loading streaming offsets from an old graph.
NActors::IActor* CreateStateLoadPlanResolver(google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> src, google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask> dst, TStateLoadPlanResolverSettings settings);

} // namespace NDq

} // namespace NYql
