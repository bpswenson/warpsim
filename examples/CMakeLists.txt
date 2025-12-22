add_executable(warpdes_demo
	demo.cpp
)
target_link_libraries(warpdes_demo PRIVATE warpdes)

if(WARPSIM_ENABLE_EXAMPLE_SMOKE_TESTS)
	add_test(NAME warpdes.example_demo COMMAND $<TARGET_FILE:warpdes_demo>)
endif()

add_executable(warpdes_entity_migration_demo
	entity_migration_demo.cpp
)
target_link_libraries(warpdes_entity_migration_demo PRIVATE warpdes)

if(WARPSIM_ENABLE_EXAMPLE_SMOKE_TESTS)
	add_test(NAME warpdes.example_entity_migration_demo COMMAND $<TARGET_FILE:warpdes_entity_migration_demo>)
endif()

add_executable(warpdes_market_demo
	market_demo.cpp
)
target_link_libraries(warpdes_market_demo PRIVATE warpdes)

if(WARPSIM_ENABLE_EXAMPLE_SMOKE_TESTS)
	add_test(NAME warpdes.example_market_demo COMMAND $<TARGET_FILE:warpdes_market_demo>)
endif()

add_executable(warpdes_radar_explosion_demo
	radar_explosion_demo.cpp
)
target_link_libraries(warpdes_radar_explosion_demo PRIVATE warpdes)

if(WARPSIM_ENABLE_EXAMPLE_SMOKE_TESTS)
	add_test(NAME warpdes.example_radar_explosion_demo COMMAND $<TARGET_FILE:warpdes_radar_explosion_demo>)
endif()

add_executable(warpdes_airport_fujimoto
	airport_fujimoto.cpp
)
target_link_libraries(warpdes_airport_fujimoto PRIVATE warpdes)

if(WARPSIM_ENABLE_MPI_TESTS)
	target_link_libraries(warpdes_airport_fujimoto PRIVATE MPI::MPI_CXX)
	target_compile_definitions(warpdes_airport_fujimoto PRIVATE WARPSIM_HAS_MPI=1)
endif()

if(WARPSIM_ENABLE_EXAMPLE_SMOKE_TESTS)
	# Small deterministic run that verifies committed output + rank-count independence.
	# Expected hash is checked inside the binary.
	add_test(
		NAME warpdes.example_airport_fujimoto
		COMMAND $<TARGET_FILE:warpdes_airport_fujimoto> --airports 8 --planes 64 --end 400 --service-max 5 --flight-max 9 --seed 1 --emit-plane-mod 4 --verify --expected-hash 4209842363547979520
	)
endif()

if(WARPSIM_ENABLE_MPI_TESTS)
	add_executable(warpdes_phold_mpi
		phold_mpi.cpp
	)
	target_link_libraries(warpdes_phold_mpi PRIVATE warpdes MPI::MPI_CXX)
	target_compile_definitions(warpdes_phold_mpi PRIVATE WARPSIM_HAS_MPI=1)
	add_test(
		NAME warpdes.mpi_phold_smoke
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_phold_mpi> ${MPIEXEC_POSTFLAGS} --lps 64 --init 1 --end 200 --remote 0.9 --lookahead 5 --seed 1
	)

	# PHOLD determinism across ranks (committed-output hash verified inside the binary).
	add_test(
		NAME warpdes.mpi_phold_3ranks
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_phold_mpi> ${MPIEXEC_POSTFLAGS} --lps 1024 --init 1 --end 10000 --remote 0.9 --lookahead 5 --seed 1 --emit-mod 16 --verify --expected-hash 339321921475465244
	)

	add_test(
		NAME warpdes.mpi_phold_5ranks
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 5 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_phold_mpi> ${MPIEXEC_POSTFLAGS} --lps 1024 --init 1 --end 10000 --remote 0.9 --lookahead 5 --seed 1 --emit-mod 16 --verify --expected-hash 339321921475465244
	)

	add_test(
		NAME warpdes.mpi_phold_20ranks
		COMMAND ${MPIEXEC_EXECUTABLE} --oversubscribe ${MPIEXEC_NUMPROC_FLAG} 20 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_phold_mpi> ${MPIEXEC_POSTFLAGS} --lps 1024 --init 1 --end 10000 --remote 0.9 --lookahead 5 --seed 1 --emit-mod 16 --verify --expected-hash 339321921475465244
	)

	# Fujimoto airport example determinism across ranks.
	add_test(
		NAME warpdes.mpi_airport_fujimoto_3ranks
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_airport_fujimoto> ${MPIEXEC_POSTFLAGS} --airports 8 --planes 64 --end 400 --service-max 5 --flight-max 9 --seed 1 --emit-plane-mod 4 --verify --expected-hash 4209842363547979520
	)

	add_test(
		NAME warpdes.mpi_airport_fujimoto_5ranks
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 5 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_airport_fujimoto> ${MPIEXEC_POSTFLAGS} --airports 8 --planes 64 --end 400 --service-max 5 --flight-max 9 --seed 1 --emit-plane-mod 4 --verify --expected-hash 4209842363547979520
	)

	add_test(
		NAME warpdes.mpi_airport_fujimoto_20ranks
		COMMAND ${MPIEXEC_EXECUTABLE} --oversubscribe ${MPIEXEC_NUMPROC_FLAG} 20 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_airport_fujimoto> ${MPIEXEC_POSTFLAGS} --airports 8 --planes 64 --end 400 --service-max 5 --flight-max 9 --seed 1 --emit-plane-mod 4 --verify --expected-hash 4209842363547979520
	)
endif()
