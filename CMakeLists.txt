cmake_minimum_required(VERSION 3.20)

project(warpdes VERSION 1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Package install location for downstream consumers (find_package(warpsim CONFIG)).
set(WARPSIM_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/warpsim")

option(WARPSIM_ENABLE_MPI_TESTS "Enable MPI-based tests" ON)
option(WARPSIM_ENABLE_EXAMPLE_SMOKE_TESTS "Run example binaries as CTest smoke tests" OFF)

option(WARPSIM_ENABLE_ASAN_UBSAN "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(WARPSIM_ENABLE_TSAN "Enable ThreadSanitizer (recommended with WARPSIM_ENABLE_MPI_TESTS=OFF)" OFF)

if(WARPSIM_ENABLE_ASAN_UBSAN AND WARPSIM_ENABLE_TSAN)
	message(FATAL_ERROR "Enable either WARPSIM_ENABLE_ASAN_UBSAN or WARPSIM_ENABLE_TSAN, not both")
endif()

if(WARPSIM_ENABLE_ASAN_UBSAN)
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
		add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
		add_link_options(-fsanitize=address,undefined)
	else()
		message(WARNING "WARPSIM_ENABLE_ASAN_UBSAN is set but compiler is not Clang/GNU")
	endif()
endif()

if(WARPSIM_ENABLE_TSAN)
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
		add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
		add_link_options(-fsanitize=thread)
	else()
		message(WARNING "WARPSIM_ENABLE_TSAN is set but compiler is not Clang/GNU")
	endif()
endif()

enable_testing()

if(WARPSIM_ENABLE_MPI_TESTS)
	find_package(MPI REQUIRED COMPONENTS CXX)
endif()

add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(examples)

configure_package_config_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/warpsimConfig.cmake.in"
	"${CMAKE_CURRENT_BINARY_DIR}/warpsimConfig.cmake"
	INSTALL_DESTINATION "${WARPSIM_INSTALL_CMAKEDIR}"
)

write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/warpsimConfigVersion.cmake"
	VERSION "${PROJECT_VERSION}"
	COMPATIBILITY SameMajorVersion
)

install(
	FILES
		"${CMAKE_CURRENT_BINARY_DIR}/warpsimConfig.cmake"
		"${CMAKE_CURRENT_BINARY_DIR}/warpsimConfigVersion.cmake"
	DESTINATION "${WARPSIM_INSTALL_CMAKEDIR}"
)

