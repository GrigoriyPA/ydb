#include "dq_stage_state_recovery_info.h"

#include <ydb/library/yql/dq/proto/dq_tasks.pb.h>
#include <ydb/library/yverify_stream/yverify_stream.h>

#include <yql/essentials/minikql/mkql_node_serialization.h>
#include <yql/essentials/minikql/mkql_node_visitor.h>

#include <util/generic/string.h>
#include <util/generic/strbuf.h>

#include <array>
#include <optional>

namespace NYql::NDq {

using namespace NKikimr;
using namespace NKikimr::NMiniKQL;

namespace {

class TStageStateCheckVisitor final : public TEmptyNodeVisitor {
    static constexpr TStringBuf MULTI_HOPPING_NODE = "MultiHoppingCore";
    static constexpr std::array<TStringBuf, 3> STATEFUL_NODES = {
        "",
        "",
        ""
    };

public:
    explicit TStageStateCheckVisitor(TString& error)
        : Error(error)
    {}

    void Visit(TCallable& node) final {
        const auto* nodeType = node.GetType();
        Y_VALIDATE(nodeType, "Missing node type");

        const auto& name = nodeType->GetName();
    }

private:
    TString& Error;
    std::optional<TStageStateRecoveryInfo::TProgramInfo> ProgramInfo;
};

} // anonymous namespace

TStageStateRecoveryInfo::TContext::TContext()
    : Alloc(__LOCATION__, TAlignedPagePoolCounters(), /* supportsSizedAllocators */ false, /* initiallyAcquired */ false)
    , TypeEnv(Alloc)
{}

bool TStageStateRecoveryInfo::FillProgramInfo(const NDqProto::TProgram& program, TContext::TPtr ctx, TString& error) {
    const auto runtimeVersion = program.GetRuntimeVersion();
    Y_VALIDATE(runtimeVersion, "Missing program runtime version");
    Y_VALIDATE(runtimeVersion <= NDqProto::ERuntimeVersion::RUNTIME_VERSION_YQL_1_0, "Unexpected program runtime version");

    TRuntimeNode programNode;
    {
        auto guard = ctx->TypeEnv.BindAllocator();
        programNode = DeserializeRuntimeNode(program.GetRaw(), ctx->TypeEnv);
    }

    Y_VALIDATE(programNode.IsImmediate() && programNode.GetNode() && programNode.GetNode()->GetType() && programNode.GetNode()->GetType()->IsStruct(), "Unexpected program node type");
    const auto& programStruct = static_cast<TStructLiteral&>(*programNode.GetNode());
    const auto programType = programStruct.GetType();
    Y_VALIDATE(programType, "Missing program type");
    const auto programRootIdx = programType->FindMemberIndex("Program");
    Y_VALIDATE(programRootIdx, "Missing program root");
    const TRuntimeNode programRoot = programStruct.GetValue(*programRootIdx);

    TExploringNodeVisitor programExplorer;
    programExplorer.Walk(programRoot.GetNode(), ctx->TypeEnv);
}

} // namespace NYql::NDq
