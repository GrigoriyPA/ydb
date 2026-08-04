#pragma once

#include <yql/essentials/minikql/mkql_alloc.h>
#include <yql/essentials/minikql/mkql_node.h>

#include <util/datetime/base.h>
#include <util/generic/fwd.h>

#include <memory>
#include <optional>

namespace NYql {

namespace NDqProto {

class TProgram;

} // namespace NDqProto

namespace NDq {

class TStageStateRecoveryInfo {
    struct TRecoveryInfo {
        // Series of all available timestamps: {Timestamp, Timestamp - Step, Timestamp - 2 * Step, ...}
        // from which reading may be started to recover operators state in at-least once semantic.
        // NOTE: now it is optimized only for hopping aggregation representation.
        // NOTE: for hopping it may be more accurately to represent available timestamps as top-K time series 
        //       in cases of non-multiple windows in nested/multi-branch hopping aggregations.
        TInstant Timestamp;
        TDuration Step;
    };

public:
    struct TProgramInfo {
        // Hopping aggregation info (only supported recovery state case now)
        TDuration HopStep;
        TDuration HopWindowSize;
    };

    struct TContext {
        using TPtr = std::shared_ptr<TContext>;

        TContext();

        NKikimr::NMiniKQL::TScopedAlloc Alloc;
        NKikimr::NMiniKQL::TTypeEnvironment TypeEnv;
    };

    // Check that stage program has state and this state may be recovered by rollback to old event times
    bool FillProgramInfo(const NYql::NDqProto::TProgram& program, TContext::TPtr ctx, TString& error);

private:
    std::optional<TRecoveryInfo> Recovery; // Nullopt if stage and dependent stages has no state
    std::optional<TProgramInfo> ProgramInfo; // Nullopt if stage has no stateful operators
};

} // namespace NDq

} // namespace NYql
