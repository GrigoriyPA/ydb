PY23_LIBRARY()

PY_SRCS(
    __init__.py
)

PEERDIR(
    library/python/windows
)

END()

RECURSE_FOR_TESTS(
    ut
)
