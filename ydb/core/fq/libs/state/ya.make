LIBRARY()

PEERDIR(
    ydb/core/fq/libs/checkpointing/events
    ydb/library/actors/core
    ydb/library/yql/dq/actors/compute
    ydb/library/yql/dq/proto
    ydb/library/yql/providers/pq/common
    ydb/library/yql/providers/pq/gateway/abstract
    ydb/library/yql/providers/pq/proto
    ydb/library/yql/providers/pq/task_meta
    yql/essentials/minikql
    yql/essentials/public/issue
    yql/essentials/public/issue/protos
)

SRCS(
    dq_state_load_plan.cpp
    dq_state_load_plan_resolver.cpp
    dq_stage_state_recovery_info.cpp
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
