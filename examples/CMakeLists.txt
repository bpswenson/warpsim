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

add_executable(warpdes_airplanes_missile_aoi_demo airplanes_missile_aoi_demo.cpp)
target_link_libraries(warpdes_airplanes_missile_aoi_demo PRIVATE warpdes)
if(WARPSIM_ENABLE_MPI_TESTS)
	target_link_libraries(warpdes_airplanes_missile_aoi_demo PRIVATE MPI::MPI_CXX)
	target_compile_definitions(warpdes_airplanes_missile_aoi_demo PRIVATE WARPSIM_HAS_MPI=1)
endif()
add_executable(warpdes_airport_fujimoto
	airport_fujimoto.cpp
)
target_link_libraries(warpdes_airport_fujimoto PRIVATE warpdes)

add_executable(warpdes_regional_airport_migration_demo
	regional_airport_migration_demo.cpp
)
target_link_libraries(warpdes_regional_airport_migration_demo PRIVATE warpdes)

if(WARPSIM_ENABLE_MPI_TESTS)
	target_link_libraries(warpdes_regional_airport_migration_demo PRIVATE MPI::MPI_CXX)
	target_compile_definitions(warpdes_regional_airport_migration_demo PRIVATE WARPSIM_HAS_MPI=1)
endif()

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

	# Airplanes + missiles + AOI demo (MPI smoke at multiple sizes).
	add_test(
		NAME warpdes.mpi_airplanes_missile_aoi_3ranks
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_airplanes_missile_aoi_demo> ${MPIEXEC_POSTFLAGS} --regions 4 --planes-per-region 8 --end 60 --seed 1 --query-every 1000
	)

	add_test(
		NAME warpdes.mpi_airplanes_missile_aoi_5ranks
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 5 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_airplanes_missile_aoi_demo> ${MPIEXEC_POSTFLAGS} --regions 4 --planes-per-region 8 --end 60 --seed 1 --query-every 1000
	)

	# Regional airport + migration example determinism across multiple rank counts.
	add_test(
		NAME warpdes.mpi_regional_airport_migration_2ranks
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_regional_airport_migration_demo> ${MPIEXEC_POSTFLAGS} --regions 14 --airports-per-region 2 --planes 112 --end 600 --service-max 5 --region-width 1000 --speed 25 --seed 1 --emit-plane-mod 8 --verify --expected-hash 4834955128110636547
	)
	add_test(
		NAME warpdes.mpi_regional_airport_migration_4ranks
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 4 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_regional_airport_migration_demo> ${MPIEXEC_POSTFLAGS} --regions 14 --airports-per-region 2 --planes 112 --end 600 --service-max 5 --region-width 1000 --speed 25 --seed 1 --emit-plane-mod 8 --verify --expected-hash 4834955128110636547
	)
	add_test(
		NAME warpdes.mpi_regional_airport_migration_6ranks
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 6 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_regional_airport_migration_demo> ${MPIEXEC_POSTFLAGS} --regions 14 --airports-per-region 2 --planes 112 --end 600 --service-max 5 --region-width 1000 --speed 25 --seed 1 --emit-plane-mod 8 --verify --expected-hash 4834955128110636547
	)
	add_test(
		NAME warpdes.mpi_regional_airport_migration_8ranks
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 8 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_regional_airport_migration_demo> ${MPIEXEC_POSTFLAGS} --regions 14 --airports-per-region 2 --planes 112 --end 600 --service-max 5 --region-width 1000 --speed 25 --seed 1 --emit-plane-mod 8 --verify --expected-hash 4834955128110636547
	)
	add_test(
		NAME warpdes.mpi_regional_airport_migration_10ranks
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 10 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_regional_airport_migration_demo> ${MPIEXEC_POSTFLAGS} --regions 14 --airports-per-region 2 --planes 112 --end 600 --service-max 5 --region-width 1000 --speed 25 --seed 1 --emit-plane-mod 8 --verify --expected-hash 4834955128110636547
	)

	set_tests_properties(warpdes.mpi_airplanes_missile_aoi_3ranks PROPERTIES TIMEOUT 45)
	set_tests_properties(warpdes.mpi_airplanes_missile_aoi_5ranks PROPERTIES TIMEOUT 45)
	set_tests_properties(warpdes.mpi_regional_airport_migration_2ranks PROPERTIES TIMEOUT 90)
	set_tests_properties(warpdes.mpi_regional_airport_migration_4ranks PROPERTIES TIMEOUT 90)
	set_tests_properties(warpdes.mpi_regional_airport_migration_6ranks PROPERTIES TIMEOUT 90)
	set_tests_properties(warpdes.mpi_regional_airport_migration_8ranks PROPERTIES TIMEOUT 90)
	set_tests_properties(warpdes.mpi_regional_airport_migration_10ranks PROPERTIES TIMEOUT 90)
endif()
