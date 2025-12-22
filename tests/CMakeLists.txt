add_compile_definitions(WARPSIM_ENABLE_INVARIANT_CHECKS_DEFAULT=1)

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

add_executable(warpdes_migration_wire_roundtrip_test
	migration_wire_roundtrip_test.cpp
)
target_link_libraries(warpdes_migration_wire_roundtrip_test PRIVATE warpdes)
add_test(NAME warpdes.migration_wire_roundtrip COMMAND warpdes_migration_wire_roundtrip_test)

add_executable(warpdes_optimistic_migration_payloads_test
	optimistic_migration_payloads_test.cpp
)
target_link_libraries(warpdes_optimistic_migration_payloads_test PRIVATE warpdes)
add_test(NAME warpdes.optimistic_migration_payloads COMMAND warpdes_optimistic_migration_payloads_test)

add_executable(warpdes_directory_lp_unit_test
	directory_lp_unit_test.cpp
)
target_link_libraries(warpdes_directory_lp_unit_test PRIVATE warpdes)
add_test(NAME warpdes.directory_lp_unit COMMAND warpdes_directory_lp_unit_test)

add_executable(warpdes_committed_event_trace_determinism_test
	committed_event_trace_determinism_test.cpp
)
target_link_libraries(warpdes_committed_event_trace_determinism_test PRIVATE warpdes)
add_test(NAME warpdes.committed_event_trace_determinism COMMAND warpdes_committed_event_trace_determinism_test)

add_executable(warpdes_uid_determinism_test
	uid_determinism_test.cpp
)
target_link_libraries(warpdes_uid_determinism_test PRIVATE warpdes)
add_test(NAME warpdes.uid_determinism COMMAND warpdes_uid_determinism_test)

add_executable(warpdes_run_one_rollback_test
	run_one_rollback_test.cpp
)
target_link_libraries(warpdes_run_one_rollback_test PRIVATE warpdes)
add_test(NAME warpdes.run_one_rollback COMMAND warpdes_run_one_rollback_test)

add_executable(warpdes_committed_output_test
	committed_output_test.cpp
)
target_link_libraries(warpdes_committed_output_test PRIVATE warpdes)
add_test(NAME warpdes.committed_output COMMAND warpdes_committed_output_test)

add_executable(warpdes_randomized_rollback_stress_determinism_test
	randomized_rollback_stress_determinism_test.cpp
)
target_link_libraries(warpdes_randomized_rollback_stress_determinism_test PRIVATE warpdes)
add_test(NAME warpdes.randomized_rollback_stress_determinism COMMAND warpdes_randomized_rollback_stress_determinism_test)

add_executable(warpdes_rollback_snapshot_oldest_anti_test
	rollback_snapshot_oldest_anti_test.cpp
)
target_link_libraries(warpdes_rollback_snapshot_oldest_anti_test PRIVATE warpdes)
add_test(NAME warpdes.rollback_snapshot_oldest_anti COMMAND warpdes_rollback_snapshot_oldest_anti_test)

add_executable(warpdes_timestamp_sequence_autofill_test
	timestamp_sequence_autofill_test.cpp
)
target_link_libraries(warpdes_timestamp_sequence_autofill_test PRIVATE warpdes)
add_test(NAME warpdes.timestamp_sequence_autofill COMMAND warpdes_timestamp_sequence_autofill_test)

add_executable(warpdes_sequence_restored_on_rollback_test
	sequence_restored_on_rollback_test.cpp
)
target_link_libraries(warpdes_sequence_restored_on_rollback_test PRIVATE warpdes)
add_test(NAME warpdes.sequence_restored_on_rollback COMMAND warpdes_sequence_restored_on_rollback_test)

add_executable(warpdes_anti_tombstone_survives_rollback_test
	anti_tombstone_survives_rollback_test.cpp
)
target_link_libraries(warpdes_anti_tombstone_survives_rollback_test PRIVATE warpdes)
add_test(NAME warpdes.anti_tombstone_survives_rollback COMMAND warpdes_anti_tombstone_survives_rollback_test)

add_executable(warpdes_sent_log_rollback_emits_anti_test
	sent_log_rollback_emits_anti_test.cpp
)
target_link_libraries(warpdes_sent_log_rollback_emits_anti_test PRIVATE warpdes)
add_test(NAME warpdes.sent_log_rollback_emits_anti COMMAND warpdes_sent_log_rollback_emits_anti_test)

add_executable(warpdes_state_history_basic_test
	state_history_basic_test.cpp
)
target_link_libraries(warpdes_state_history_basic_test PRIVATE warpdes)
add_test(NAME warpdes.state_history_basic COMMAND warpdes_state_history_basic_test)

add_executable(warpdes_state_history_drop_after_test
	state_history_drop_after_test.cpp
)
target_link_libraries(warpdes_state_history_drop_after_test PRIVATE warpdes)
add_test(NAME warpdes.state_history_drop_after COMMAND warpdes_state_history_drop_after_test)

add_executable(warpdes_state_history_trim_test
	state_history_trim_test.cpp
)
target_link_libraries(warpdes_state_history_trim_test PRIVATE warpdes)
add_test(NAME warpdes.state_history_trim COMMAND warpdes_state_history_trim_test)

add_executable(warpdes_wire_truncation_decode_test
	wire_truncation_decode_test.cpp
)
target_link_libraries(warpdes_wire_truncation_decode_test PRIVATE warpdes)
add_test(NAME warpdes.wire_truncation_decode COMMAND warpdes_wire_truncation_decode_test)

add_executable(warpdes_modeling_helpers_test
	modeling_helpers_test.cpp
)
target_link_libraries(warpdes_modeling_helpers_test PRIVATE warpdes)
add_test(NAME warpdes.modeling_helpers COMMAND warpdes_modeling_helpers_test)

add_executable(warpdes_targeted_event_requires_routing_test
	targeted_event_requires_routing_test.cpp
)
target_link_libraries(warpdes_targeted_event_requires_routing_test PRIVATE warpdes)
add_test(NAME warpdes.targeted_event_requires_routing COMMAND warpdes_targeted_event_requires_routing_test)

# Verifies that `cmake --install` produces a usable `find_package(warpsim CONFIG)` package.
set(_warpsim_install_prefix "${CMAKE_BINARY_DIR}/_install_test")
add_test(
	NAME warpdes.cmake_find_package_install
	COMMAND ${CMAKE_COMMAND}
		-Dwarpsim_BUILD_DIR=${CMAKE_BINARY_DIR}
		-Dwarpsim_SOURCE_DIR=${CMAKE_SOURCE_DIR}
		-Dwarpsim_INSTALL_PREFIX=${_warpsim_install_prefix}
		-P ${CMAKE_SOURCE_DIR}/tests/cmake_integration/find_package_install_test.cmake
)

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

	 add_executable(warpdes_mpi_determinism_digest_invariance_test
			mpi_determinism_digest_invariance_test.cpp
		)
	 target_link_libraries(warpdes_mpi_determinism_digest_invariance_test PRIVATE warpdes MPI::MPI_CXX)
	 add_test(NAME warpdes.mpi_determinism_digest_invariance_2ranks COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_determinism_digest_invariance_test> ${MPIEXEC_POSTFLAGS})
	 add_test(NAME warpdes.mpi_determinism_digest_invariance_3ranks COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_determinism_digest_invariance_test> ${MPIEXEC_POSTFLAGS})
	 add_test(NAME warpdes.mpi_determinism_digest_invariance_5ranks COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 5 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_determinism_digest_invariance_test> ${MPIEXEC_POSTFLAGS})

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

	add_executable(warpdes_mpi_ack_inflight_exactly_once_test
		mpi_ack_inflight_exactly_once_test.cpp
	)
	target_link_libraries(warpdes_mpi_ack_inflight_exactly_once_test PRIVATE warpdes MPI::MPI_CXX)
	add_test(
		NAME warpdes.mpi_ack_inflight_exactly_once
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_ack_inflight_exactly_once_test> ${MPIEXEC_POSTFLAGS}
	)

	add_executable(warpdes_mpi_colored_gvt_committed_flush_test
		mpi_colored_gvt_committed_flush_test.cpp
	)
	target_link_libraries(warpdes_mpi_colored_gvt_committed_flush_test PRIVATE warpdes MPI::MPI_CXX)
	add_test(
		NAME warpdes.mpi_colored_gvt_committed_flush
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_colored_gvt_committed_flush_test> ${MPIEXEC_POSTFLAGS}
	)

	add_executable(warpdes_mpi_receiver_rollback_anti_cancels_committed_test
		mpi_receiver_rollback_anti_cancels_committed_test.cpp
	)
	target_link_libraries(warpdes_mpi_receiver_rollback_anti_cancels_committed_test PRIVATE warpdes MPI::MPI_CXX)
	add_test(
		NAME warpdes.mpi_receiver_rollback_anti_cancels_committed
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_receiver_rollback_anti_cancels_committed_test> ${MPIEXEC_POSTFLAGS}
	)

	add_executable(warpdes_mpi_optimistic_migration_directory_test
		mpi_optimistic_migration_directory_test.cpp
	)
	target_link_libraries(warpdes_mpi_optimistic_migration_directory_test PRIVATE warpdes MPI::MPI_CXX)
	add_test(
		NAME warpdes.mpi_optimistic_migration_directory
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_optimistic_migration_directory_test> ${MPIEXEC_POSTFLAGS}
	)

	add_executable(warpdes_mpi_entity_migration_policy_test
		mpi_entity_migration_policy_test.cpp
	)
	target_link_libraries(warpdes_mpi_entity_migration_policy_test PRIVATE warpdes MPI::MPI_CXX)
	add_test(NAME warpdes.mpi_entity_migration_policy_2ranks COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_entity_migration_policy_test> ${MPIEXEC_POSTFLAGS})
	add_test(NAME warpdes.mpi_entity_migration_policy_3ranks COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_entity_migration_policy_test> ${MPIEXEC_POSTFLAGS})
	add_test(NAME warpdes.mpi_entity_migration_policy_5ranks COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 5 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_entity_migration_policy_test> ${MPIEXEC_POSTFLAGS})

	add_executable(warpdes_mpi_optimistic_migration_straggler_rollback_test
		mpi_optimistic_migration_straggler_rollback_test.cpp
	)
	target_link_libraries(warpdes_mpi_optimistic_migration_straggler_rollback_test PRIVATE warpdes MPI::MPI_CXX)
	add_test(NAME warpdes.mpi_optimistic_migration_straggler_rollback_2ranks COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_optimistic_migration_straggler_rollback_test> ${MPIEXEC_POSTFLAGS})
	add_test(NAME warpdes.mpi_optimistic_migration_straggler_rollback_3ranks COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_optimistic_migration_straggler_rollback_test> ${MPIEXEC_POSTFLAGS})
	add_test(NAME warpdes.mpi_optimistic_migration_straggler_rollback_5ranks COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 5 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_mpi_optimistic_migration_straggler_rollback_test> ${MPIEXEC_POSTFLAGS})

endif()
