#pragma once

#include <ydb/library/yql/dq/proto/dq_tasks.pb.h>
#include <util/generic/maybe.h>
#include <util/generic/strbuf.h>

namespace NFq {

// Type-independent prefix of a MultiHoppingCore version 3 checkpoint.
// Hop and window sizes belong to the program, not the checkpoint.
struct THoppingRecoveryState {
    static constexpr ui32 StateVersion = 3;

    ui64 MinWindowStartIndex = 0;

    static THoppingRecoveryState Read(TStringBuf state);
    static TString MakeRecoveryState(ui64 minWindowStartIndex);
};

// Recovery information is extracted once per stage, independently of task IDs
// and of the key/aggregate types in the old and new query.
struct TStageStateRecoveryInfo {
    struct THoppingSettings {
        ui64 HopTimeUs = 0;
        ui64 WindowSizeUs = 0;
    };
    TMaybe<THoppingSettings> Hopping;
    bool HasWatermarkGenerator = false;

    static TStageStateRecoveryInfo FromProgram(const NYql::NDqProto::TProgram& program);

    // Earliest input needed to produce all hop ends at or after outputStartTimeUs.
    ui64 InputStartForOutput(ui64 outputStartTimeUs) const;
    // Select a complete window whose end precedes the old output boundary.
    ui64 WindowStartBefore(ui64 outputStartTimeUs) const;
};

} // namespace NFq
