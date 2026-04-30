#include "yql_kikimr_expr_nodes.h"

#include <ydb/library/yql/dq/expr_nodes/dq_expr_nodes.h>
#include <ydb/library/yql/providers/dq/expr_nodes/dqs_expr_nodes.h>

#include <yql/essentials/providers/common/transform/yql_visit.h>

namespace NYql {

using namespace NNodes;

namespace {

class TKiSourceConstraintsTransformer final : public TVisitorTransformerBase {
public:
    explicit TKiSourceConstraintsTransformer(TIntrusivePtr<TKikimrSessionContext> sessionCtx)
        : TVisitorTransformerBase(true)
        , SessionCtx(sessionCtx)
    {
        AddHandler({
            TKiReadTable::CallableName(),
            TKiReadTableScheme::CallableName(),
            TKiReadTableList::CallableName(),
        }, Hndl(&TKiSourceConstraintsTransformer::HandleDefault));

        if (IsIn({EKikimrQueryType::Query, EKikimrQueryType::Script}, SessionCtx->Query().Type)) {
            AddHandler({TDqSource::CallableName()}, Hndl(&TKiSourceConstraintsTransformer::CopyAllFromSecond));
            AddHandler({
                TDqSourceWrap::CallableName(),
                TDqSourceWideWrap::CallableName(),
                TDqSourceWideBlockWrap::CallableName(),
                TDqReadWrap::CallableName(),
                TDqReadWideWrap::CallableName(),
                TDqReadBlockWideWrap::CallableName(),
                TDqLookupSourceWrap::CallableName(),
            }, Hndl(&TKiSourceConstraintsTransformer::HandleDefault));
        }
    }

private:
    static TStatus HandleDefault(const TExprNode::TPtr& node, TExprContext& ctx) {
        Y_UNUSED(node, ctx);
        return TStatus::Ok;
    }

    static TStatus CopyAllFromSecond(const TExprNode::TPtr& node, TExprContext& ctx) {
        Y_UNUSED(ctx);
        node->CopyConstraints(*node->Child(1));
        return TStatus::Ok;
    }

    const TIntrusivePtr<TKikimrSessionContext> SessionCtx;
};

} // anonymous namespace

std::unique_ptr<IGraphTransformer> CreateKiSourceConstraintsTransformer(TIntrusivePtr<TKikimrSessionContext> sessionCtx) {
    return std::make_unique<TKiSourceConstraintsTransformer>(sessionCtx);
}

} // NYql
