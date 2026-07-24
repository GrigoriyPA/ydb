#pragma once

#include <ydb/library/actors/core/actorsystem_fwd.h>

#include <google/protobuf/message.h>

namespace NYql {

namespace NDqProto {

class TDqTask;

} // namespace NDqProto

namespace NDq {

// Make plan for loading streaming offsets from an old graph.
NActors::IActor* CreateStateLoadPlanResolver(const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& src, const google::protobuf::RepeatedPtrField<NYql::NDqProto::TDqTask>& dst, bool force);

} // namespace NDq

} // namespace NYql
