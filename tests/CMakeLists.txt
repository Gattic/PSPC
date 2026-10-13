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

if(TARGET pspc_runtime)
	add_executable(pspc_host_audio_tests host/test_audio.cpp)
	target_link_libraries(pspc_host_audio_tests PRIVATE pspc_runtime)
	add_executable(pspc_scheduler_tests runtime/test_scheduler.cpp)
	target_link_libraries(pspc_scheduler_tests PRIVATE pspc_runtime)
	add_executable(pspc_interrupt_context_tests runtime/test_interrupt_context.cpp)
	target_link_libraries(pspc_interrupt_context_tests PRIVATE pspc_runtime)
	foreach(clock IN ITEMS real fast)
		foreach(mode IN ITEMS preempted blocked woken)
			add_test(NAME interrupt_context_${clock}_${mode} COMMAND pspc_interrupt_context_tests ${mode} ${clock})
			set_tests_properties(interrupt_context_${clock}_${mode} PROPERTIES TIMEOUT 5)
		endforeach()
	endforeach()
	add_test(NAME interrupt_context_fast_busy COMMAND pspc_interrupt_context_tests busy fast)
	set_tests_properties(interrupt_context_fast_busy PROPERTIES TIMEOUT 5)
	if(WIN32)
		foreach(test_target IN ITEMS pspc_host_audio_tests pspc_scheduler_tests pspc_interrupt_context_tests)
			add_custom_command(TARGET ${test_target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${test_target}>
					$<TARGET_FILE_DIR:${test_target}>
				COMMAND_EXPAND_LISTS)
		endforeach()
	endif()
	add_test(NAME scheduler COMMAND pspc_scheduler_tests)
	set_tests_properties(scheduler PROPERTIES TIMEOUT 5)
	add_test(NAME host_audio COMMAND pspc_host_audio_tests)
	set_tests_properties(host_audio PROPERTIES TIMEOUT 5
		ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
endif()

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
