add_executable(warpdes_rollback_anti_test
	rollback_anti_test.cpp
)
target_link_libraries(warpdes_rollback_anti_test PRIVATE warpdes)
add_test(NAME warpdes.rollback_anti COMMAND warpdes_rollback_anti_test)

add_executable(warpdes_anti_before_event_test
	anti_before_event_test.cpp
)
target_link_libraries(warpdes_anti_before_event_test PRIVATE warpdes)
add_test(NAME warpdes.anti_before_event COMMAND warpdes_anti_before_event_test)

add_executable(warpdes_entity_snapshot_rollback_test
	entity_snapshot_rollback_test.cpp
)
target_link_libraries(warpdes_entity_snapshot_rollback_test PRIVATE warpdes)
add_test(NAME warpdes.entity_snapshot_rollback COMMAND warpdes_entity_snapshot_rollback_test)

add_executable(warpdes_fossil_collection_test
	fossil_collection_test.cpp
)
target_link_libraries(warpdes_fossil_collection_test PRIVATE warpdes)
add_test(NAME warpdes.fossil_collection COMMAND warpdes_fossil_collection_test)

add_executable(warpdes_gvt_safe_migration_test
	gvt_safe_migration_test.cpp
)
target_link_libraries(warpdes_gvt_safe_migration_test PRIVATE warpdes)
add_test(NAME warpdes.gvt_safe_migration COMMAND warpdes_gvt_safe_migration_test)

add_executable(warpdes_optimistic_migration_directory_test
	optimistic_migration_directory_test.cpp
)
target_link_libraries(warpdes_optimistic_migration_directory_test PRIVATE warpdes)
add_test(NAME warpdes.optimistic_migration_directory COMMAND warpdes_optimistic_migration_directory_test)

if(WARPSIM_ENABLE_MPI_TESTS)
	add_executable(warpdes_mpi_smoke_test
		mpi_smoke_test.cpp
	)
	target_link_libraries(warpdes_mpi_smoke_test PRIVATE warpdes MPI::MPI_CXX)
	add_test(
		NAME warpdes.mpi_smoke
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_smoke_test> ${MPIEXEC_POSTFLAGS}
	)

	add_executable(warpdes_mpi_transport_delivery_test
		mpi_transport_delivery_test.cpp
	)
	target_link_libraries(warpdes_mpi_transport_delivery_test PRIVATE warpdes MPI::MPI_CXX)
	add_test(
		NAME warpdes.mpi_transport_delivery
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_transport_delivery_test> ${MPIEXEC_POSTFLAGS}
	)

	add_executable(warpdes_mpi_multirank_gvt_termination_test
		mpi_multirank_gvt_termination_test.cpp
	)
	target_link_libraries(warpdes_mpi_multirank_gvt_termination_test PRIVATE warpdes MPI::MPI_CXX)
	add_test(
		NAME warpdes.mpi_multirank_gvt_termination
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_multirank_gvt_termination_test> ${MPIEXEC_POSTFLAGS}
	)

	add_executable(warpdes_mpi_fossil_collection_test
		mpi_fossil_collection_test.cpp
	)
	target_link_libraries(warpdes_mpi_fossil_collection_test PRIVATE warpdes MPI::MPI_CXX)
	add_test(
		NAME warpdes.mpi_fossil_collection
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_fossil_collection_test> ${MPIEXEC_POSTFLAGS}
	)
endif()
