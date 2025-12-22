add_library(warpdes INTERFACE)

target_include_directories(warpdes
	INTERFACE
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/warpsim>
)

# Consumer-facing target name for find_package(warpsim).
add_library(warpsim INTERFACE)
target_link_libraries(warpsim INTERFACE warpdes)

install(
	TARGETS warpdes warpsim
	EXPORT warpsimTargets
)

install(
	DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/"
	DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/warpsim"
	FILES_MATCHING
	PATTERN "*.hpp"
)

install(
	EXPORT warpsimTargets
	NAMESPACE warpsim::
	DESTINATION "${WARPSIM_INSTALL_CMAKEDIR}"
)
