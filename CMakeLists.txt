cmake_minimum_required(VERSION 3.16)
project(cell VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Library
add_library(cell STATIC
    src/allocator.cpp
    src/context.cpp
    src/arena.cpp
    src/buddy.cpp
    src/debug.cpp
    src/large.cpp
)

target_include_directories(cell PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Memory statistics (compile-time optional)
option(CELL_ENABLE_STATS "Enable memory statistics tracking" OFF)
if(CELL_ENABLE_STATS)
    target_compile_definitions(cell PUBLIC CELL_ENABLE_STATS)
    message(STATUS "Cell: Memory statistics enabled")
endif()

# Debug features (compile-time optional)
option(CELL_DEBUG_GUARDS "Enable guard bytes for bounds checking" OFF)
option(CELL_DEBUG_STACKTRACE "Enable stack trace capture on allocation" OFF)
option(CELL_DEBUG_LEAKS "Enable leak detection" OFF)

if(CELL_DEBUG_GUARDS)
    target_compile_definitions(cell PUBLIC CELL_DEBUG_GUARDS)
    message(STATUS "Cell: Guard bytes enabled")
endif()

if(CELL_DEBUG_STACKTRACE)
    target_compile_definitions(cell PUBLIC CELL_DEBUG_STACKTRACE)
    message(STATUS "Cell: Stack trace capture enabled")
endif()

if(CELL_DEBUG_LEAKS)
    target_compile_definitions(cell PUBLIC CELL_DEBUG_LEAKS)
    message(STATUS "Cell: Leak detection enabled")
endif()

# Memory budget limits (compile-time optional)
option(CELL_ENABLE_BUDGET "Enable memory budget limits" OFF)
if(CELL_ENABLE_BUDGET)
    target_compile_definitions(cell PUBLIC CELL_ENABLE_BUDGET)
    message(STATUS "Cell: Memory budget limits enabled")
endif()

# Instrumentation callbacks (compile-time optional)
option(CELL_ENABLE_INSTRUMENTATION "Enable allocation callbacks for instrumentation" OFF)
if(CELL_ENABLE_INSTRUMENTATION)
    target_compile_definitions(cell PUBLIC CELL_ENABLE_INSTRUMENTATION)
    message(STATUS "Cell: Instrumentation callbacks enabled")
endif()

# Tests (optional, requires GTest)
option(CELL_BUILD_TESTS "Build unit tests" ON)

if(CELL_BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_executable(cell_tests tests/test_context.cpp)
        target_link_libraries(cell_tests PRIVATE cell GTest::gtest_main)
        add_test(NAME cell_tests COMMAND cell_tests)
    else()
        message(STATUS "GTest not found, skipping GTest-based tests")
    endif()

    # Standalone allocator test (no GTest required)
    add_executable(test_allocator tests/test_allocator.cpp)
    target_link_libraries(test_allocator PRIVATE cell)
    add_test(NAME test_allocator COMMAND test_allocator)

    # Sub-cell allocator test
    add_executable(test_sub_cell tests/test_sub_cell.cpp)
    target_link_libraries(test_sub_cell PRIVATE cell)
    add_test(NAME test_sub_cell COMMAND test_sub_cell)

    # Arena allocator test
    add_executable(test_arena tests/test_arena.cpp)
    target_link_libraries(test_arena PRIVATE cell)
    add_test(NAME test_arena COMMAND test_arena)

    # Pool<T> and ArenaScope test
    add_executable(test_pool tests/test_pool.cpp)
    target_link_libraries(test_pool PRIVATE cell)
    add_test(NAME test_pool COMMAND test_pool)

    # Buddy and large allocation test
    add_executable(test_buddy tests/test_buddy.cpp)
    target_link_libraries(test_buddy PRIVATE cell)
    add_test(NAME test_buddy COMMAND test_buddy)

    # Memory statistics test
    add_executable(test_stats tests/test_stats.cpp)
    target_link_libraries(test_stats PRIVATE cell)
    add_test(NAME test_stats COMMAND test_stats)

    # Debug features test
    add_executable(test_debug tests/test_debug.cpp)
    target_link_libraries(test_debug PRIVATE cell)
    add_test(NAME test_debug COMMAND test_debug)

    # Large allocation test
    add_executable(test_large tests/test_large.cpp)
    target_link_libraries(test_large PRIVATE cell)
    add_test(NAME test_large COMMAND test_large)

    # Memory budget test
    add_executable(test_budget tests/test_budget.cpp)
    target_link_libraries(test_budget PRIVATE cell)
    add_test(NAME test_budget COMMAND test_budget)

    # Instrumentation callbacks test
    add_executable(test_instrumentation tests/test_instrumentation.cpp)
    target_link_libraries(test_instrumentation PRIVATE cell)
    add_test(NAME test_instrumentation COMMAND test_instrumentation)

    # Stress tests
    add_executable(test_stress tests/test_stress.cpp)
    target_link_libraries(test_stress PRIVATE cell)
    add_test(NAME test_stress COMMAND test_stress)

    # Fuzzing stress tests
    add_executable(test_fuzz tests/test_fuzz.cpp)
    target_link_libraries(test_fuzz PRIVATE cell)
    add_test(NAME test_fuzz COMMAND test_fuzz)
endif()

# Benchmarks (optional, requires Google Benchmark)
option(CELL_BUILD_BENCHMARKS "Build performance benchmarks" OFF)

if(CELL_BUILD_BENCHMARKS)
    # Try to find system-installed Google Benchmark, otherwise fetch it
    find_package(benchmark QUIET)

    if(NOT benchmark_FOUND)
        message(STATUS "Cell: Google Benchmark not found, fetching from GitHub...")
        include(FetchContent)
        FetchContent_Declare(
            benchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG v1.8.3
        )
        # Disable benchmark tests to speed up build
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(benchmark)
    endif()

    # Check for jemalloc
    find_library(JEMALLOC_LIB jemalloc)
    find_path(JEMALLOC_INCLUDE jemalloc/jemalloc.h)

    set(BENCHMARK_SOURCES
        benchmarks/bench_main.cpp
        benchmarks/bench_alloc_patterns.cpp
        benchmarks/bench_baseline.cpp
        benchmarks/bench_threading.cpp
        benchmarks/bench_abstractions.cpp
        benchmarks/bench_locality.cpp
    )

    set(BENCHMARK_LIBS
        cell
        benchmark::benchmark
    )

    add_executable(cell_benchmarks ${BENCHMARK_SOURCES})

    target_link_libraries(cell_benchmarks PRIVATE ${BENCHMARK_LIBS})

    # Ensure benchmarks run with optimizations
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(cell_benchmarks PRIVATE -O3 -DNDEBUG)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        target_compile_options(cell_benchmarks PRIVATE /O2 /DNDEBUG)
    endif()

    message(STATUS "Cell: Benchmarks enabled")
endif()

