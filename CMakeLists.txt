cmake_minimum_required(VERSION 3.21)
# The release version: the installer (tools/make_installer.py), the launcher and pspc --version show it.
project(PSPC VERSION 0.1.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(FATAL_ERROR "Supported hosts: Windows and Linux")
endif()
find_package(Threads REQUIRED)
if(NOT MSVC)
	# Guest memory is accessed through several scalar types; keep the semantics bit-exact.
	add_compile_options(-fno-strict-aliasing -ffp-contract=off)
	add_compile_definitions(_FILE_OFFSET_BITS=64)
endif()

# 64-bit only: disc images and files past 4 GB become buffer lengths and offsets in size_t, and
# the recompiler emits x64 code.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "PSPC builds for 64-bit targets only (use the x64 compiler, e.g. vcvars64)")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(PSPC_BUILD_TESTS "Build unit tests" ON)
option(PSPC_BUILD_RUNTIME "Build the runtime and graphical launcher" ON)

if(MSVC)
	add_compile_options(/W3 /permissive- /Zc:preprocessor /utf-8 /wd4100 /wd4244 /wd4267 /wd4146 /wd4996)
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
	# Static CRT keeps pspc.exe and AOT DLLs free of redistributable dependencies.
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_subdirectory(src)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt)
	add_subdirectory(tools)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	# A relocatable, source-tree-independent AOT SDK, not a runtime/system installer.
	# Python and a compatible C++20 toolchain remain host dependencies.
	install(TARGETS pspc-recomp pspc-iso pspc-decrypt RUNTIME DESTINATION . COMPONENT AOT)
	install(TARGETS pspc_sem pspc_common ARCHIVE DESTINATION aot/lib COMPONENT AOT)
	install(DIRECTORY src/allegrex DESTINATION aot/src COMPONENT AOT)
	install(FILES src/common/common.h DESTINATION aot/src/common COMPONENT AOT)
	install(FILES src/recomp/codegen.cpp DESTINATION aot/src/recomp COMPONENT AOT)
	install(FILES ${CMAKE_BINARY_DIR}/generated/pspc_sem_version.h DESTINATION aot/generated COMPONENT AOT)
	install(FILES tools/pspc_build.py DESTINATION tools COMPONENT AOT)
	install(FILES LICENSE THIRD_PARTY.md DESTINATION . COMPONENT AOT)
	install(FILES tools/pspc-decrypt/COPYING tools/pspc-decrypt/libkirk/NOTICE
		DESTINATION licenses/pspc-decrypt COMPONENT AOT)
	install(FILES docs/linux-port.md docs/RECOMPILER.md DESTINATION docs COMPONENT AOT)
	if(PSPC_BUILD_RUNTIME)
		include(cmake/LinuxPackage.cmake)
		# Explicit opt-in; not part of ALL, install or CPack. No system libraries change.
		find_package(Python3 REQUIRED COMPONENTS Interpreter)
		add_custom_target(pspc-local-sdl
			COMMAND ${CMAKE_COMMAND} -S "${CMAKE_SOURCE_DIR}/cmake/local-sdl"
				-B "${CMAKE_BINARY_DIR}/local-sdl" -G "${CMAKE_GENERATOR}"
				-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
			COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}/local-sdl"
				--parallel 2 --target SDL3-shared SDL2 pspc_sdl_wayland_scale_tests
			COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${CMAKE_BINARY_DIR}/local-sdl"
				--output-on-failure
			COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tools/run_local_sdl.py"
				--build-dir "${CMAKE_BINARY_DIR}" --check
			USES_TERMINAL VERBATIM)
	endif()
endif()

if(PSPC_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
