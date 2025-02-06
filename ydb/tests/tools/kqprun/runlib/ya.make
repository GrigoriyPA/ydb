LIBRARY()

SRCS(
    application.cpp
    utils.cpp
)

PEERDIR(
    library/cpp/colorizer
    library/cpp/getopt
    util
    ydb/core/base
    ydb/core/protos
    ydb/library/actors/core
    ydb/library/actors/testlib
    ydb/library/services
    ydb/public/api/protos
    ydb/public/lib/json_value
    yql/essentials/public/issue
)

YQL_LAST_ABI_VERSION()

END()
