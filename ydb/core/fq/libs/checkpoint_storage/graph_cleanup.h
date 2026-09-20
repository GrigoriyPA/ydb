#pragma once

#include <ydb/core/fq/libs/checkpoint_storage/proto/graph_description.pb.h>

#include <library/cpp/threading/future/core/future.h>
#include <yql/essentials/public/issue/yql_issue.h>

#include <functional>

namespace NFq {

// Called before removing the last reference to a checkpoint graph description.
// Cleanup must be idempotent: a failed deletion can be retried.
using TCheckpointGraphCleanup = std::function<NThreading::TFuture<NYql::TIssues>(const NProto::TCheckpointGraphDescription&)>;

} // namespace NFq
