cmake_minimum_required(VERSION 3.20)

project(warpsim_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(warpsim CONFIG REQUIRED)

add_executable(warpsim_consumer main.cpp)

# This should provide include dirs for the installed headers.
target_link_libraries(warpsim_consumer PRIVATE warpsim::warpsim)
