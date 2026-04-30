#include "yql_pq_datasource_constraints.h"

#include <ydb/library/yql/providers/pq/expr_nodes/yql_pq_expr_nodes.h>

#include <yql/essentials/providers/common/transform/yql_visit.h>

namespace NYql {

using namespace NNodes;

namespace {

class TPqDataSourceConstraintTransformer : public TVisitorTransformerBase {
    using TBase = TVisitorTransformerBase;

public:
    TPqDataSourceConstraintTransformer()
        : TBase(/* failOnUnknown */ true)
    {
        AddHandler({
            TPqReadTopic::CallableName(),
            TDqPqTopicSource::CallableName(),
        }, Hndl(&TPqDataSourceConstraintTransformer::HandleStreamingSource));
        AddHandler({
            TCoConfigure::CallableName(),
            TPqTopic::CallableName(),
            TCoSystemMetadata::CallableName(),
            TDqPqFederatedCluster::CallableName(),
        }, Hndl(&TPqDataSourceConstraintTransformer::HandleDefault));
    }

    TStatus HandleDefault(TExprBase, TExprContext&) {
        return TStatus::Ok;
    }

    TStatus HandleStreamingSource(TExprBase node, TExprContext& ctx) {
        node.MutableRaw()->AddConstraint(ctx.MakeConstraint<TStreamingConstraintNode>());
        return TStatus::Ok;
    }
};

} // anonymous namespace

std::unique_ptr<IGraphTransformer> CreatePqDataSourceConstraintTransformer() {
    return std::make_unique<TPqDataSourceConstraintTransformer>();
}

} // NYql
