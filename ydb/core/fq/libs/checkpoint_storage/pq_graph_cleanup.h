#pragma once

#include "graph_cleanup.h"

#include <ydb/library/yql/providers/common/token_accessor/client/factory.h>
#include <ydb/library/yql/providers/pq/gateway/abstract/yql_pq_gateway.h>

namespace NFq {

TCheckpointGraphCleanup CreatePqCheckpointGraphCleanup(
    NYql::IPqStaticGateway::TPtr pqGateway,
    NYdb::TDriver driver,
    NYql::IStructuredTokenCredentialsFactory::TPtr credentialsFactory);

} // namespace NFq
