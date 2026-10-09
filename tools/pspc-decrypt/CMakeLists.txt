# SPDX-License-Identifier: GPL-3.0-or-later
#
# pspc-decrypt is GPL-3.0-or-later because it links libkirk. It is a separate executable on
# purpose: nothing under src/ may link these targets or include these sources; the runtime only
# runs the executable as a subprocess. Its tests are a separate executable for the same reason
# (they are not part of pspc_unittests).
#
# Builds as part of PSPC (via tools/CMakeLists.txt) or on its own:
#   cmake -S tools/pspc-decrypt -B build/pspc-decrypt -G Ninja

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	project(pspc_decrypt C CXX)
	set(CMAKE_CXX_STANDARD 20)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(CMAKE_C_STANDARD 11)
	if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
		set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
	endif()
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
	set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
	option(PSPC_BUILD_TESTS "Build unit tests" ON)
	if(MSVC)
		add_compile_options(/W3 /permissive- /Zc:preprocessor /utf-8 /wd4100 /wd4244 /wd4267 /wd4146 /wd4996)
		add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
endif()

set(PSPC_DECRYPT_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# libkirk, unmodified upstream C. Its warnings are upstream's business.
add_library(pspc_decrypt_kirk STATIC
	libkirk/AES.c
	libkirk/SHA1.c
	libkirk/amctrl.c
	libkirk/bn.c
	libkirk/ec.c
	libkirk/kirk_engine.c
)
if(MSVC)
	target_compile_options(pspc_decrypt_kirk PRIVATE /wd4018 /wd4101 /wd4146 /wd4244 /wd4267 /wd4305 /wd4996)
else()
	target_compile_options(pspc_decrypt_kirk PRIVATE -w)
endif()

add_library(pspc_decrypt_core STATIC
	src/common.cpp
	src/des.cpp
	src/formats.cpp
	src/inflate.cpp
	src/ipl.cpp
	src/json.cpp
	src/kl4e.cpp
	src/lzrc.cpp
	src/module.cpp
	src/prx_decrypter.cpp
	src/psar.cpp
)
target_include_directories(pspc_decrypt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pspc_decrypt_core PUBLIC pspc_decrypt_kirk)

add_executable(pspc-decrypt src/main.cpp)
target_link_libraries(pspc-decrypt PRIVATE pspc_decrypt_core)

if(PSPC_BUILD_TESTS)
	# The root enables testing only after src/ and tools/ are added, so do it here as well.
	enable_testing()
	file(GLOB PSPC_DECRYPT_TEST_SOURCES CONFIGURE_DEPENDS
		${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c)
	add_executable(pspc_decrypt_tests ${PSPC_DECRYPT_TEST_SOURCES})
	target_link_libraries(pspc_decrypt_tests PRIVATE pspc_decrypt_core)
	if(MSVC)
		set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/tests/ref_kl4e_artart78.c PROPERTIES COMPILE_OPTIONS "/wd4018;/wd4244")
	endif()
	add_dependencies(pspc_decrypt_tests pspc-decrypt)
	add_test(NAME pspc_decrypt_tests
		COMMAND pspc_decrypt_tests
			--tool $<TARGET_FILE:pspc-decrypt>
			--repo ${PSPC_DECRYPT_REPO_ROOT}
			--tmp ${CMAKE_CURRENT_BINARY_DIR}/test-tmp)
endif()
