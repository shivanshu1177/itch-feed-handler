include(FetchContent)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.4
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(bench_itch
    bench_level2_book.cpp
    bench_itch_parser.cpp
    bench_spsc_queue.cpp
    bench_seqlock.cpp
    bench_order_map.cpp
)

target_link_libraries(bench_itch PRIVATE
    itch_protocol
    itch_orderbook
    itch_monitoring
    itch_headers
    benchmark::benchmark_main
)

target_compile_options(bench_itch PRIVATE
    $<$<CONFIG:Release>:-O3;-march=native;-funroll-loops;-fno-rtti>
    $<$<CONFIG:Debug>:-O2;-g>
)
