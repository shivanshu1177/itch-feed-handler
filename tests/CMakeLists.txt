include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
# Prevent overriding parent project compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(test_itch
    test_itch_parser.cpp
    test_sequence_tracker.cpp
    test_order_pool.cpp
    test_order_map.cpp
    test_level2_book.cpp
    test_shard_router.cpp
    test_bbo_publisher.cpp
    test_latency_histogram.cpp
    test_seqlock.cpp
    test_spsc_queue.cpp
)

target_link_libraries(test_itch PRIVATE
    itch_protocol
    itch_orderbook
    itch_routing
    itch_monitoring
    itch_headers
    GTest::gtest_main
)

target_compile_options(test_itch PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-O2 -g>
)

include(GoogleTest)
gtest_discover_tests(test_itch)
