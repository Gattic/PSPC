cmake_minimum_required(VERSION 3.20)
project(PSPC C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(PSPC_BUILD_TESTS "Build unit tests" ON)

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

if(PSPC_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
