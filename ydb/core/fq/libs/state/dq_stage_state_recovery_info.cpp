#include "dq_stage_state_recovery_info.h"

#include <yql/essentials/minikql/comp_nodes/mkql_saveload.h>
#include <yql/essentials/minikql/mkql_alloc.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/minikql/mkql_node_serialization.h>
#include <yql/essentials/minikql/mkql_node_visitor.h>
#include <yql/essentials/utils/yql_panic.h>

namespace NFq {

using namespace NKikimr::NMiniKQL;

THoppingRecoveryState THoppingRecoveryState::Read(TStringBuf state) {
    TInputSerializer in(state, EMkqlStateType::SIMPLE_BLOB);
    YQL_ENSURE(in.GetStateVersion() == StateVersion, "Hopping checkpoint has no recovery metadata");
    THoppingRecoveryState result;
    in(result.MinWindowStartIndex);
    return result;
}

TString THoppingRecoveryState::MakeRecoveryState(ui64 minWindowStartIndex) {
    TString result;
    WriteUi32(result, static_cast<ui32>(EMkqlStateType::SIMPLE_BLOB));
    WriteUi32(result, StateVersion);
    WriteUi64(result, minWindowStartIndex); // Lower bound for complete windows, including new keys.
    WriteUi32(result, 0); // No keys or aggregate values are transferred.
    WriteBool(result, false); // Finished.
    return result;
}

TStageStateRecoveryInfo TStageStateRecoveryInfo::FromProgram(const NYql::NDqProto::TProgram& program) {
    YQL_ENSURE(program.GetRuntimeVersion() == NYql::NDqProto::RUNTIME_VERSION_YQL_1_0,
        "Unsupported program runtime for history replay");
    TScopedAlloc alloc(__LOCATION__);
    TTypeEnvironment env(alloc);
    const auto root = DeserializeRuntimeNode(program.GetRaw(), env);
    TExploringNodeVisitor explorer;
    explorer.Walk(root.GetNode(), env);
    TStageStateRecoveryInfo result;
    for (const auto* node : explorer.GetNodes()) {
        if (!node->GetType()->IsCallable()) {
            continue;
        }
        const auto& callable = static_cast<const TCallable&>(*node);
        const TStringBuf name = callable.GetType()->GetName();
        if (name == "MultiHoppingCore") {
            YQL_ENSURE(!result.Hopping, "History replay supports at most one hopping operator per stage");
            YQL_ENSURE(callable.GetInputsCount() > 20, "Unsupported hopping operator");
            const auto readInterval = [&](ui32 index) {
                const auto input = callable.GetInput(index);
                YQL_ENSURE(input.IsImmediate() && input.GetStaticType()->IsData(),
                    "History replay requires constant hopping intervals");
                const auto value = AS_VALUE(TDataLiteral, input)->AsValue().Get<i64>();
                YQL_ENSURE(value > 0, "Invalid hopping interval");
                return static_cast<ui64>(value);
            };
            const auto watermarkMode = callable.GetInput(20);
            YQL_ENSURE(watermarkMode.IsImmediate() && watermarkMode.GetStaticType()->IsData()
                && AS_VALUE(TDataLiteral, watermarkMode)->AsValue().Get<bool>(),
                "History replay requires watermark-driven hopping");
            YQL_ENSURE(callable.GetInputsCount() > 25, "Hopping minimum window start checking is not enabled");
            const auto checkMinWindowStart = callable.GetInput(25);
            YQL_ENSURE(checkMinWindowStart.IsImmediate() && checkMinWindowStart.GetStaticType()->IsData()
                && AS_VALUE(TDataLiteral, checkMinWindowStart)->AsValue().Get<bool>(),
                "Hopping minimum window start checking is not enabled");
            auto& hopping = result.Hopping.ConstructInPlace();
            hopping.HopTimeUs = readInterval(16);
            hopping.WindowSizeUs = readInterval(17);
            YQL_ENSURE(hopping.WindowSizeUs >= hopping.HopTimeUs
                && hopping.WindowSizeUs % hopping.HopTimeUs == 0, "Invalid hopping window");
        } else if (name == "DqWatermarkGenerator") {
            result.HasWatermarkGenerator = true;
        } else {
            if (callable.GetInputsCount() && (callable.GetInput(0).GetStaticType()->IsFlow()
                    || callable.GetInput(0).GetStaticType()->IsStream())) {
                TStringBuf operation = name;
                operation.SkipPrefix("Wide");
                operation.ChopSuffix("Inclusive");
                operation.ChopSuffix("WithSpilling");
                operation.ChopSuffix("Blocks");
                for (const TStringBuf stateful : {"ChainMap", "Chain1Map", "MapNext", "Enumerate", "Fold", "Fold1",
                        "Take", "Skip", "TakeWhile", "SkipWhile", "Collect", "Reduce", "Length", "HasItems",
                        "Head", "Last", "ToOptional", "Top", "TopSort", "Sort"}) {
                    YQL_ENSURE(operation != stateful, "Unsupported stateful operator for history replay: " << name);
                }
            }
            // These operators keep streaming state outside hopping windows.
            // Their state cannot be reconstructed using a window start bound.
            for (const TStringBuf stateful : {"Condense", "WideCondense", "CombineCore", "WideCombiner", "WideLastCombiner", "BlockCombine", "BlockMerge",
                    "Squeeze", "Chopper", "HoppingCore", "TimeOrderRecover", "MatchRecognize", "JoinCore", "GraceJoin", "GraceSelfJoin"}) {
                YQL_ENSURE(!name.Contains(stateful), "Unsupported stateful operator for history replay: " << name);
            }
        }
    }
    return result;
}

ui64 TStageStateRecoveryInfo::InputStartForOutput(ui64 outputStartTimeUs) const {
    if (!Hopping) {
        return outputStartTimeUs;
    }
    const auto hop = Hopping->HopTimeUs;
    const auto remainder = outputStartTimeUs % hop;
    YQL_ENSURE(!remainder || outputStartTimeUs <= Max<ui64>() - (hop - remainder), "Hopping recovery time overflow");
    const auto end = outputStartTimeUs + (remainder ? hop - remainder : 0);
    return end > Hopping->WindowSizeUs ? end - Hopping->WindowSizeUs : 0;
}

ui64 TStageStateRecoveryInfo::WindowStartBefore(ui64 outputStartTimeUs) const {
    YQL_ENSURE(Hopping, "Missing hopping recovery boundary");
    if (!outputStartTimeUs) {
        return 0; // No old event-time frontier: replay all retained history.
    }
    const auto end = (outputStartTimeUs - 1) / Hopping->HopTimeUs * Hopping->HopTimeUs;
    return end > Hopping->WindowSizeUs ? end - Hopping->WindowSizeUs : 0;
}

} // namespace NFq
