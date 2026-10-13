# Unit tests: every tests/unit/*.cpp is part of one executable. Components that need extra
# libraries add them with target_link_libraries(pspc_unittests ...) from their own CMakeLists.
file(GLOB PSPC_UNIT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)
if(NOT PSPC_BUILD_RUNTIME)
	list(FILTER PSPC_UNIT_SOURCES EXCLUDE REGEX "/test_(audio|gpu|http|pspc_network)")
endif()
add_executable(pspc_unittests ${PSPC_UNIT_SOURCES})
target_link_libraries(pspc_unittests PRIVATE pspc_sem)
if(NOT PSPC_BUILD_RUNTIME)
	# Socket and ad hoc loopback tests do not need SDL, media codecs, or the HTTP backend.
	target_sources(pspc_unittests PRIVATE
		${CMAKE_SOURCE_DIR}/src/net/socket.cpp
		${CMAKE_SOURCE_DIR}/src/net/host_net.cpp
		${CMAKE_SOURCE_DIR}/src/net/dns.cpp
		${CMAKE_SOURCE_DIR}/src/net/adhoc_server.cpp
	)
	if(WIN32)
		target_link_libraries(pspc_unittests PRIVATE ws2_32 iphlpapi)
	endif()
endif()
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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	if(TARGET pspc_runtime)
		add_executable(pspc_net_recvmsg_tests runtime/test_net_recvmsg.cpp)
		target_link_libraries(pspc_net_recvmsg_tests PRIVATE pspc_runtime)
		foreach(mode IN ITEMS exact_fit oversized)
			add_test(NAME net_recvmsg_${mode} COMMAND pspc_net_recvmsg_tests ${mode})
			set_tests_properties(net_recvmsg_${mode} PROPERTIES TIMEOUT 5)
		endforeach()
	endif()
endif()
