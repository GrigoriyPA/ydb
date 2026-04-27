#pragma once
#include <yql/essentials/minikql/computation/mkql_computation_node_impl.h>

namespace NKikimr {
namespace NMiniKQL {

IComputationNode* WrapStreamingAggregation(TCallable& callable, const TComputationNodeFactoryContext& ctx);

} // namespace NMiniKQL
} // namespace NKikimr
