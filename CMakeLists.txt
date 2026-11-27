cmake_minimum_required(VERSION 3.16)
project(pollnet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(pollnet INTERFACE)
target_include_directories(pollnet INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pollnet INTERFACE cxx_std_20)
if(WIN32)
    target_link_libraries(pollnet INTERFACE ws2_32)
endif()

foreach(target tcp_server tcp_client udp_server udp_client)
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} PRIVATE pollnet)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /utf-8>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
    )
endforeach()
