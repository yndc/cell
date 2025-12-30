cmake_minimum_required(VERSION 3.16)
project(cell VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Library
add_library(cell STATIC
    src/context.cpp
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
        message(STATUS "GTest not found, skipping tests")
    endif()
endif()
