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
foreach(lib pspc_fs pspc_gpu pspc_loader pspc_audio pspc_net pspc_game_library)
	if(TARGET ${lib})
		target_link_libraries(pspc_unittests PRIVATE ${lib})
	endif()
endforeach()
# The D3D11 renderer backend against the software renderer (unit/test_gpu_d3d11.cpp).
if(TARGET pspc_gpu_hw)
	target_link_libraries(pspc_unittests PRIVATE pspc_gpu_hw)
	target_compile_definitions(pspc_unittests PRIVATE PSPC_TEST_GPU_HW=1)
endif()
if(TARGET pspc_gpu_gl)
	find_package(SDL2 REQUIRED CONFIG)
	target_link_libraries(pspc_unittests PRIVATE pspc_gpu_gl SDL2::SDL2)
	target_compile_definitions(pspc_unittests PRIVATE PSPC_TEST_GPU_GL=1)
endif()
if(TARGET pspc-launcher AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(PSPC_GTK REQUIRED IMPORTED_TARGET gtk+-3.0>=3.22)
	target_sources(pspc_unittests PRIVATE ${CMAKE_SOURCE_DIR}/src/frontend/servers.cpp)
	target_link_libraries(pspc_unittests PRIVATE PkgConfig::PSPC_GTK)
	target_compile_definitions(pspc_unittests PRIVATE PSPC_TEST_LAUNCHER=1)
endif()
add_test(NAME unittests COMMAND pspc_unittests)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME firmware_manager COMMAND ${Python3_EXECUTABLE} -I
	${CMAKE_CURRENT_SOURCE_DIR}/tools/test_firmware_manager.py)
set_tests_properties(firmware_manager PROPERTIES TIMEOUT 30)
add_test(NAME firmware_discovery COMMAND ${Python3_EXECUTABLE} -I
	${CMAKE_CURRENT_SOURCE_DIR}/tools/test_firmware_discovery.py)
set_tests_properties(firmware_discovery PROPERTIES TIMEOUT 30)

if(TARGET pspc_runtime)
	add_executable(pspc_host_audio_tests host/test_audio.cpp)
	target_link_libraries(pspc_host_audio_tests PRIVATE pspc_runtime)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		add_executable(pspc_host_video_reset_tests host/test_video_reset.cpp)
		target_link_libraries(pspc_host_video_reset_tests PRIVATE pspc_runtime)
		foreach(function IN ITEMS CreateRenderer CreateTexture DestroyTexture UpdateTexture RenderCopy RenderPresent PollEvent)
			target_link_options(pspc_host_video_reset_tests PRIVATE "-Wl,--wrap=SDL_${function}")
		endforeach()
		foreach(mode IN ITEMS resumed paused scaled)
			add_test(NAME host_video_reset_${mode} COMMAND pspc_host_video_reset_tests ${mode})
			set_tests_properties(host_video_reset_${mode} PROPERTIES TIMEOUT 10
				ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
		endforeach()
	endif()
	add_executable(pspc_scheduler_tests runtime/test_scheduler.cpp)
	target_link_libraries(pspc_scheduler_tests PRIVATE pspc_runtime)
	add_executable(pspc_interp_tests runtime/test_interp.cpp)
	target_link_libraries(pspc_interp_tests PRIVATE pspc_runtime)
	add_test(NAME interpreter COMMAND pspc_interp_tests)
	set_tests_properties(interpreter PROPERTIES TIMEOUT 10)
	add_executable(pspc_display_pacing_tests runtime/test_display_pacing.cpp)
	target_link_libraries(pspc_display_pacing_tests PRIVATE pspc_runtime)
	foreach(clock IN ITEMS real fast)
		add_test(NAME display_pacing_${clock} COMMAND pspc_display_pacing_tests ${clock})
		set_tests_properties(display_pacing_${clock} PROPERTIES TIMEOUT 10
			ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
	endforeach()
	add_executable(pspc_lifetime_tests runtime/test_lifetime.cpp)
	target_link_libraries(pspc_lifetime_tests PRIVATE pspc_runtime)
	find_package(Python3 REQUIRED COMPONENTS Interpreter)
	find_program(PSPC_VALGRIND valgrind)
	foreach(mode IN ITEMS deferred shutdown)
		set(lifetime_command ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/runtime/test_lifetime.py
			--probe $<TARGET_FILE:pspc_lifetime_tests> --case ${mode})
		add_test(NAME lifetime_${mode} COMMAND ${lifetime_command})
		set_tests_properties(lifetime_${mode} PROPERTIES TIMEOUT 40)
		if(PSPC_VALGRIND)
			add_test(NAME lifetime_${mode}_memcheck COMMAND ${lifetime_command} --valgrind ${PSPC_VALGRIND})
			set_tests_properties(lifetime_${mode}_memcheck PROPERTIES TIMEOUT 40)
		endif()
	endforeach()
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
		foreach(test_target IN ITEMS pspc_host_audio_tests pspc_scheduler_tests pspc_interp_tests pspc_display_pacing_tests pspc_interrupt_context_tests pspc_lifetime_tests)
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
	find_package(Python3 REQUIRED COMPONENTS Interpreter)
	add_test(NAME ghidra_headless COMMAND ${Python3_EXECUTABLE} -I
		${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ghidra_analyze.py)
	set_tests_properties(ghidra_headless PROPERTIES TIMEOUT 30)
	add_executable(pspc_aot_probe aot/module_probe.cpp)
	target_link_libraries(pspc_aot_probe PRIVATE pspc_common)
	target_include_directories(pspc_aot_probe PRIVATE ${CMAKE_BINARY_DIR}/generated)
	add_dependencies(pspc_aot_probe pspc-recomp pspc-iso pspc-decrypt pspc_sem)
	add_test(NAME aot_pipeline COMMAND ${Python3_EXECUTABLE} -I
		${CMAKE_CURRENT_SOURCE_DIR}/aot/test_pipeline.py
		--build-dir ${CMAKE_BINARY_DIR} --probe $<TARGET_FILE:pspc_aot_probe>
		--compiler ${CMAKE_CXX_COMPILER})
	set_tests_properties(aot_pipeline PROPERTIES TIMEOUT 180)
	add_test(NAME aot_install COMMAND ${Python3_EXECUTABLE} -I
		${CMAKE_CURRENT_SOURCE_DIR}/aot/test_pipeline.py --installed
		--build-dir ${CMAKE_BINARY_DIR} --probe $<TARGET_FILE:pspc_aot_probe>
		--compiler ${CMAKE_CXX_COMPILER})
	set_tests_properties(aot_install PROPERTIES TIMEOUT 180)
	if(TARGET pspc_runtime)
		add_executable(pspc_net_recvmsg_tests runtime/test_net_recvmsg.cpp)
		target_link_libraries(pspc_net_recvmsg_tests PRIVATE pspc_runtime)
		foreach(mode IN ITEMS exact_fit oversized)
			add_test(NAME net_recvmsg_${mode} COMMAND pspc_net_recvmsg_tests ${mode})
			set_tests_properties(net_recvmsg_${mode} PROPERTIES TIMEOUT 5)
		endforeach()
		add_executable(pspc_aot_runtime_probe aot/runtime_probe.cpp)
		target_link_libraries(pspc_aot_runtime_probe PRIVATE pspc_runtime)
		add_dependencies(pspc_aot_runtime_probe pspc-recomp pspc_sem)
		add_test(NAME aot_runtime COMMAND ${Python3_EXECUTABLE} -I
			${CMAKE_CURRENT_SOURCE_DIR}/aot/test_pipeline.py --runtime
			--build-dir ${CMAKE_BINARY_DIR} --probe $<TARGET_FILE:pspc_aot_runtime_probe>
			--compiler ${CMAKE_CXX_COMPILER})
		set_tests_properties(aot_runtime PROPERTIES TIMEOUT 180)
	endif()
	if(TARGET pspc-launcher)
		add_test(NAME launcher_cli COMMAND ${Python3_EXECUTABLE} -I
			${CMAKE_CURRENT_SOURCE_DIR}/launcher/test_cli.py $<TARGET_FILE:pspc-launcher>)
		set_tests_properties(launcher_cli PROPERTIES TIMEOUT 30)
		add_test(NAME launcher_paths COMMAND ${Python3_EXECUTABLE} -I
			${CMAKE_CURRENT_SOURCE_DIR}/launcher/test_paths.py --build-dir ${CMAKE_BINARY_DIR}
			--compiler ${CMAKE_CXX_COMPILER})
		set_tests_properties(launcher_paths PROPERTIES TIMEOUT 120)
		add_test(NAME runtime_package COMMAND ${Python3_EXECUTABLE} -I
			${CMAKE_CURRENT_SOURCE_DIR}/launcher/test_package.py --build-dir ${CMAKE_BINARY_DIR}
			--compiler ${CMAKE_CXX_COMPILER})
		set_tests_properties(runtime_package PROPERTIES TIMEOUT 240)
		add_test(NAME desktop_integration COMMAND ${Python3_EXECUTABLE} -I
			${CMAKE_CURRENT_SOURCE_DIR}/launcher/test_desktop.py --build-dir ${CMAKE_BINARY_DIR})
		set_tests_properties(desktop_integration PROPERTIES TIMEOUT 90)
	endif()
endif()
