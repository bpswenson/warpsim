cmake_minimum_required(VERSION 3.20)

project(warpdes VERSION 1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(WARPSIM_ENABLE_MPI_TESTS "Enable MPI-based tests" ON)
option(WARPSIM_ENABLE_EXAMPLE_SMOKE_TESTS "Run example binaries as CTest smoke tests" OFF)

enable_testing()

if(WARPSIM_ENABLE_MPI_TESTS)
	find_package(MPI REQUIRED COMPONENTS CXX)
endif()

add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(examples)

