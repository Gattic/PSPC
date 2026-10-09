# Unit tests: every tests/unit/*.cpp is part of one executable. Components that need extra
# libraries add them with target_link_libraries(pspc_unittests ...) from their own CMakeLists.
file(GLOB PSPC_UNIT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)
add_executable(pspc_unittests ${PSPC_UNIT_SOURCES})
target_link_libraries(pspc_unittests PRIVATE pspc_sem)
foreach(lib pspc_fs pspc_gpu pspc_loader pspc_audio)
	if(TARGET ${lib})
		target_link_libraries(pspc_unittests PRIVATE ${lib})
	endif()
endforeach()
add_test(NAME unittests COMMAND pspc_unittests)
