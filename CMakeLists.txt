cmake_minimum_required(VERSION 3.10.0)
project(pollnet VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
add_executable(tcp_server tcp_server.cpp)
add_executable(tcp_client tcp_client.cpp)
add_executable(udp_server udp_server.cpp)
add_executable(udp_client udp_client.cpp)

