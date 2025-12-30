cmake_minimum_required(VERSION 3.16)
project(cell VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Library
add_library(cell STATIC
    src/allocator.cpp
    src/context.cpp
    src/arena.cpp
)

target_include_directories(cell PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Tests (optional, requires GTest)
option(CELL_BUILD_TESTS "Build unit tests" ON)

if(CELL_BUILD_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        enable_testing()
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
endif()
