# Unit tests: every tests/unit/*.cpp is part of one executable. Components that need extra
# libraries add them with target_link_libraries(pspc_unittests ...) from their own CMakeLists.
file(GLOB PSPC_UNIT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)
add_executable(pspc_unittests ${PSPC_UNIT_SOURCES})
target_link_libraries(pspc_unittests PRIVATE pspc_sem)
foreach(lib pspc_fs pspc_gpu pspc_loader pspc_audio pspc_net)
	if(TARGET ${lib})
		target_link_libraries(pspc_unittests PRIVATE ${lib})
	endif()
endforeach()
# The D3D11 renderer backend against the software renderer (unit/test_gpu_d3d11.cpp).
if(TARGET pspc_gpu_hw)
	target_link_libraries(pspc_unittests PRIVATE pspc_gpu_hw)
	target_compile_definitions(pspc_unittests PRIVATE PSPC_TEST_GPU_HW=1)
endif()
add_test(NAME unittests COMMAND pspc_unittests)
