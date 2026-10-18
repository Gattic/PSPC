cmake_minimum_required(VERSION 3.21)
project(PSPCLocalSDL LANGUAGES C)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(FATAL_ERROR "The local SDL fullscreen fix is Linux-only")
endif()
if(POLICY CMP0135)
	cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)

# This separate build cannot change PSPC's system SDL discovery or package contents.
# Never install these libraries: the launch helper selects this directory per process.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_WAYLAND ON CACHE BOOL "" FORCE)
set(SDL_WAYLAND_LIBDECOR ON CACHE BOOL "" FORCE)
set(SDL_X11 ON CACHE BOOL "" FORCE)
set(SDL_OPENGL ON CACHE BOOL "" FORCE)

FetchContent_Declare(sdl3
	URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
	URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
	PATCH_COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=<SOURCE_DIR>
		-P "${CMAKE_CURRENT_SOURCE_DIR}/PatchSDL.cmake")
FetchContent_MakeAvailable(sdl3)
# Also check on incremental configurations, when FetchContent skips its patch step.
set(SOURCE_DIR "${sdl3_SOURCE_DIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/PatchSDL.cmake")

# SDL's ON options can silently become unavailable. Require the affected backend and
# preserve desktop rendering/audio rather than producing the earlier dummy-audio probe.
file(READ "${sdl3_BINARY_DIR}/CMakeFiles/SDL_build_config.h.intermediate" config)
foreach(feature SDL_VIDEO_DRIVER_WAYLAND HAVE_LIBDECOR_H SDL_VIDEO_DRIVER_X11 SDL_VIDEO_OPENGL)
	if(NOT config MATCHES "#define ${feature} 1")
		message(FATAL_ERROR "Local SDL requires ${feature}; install its development dependencies (docs/local-sdl.md)")
	endif()
endforeach()
if(NOT config MATCHES "#define SDL_AUDIO_DRIVER_(ALSA|PULSEAUDIO|PIPEWIRE) 1")
	message(FATAL_ERROR "Local SDL requires ALSA, PulseAudio or PipeWire development dependencies (docs/local-sdl.md)")
endif()

set(SDL2COMPAT_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2COMPAT_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2COMPAT_STATIC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sdl2_compat
	URL https://github.com/libsdl-org/sdl2-compat/releases/download/release-2.32.74/sdl2-compat-2.32.74.tar.gz
	URL_HASH SHA256=ec68abde77e2e459c8abc4f9587b8976b5d2a657b2bfda2fb3a079b0f8924588)
FetchContent_MakeAvailable(sdl2_compat)

# Deterministic regression for the patched backend; no native display is required.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(geometry_test_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/host")
set(wayland_source "${sdl3_SOURCE_DIR}/src/video/wayland/SDL_waylandwindow.c")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/wayland_geometry.inc"
	COMMAND ${Python3_EXECUTABLE} -I "${geometry_test_dir}/extract_wayland_geometry.py"
		"${wayland_source}" "${CMAKE_CURRENT_BINARY_DIR}/wayland_geometry.inc"
	DEPENDS "${wayland_source}" "${geometry_test_dir}/extract_wayland_geometry.py" VERBATIM)
add_executable(pspc_sdl_wayland_scale_tests "${geometry_test_dir}/test_wayland_scale.c"
	"${CMAKE_CURRENT_BINARY_DIR}/wayland_geometry.inc")
# Private SDL headers include build-generated Wayland protocol headers.
add_dependencies(pspc_sdl_wayland_scale_tests SDL3-shared)
target_include_directories(pspc_sdl_wayland_scale_tests PRIVATE
	"${CMAKE_CURRENT_BINARY_DIR}" "$<TARGET_PROPERTY:SDL3-shared,INCLUDE_DIRECTORIES>")
target_compile_definitions(pspc_sdl_wayland_scale_tests PRIVATE USING_GENERATED_CONFIG_H)
target_link_libraries(pspc_sdl_wayland_scale_tests PRIVATE m)
enable_testing()
add_test(NAME wayland_scale_restore COMMAND pspc_sdl_wayland_scale_tests)
