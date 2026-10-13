cmake_minimum_required(VERSION 3.20)
project(TaskForge VERSION 1.0.0 LANGUAGES CXX)

option(TASKFORGE_BUILD_TESTS "Build TaskForge correctness tests" ON)
option(TASKFORGE_BUILD_BENCHMARKS "Build the TaskForge benchmark executable" ON)

find_package(Threads REQUIRED)

add_library(taskforge src/scheduler.cpp)
add_library(TaskForge::taskforge ALIAS taskforge)
target_include_directories(taskforge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(taskforge PUBLIC cxx_std_20)
target_link_libraries(taskforge PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(taskforge PRIVATE /W4 /permissive-)
else()
    target_compile_options(taskforge PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(taskforge_example examples/basic_usage.cpp)
target_link_libraries(taskforge_example PRIVATE TaskForge::taskforge)

if(TASKFORGE_BUILD_TESTS)
    enable_testing()
    add_executable(taskforge_tests tests/scheduler_tests.cpp)
    target_link_libraries(taskforge_tests PRIVATE TaskForge::taskforge)
    add_test(NAME taskforge_tests COMMAND taskforge_tests)
endif()

if(TASKFORGE_BUILD_BENCHMARKS)
    add_executable(taskforge_benchmark benchmarks/scheduler_benchmark.cpp)
    target_link_libraries(taskforge_benchmark PRIVATE TaskForge::taskforge)
endif()
