# Installs the current build tree into a temp prefix and then configures/builds
# a tiny consumer project that uses find_package(warpsim CONFIG).

# Allow either Warpsim_* (common CMake style) or warpsim_* (script style).
if (DEFINED Warpsim_BUILD_DIR AND NOT DEFINED warpsim_BUILD_DIR)
  set(warpsim_BUILD_DIR "${Warpsim_BUILD_DIR}")
endif()
if (DEFINED Warpsim_SOURCE_DIR AND NOT DEFINED warpsim_SOURCE_DIR)
  set(warpsim_SOURCE_DIR "${Warpsim_SOURCE_DIR}")
endif()
if (DEFINED Warpsim_INSTALL_PREFIX AND NOT DEFINED warpsim_INSTALL_PREFIX)
  set(warpsim_INSTALL_PREFIX "${Warpsim_INSTALL_PREFIX}")
endif()

if (NOT DEFINED warpsim_BUILD_DIR)
  message(FATAL_ERROR "warpsim_BUILD_DIR is required")
endif()
if (NOT DEFINED warpsim_SOURCE_DIR)
  message(FATAL_ERROR "warpsim_SOURCE_DIR is required")
endif()
if (NOT DEFINED warpsim_INSTALL_PREFIX)
  message(FATAL_ERROR "warpsim_INSTALL_PREFIX is required")
endif()

set(_prefix "${warpsim_INSTALL_PREFIX}")
set(_consumer_src "${warpsim_SOURCE_DIR}/tests/cmake_integration/consumer")
set(_consumer_build "${warpsim_BUILD_DIR}/_cmake_consumer_build")

file(REMOVE_RECURSE "${_prefix}")
file(MAKE_DIRECTORY "${_prefix}")
file(REMOVE_RECURSE "${_consumer_build}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${warpsim_BUILD_DIR}" --prefix "${_prefix}"
  RESULT_VARIABLE _install_rc
)
if (NOT _install_rc EQUAL 0)
  message(FATAL_ERROR "cmake --install failed with ${_install_rc}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${_consumer_src}" -B "${_consumer_build}" -DCMAKE_PREFIX_PATH=${_prefix} -Dwarpsim_DIR=${_prefix}/lib/cmake/warpsim
  RESULT_VARIABLE _cfg_rc
)
if (NOT _cfg_rc EQUAL 0)
  message(FATAL_ERROR "consumer configure failed with ${_cfg_rc}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build}"
  RESULT_VARIABLE _build_rc
)
if (NOT _build_rc EQUAL 0)
  message(FATAL_ERROR "consumer build failed with ${_build_rc}")
endif()
