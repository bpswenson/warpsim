add_executable(warpdes_demo
	demo.cpp
)
target_link_libraries(warpdes_demo PRIVATE warpdes)

if(WARPSIM_ENABLE_EXAMPLE_SMOKE_TESTS)
	add_test(NAME warpdes.example_demo COMMAND $<TARGET_FILE:warpdes_demo>)
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

if(WARPSIM_ENABLE_MPI_TESTS)
	add_executable(warpdes_phold_mpi
		phold_mpi.cpp
	)
	target_link_libraries(warpdes_phold_mpi PRIVATE warpdes MPI::MPI_CXX)
	add_test(
		NAME warpdes.mpi_phold_smoke
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS} $<TARGET_FILE:warpdes_phold_mpi> ${MPIEXEC_POSTFLAGS} --lps 64 --init 1 --end 200 --remote 0.9 --lookahead 5 --seed 1
	)
endif()
