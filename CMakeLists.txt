cmake_minimum_required(VERSION 3.16)
project(pollnet VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(POLLNET_BUILD_EXAMPLES "Build pollnet examples" ON)
option(POLLNET_BUILD_TESTS "Build pollnet tests" ON)
option(POLLNET_BUILD_BENCHMARKS "Build pollnet benchmarks" ON)

add_library(pollnet INTERFACE)
target_include_directories(pollnet INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(pollnet INTERFACE cxx_std_20)
if(WIN32)
    target_link_libraries(pollnet INTERFACE ws2_32)
endif()

if(POLLNET_BUILD_EXAMPLES)
    foreach(target tcp_server tcp_client udp_server udp_client)
        add_executable(${target} examples/${target}.cpp)
        target_link_libraries(${target} PRIVATE pollnet)
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /utf-8>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
        )
    endforeach()
endif()

include(CTest)
if(POLLNET_BUILD_TESTS AND BUILD_TESTING)
    add_executable(test_pollnet tests/test_pollnet.cpp)
    target_link_libraries(test_pollnet PRIVATE pollnet)
    target_compile_options(test_pollnet PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /utf-8>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
    )
    add_test(NAME pollnet_tests COMMAND test_pollnet)
endif()

if(POLLNET_BUILD_BENCHMARKS)
    find_package(Threads REQUIRED)
    add_executable(pollnet_bench benchmarks/bench_v2.cpp)
    target_include_directories(pollnet_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks)
    target_link_libraries(pollnet_bench PRIVATE pollnet Threads::Threads)
    target_compile_options(pollnet_bench PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /utf-8>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
    )
endif()
