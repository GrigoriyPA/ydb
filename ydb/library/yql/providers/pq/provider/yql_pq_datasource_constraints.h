#pragma once

#include <yql/essentials/core/yql_graph_transformer.h>

#include <memory>

namespace NYql {

std::unique_ptr<IGraphTransformer> CreatePqDataSourceConstraintTransformer();

} // NYql
