LIBRARY()

PEERDIR(
    ydb/library/actors/core
    ydb/library/services
    ydb/library/yql/dq/actors/compute
    ydb/library/yql/dq/proto
    ydb/library/yql/providers/pq/proto
    ydb/library/yql/providers/pq/task_meta
    ydb/library/yverify_stream
    yql/essentials/minikql
    yql/essentials/public/issue
    yql/essentials/public/issue/protos
)

SRCS(
    dq_stage_state_recovery_info.cpp
    dq_state_load_plan.cpp
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
