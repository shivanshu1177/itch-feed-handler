cmake_minimum_required(VERSION 3.20)
project(itch_feed_handler VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include helper CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

# CMake options
option(ITCH_BUILD_TESTS "Build unit tests" ON)
option(ITCH_BUILD_BENCH "Build benchmarks" ON)
option(ITCH_BUILD_TOOLS "Build tools" ON)

# ---------------------------------------------------------------------------
# External dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

# toml++ for configuration file parsing
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

# Find packages
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Interface library: propagates include paths to all targets transitively
# ---------------------------------------------------------------------------
add_library(itch_headers INTERFACE)
target_include_directories(itch_headers INTERFACE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/app"
    "${PROJECT_SOURCE_DIR}/include/common"
    "${PROJECT_SOURCE_DIR}/include/concurrency"
    "${PROJECT_SOURCE_DIR}/include/monitoring"
    "${PROJECT_SOURCE_DIR}/include/network"
    "${PROJECT_SOURCE_DIR}/include/orderbook"
    "${PROJECT_SOURCE_DIR}/include/protocol"
    "${PROJECT_SOURCE_DIR}/include/recovery"
    "${PROJECT_SOURCE_DIR}/include/routing"
    "${PROJECT_SOURCE_DIR}/include/session"
    "${PROJECT_SOURCE_DIR}/include/system"
    "${PROJECT_SOURCE_DIR}/include/workers"
    "${tomlplusplus_SOURCE_DIR}/include"
)

# ---------------------------------------------------------------------------
# Core library targets
# ---------------------------------------------------------------------------
add_library(itch_system STATIC src/system/cpu_affinity.cpp)
target_link_libraries(itch_system PUBLIC itch_headers Threads::Threads)
set_project_warnings(itch_system)
target_enable_sanitizers(itch_system)

add_library(itch_monitoring STATIC
    src/monitoring/latency_histogram.cpp
    src/monitoring/metrics_reporter.cpp
)
target_link_libraries(itch_monitoring PUBLIC itch_system)
set_project_warnings(itch_monitoring)
target_enable_sanitizers(itch_monitoring)

add_library(itch_network STATIC src/network/socket_receiver.cpp)
target_link_libraries(itch_network PUBLIC itch_system)
set_project_warnings(itch_network)
target_enable_sanitizers(itch_network)

add_library(itch_protocol STATIC
    src/protocol/moldudp64_parser.cpp
    src/protocol/itch_parser.cpp
    src/protocol/sequence_tracker.cpp
)
target_link_libraries(itch_protocol PUBLIC itch_headers)
set_project_warnings(itch_protocol)
target_enable_sanitizers(itch_protocol)
target_compile_options(itch_protocol PRIVATE
    $<$<CONFIG:Release>:-O3;-march=native;-funroll-loops;-fno-rtti>
)

add_library(itch_routing STATIC
    src/routing/symbol_directory.cpp
    src/routing/shard_router.cpp
)
target_link_libraries(itch_routing PUBLIC itch_protocol)
set_project_warnings(itch_routing)
target_enable_sanitizers(itch_routing)

add_library(itch_orderbook STATIC
    src/orderbook/order_pool.cpp
    src/orderbook/order_map.cpp
    src/orderbook/price_level.cpp
    src/orderbook/level2_book.cpp
    src/orderbook/bbo_publisher.cpp
    src/orderbook/order_book_pool.cpp
)
target_link_libraries(itch_orderbook PUBLIC itch_system)
set_project_warnings(itch_orderbook)
target_enable_sanitizers(itch_orderbook)
target_compile_options(itch_orderbook PRIVATE
    $<$<CONFIG:Release>:-O3;-march=native;-funroll-loops;-fno-rtti>
)

add_library(itch_session STATIC
    src/session/session_manager.cpp
)
target_link_libraries(itch_session PUBLIC itch_routing itch_orderbook itch_protocol)
set_project_warnings(itch_session)
target_enable_sanitizers(itch_session)

add_library(itch_recovery STATIC
    src/recovery/gap_detector.cpp
    src/recovery/retransmit_client.cpp
    src/recovery/book_snapshot.cpp
    src/recovery/persistent_sequence.cpp
)
target_link_libraries(itch_recovery PUBLIC itch_protocol itch_orderbook)
set_project_warnings(itch_recovery)
target_enable_sanitizers(itch_recovery)

add_library(itch_workers STATIC
    src/workers/receiver_thread.cpp
    src/workers/feed_merger.cpp
    src/workers/parser_thread.cpp
    src/workers/shard_worker.cpp
    src/workers/recovery_thread.cpp
)
target_link_libraries(itch_workers PUBLIC
    itch_network
    itch_protocol
    itch_routing
    itch_orderbook
    itch_session
    itch_recovery
    itch_monitoring
    itch_system
)
set_project_warnings(itch_workers)
target_enable_sanitizers(itch_workers)
target_compile_options(itch_workers PRIVATE
    $<$<CONFIG:Release>:-O3;-march=native;-funroll-loops;-fno-rtti>
)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(feed_handler
    src/app/main.cpp
    src/app/feed_handler.cpp
)
target_link_libraries(feed_handler PRIVATE
    itch_workers
    tomlplusplus::tomlplusplus
)
set_project_warnings(feed_handler)
target_enable_sanitizers(feed_handler)

# ---------------------------------------------------------------------------
# Optional targets
# ---------------------------------------------------------------------------
if(ITCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ITCH_BUILD_BENCH)
    add_subdirectory(benchmarks)
endif()

if(ITCH_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
