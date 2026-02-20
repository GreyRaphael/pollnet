cmake_minimum_required(VERSION 3.10.0)
project(pollnet VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
add_executable(server server.cpp)
add_executable(client client.cpp)

