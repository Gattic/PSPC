if(WIN32)
	set(SDL2_ROOT ${CMAKE_SOURCE_DIR}/third_party/SDL2-2.32.10)
	if(NOT EXISTS ${SDL2_ROOT}/lib/x64/SDL2.lib)
		message(FATAL_ERROR "SDL2 not found in ${SDL2_ROOT}. Run: powershell -ExecutionPolicy Bypass -File tools\\fetch_deps.ps1")
	endif()
	add_library(SDL2::SDL2 SHARED IMPORTED)
	set_target_properties(SDL2::SDL2 PROPERTIES
		IMPORTED_LOCATION ${SDL2_ROOT}/lib/x64/SDL2.dll
		IMPORTED_IMPLIB ${SDL2_ROOT}/lib/x64/SDL2.lib
		INTERFACE_INCLUDE_DIRECTORIES ${SDL2_ROOT}/include)
else()
	find_package(SDL2 REQUIRED CONFIG)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(PSPC_FONTS REQUIRED IMPORTED_TARGET freetype2 fontconfig)
endif()

file(GLOB PSPC_HLE_MODULES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hle/modules/*.cpp)
file(GLOB PSPC_KERNEL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernel/*.cpp)

add_library(pspc_runtime STATIC
	boot.cpp
	runtime.cpp
	vsh.cpp
	core/backtrace.cpp
	core/cpu.cpp
	core/dcache.cpp
	core/dispatch.cpp
	core/interp.cpp
	core/memory.cpp
	hle/hle.cpp
	host/sdl_host.cpp
	io/guest_driver.cpp
	io/io.cpp
	media/disc.cpp
	m33/m33.cpp
	m33/vsh_iso.cpp
	${PSPC_KERNEL_SOURCES}
	${PSPC_HLE_MODULES}
)
target_include_directories(pspc_runtime PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pspc_runtime PUBLIC pspc_sem pspc_loader pspc_fs pspc_net pspc_common SDL2::SDL2)
if(WIN32)
	target_link_libraries(pspc_runtime PRIVATE winmm)
else()
	target_link_libraries(pspc_runtime PRIVATE PkgConfig::PSPC_FONTS)
endif()
if(TARGET pspc_gpu)
	target_link_libraries(pspc_runtime PUBLIC pspc_gpu)
	target_compile_definitions(pspc_runtime PUBLIC PSPC_HAVE_GPU=1)
endif()
if(TARGET pspc_gpu_hw)
	target_link_libraries(pspc_runtime PUBLIC pspc_gpu_hw)
	target_compile_definitions(pspc_runtime PUBLIC PSPC_HAVE_GPU_HW=1)
endif()

if(TARGET pspc_gpu_gl)
	target_link_libraries(pspc_runtime PUBLIC pspc_gpu_gl)
	target_compile_definitions(pspc_runtime PUBLIC PSPC_HAVE_GPU_GL=1)
endif()

# pspc_version.h: the version in CMakeLists.txt, for the programs and their resources.
file(WRITE ${CMAKE_BINARY_DIR}/generated/pspc_version.h.tmp
	"#pragma once\n#define PSPC_VERSION \"${PROJECT_VERSION}\"\n#define PSPC_VERSION_MAJOR ${PROJECT_VERSION_MAJOR}\n"
	"#define PSPC_VERSION_MINOR ${PROJECT_VERSION_MINOR}\n#define PSPC_VERSION_PATCH ${PROJECT_VERSION_PATCH}\n")
file(COPY_FILE ${CMAKE_BINARY_DIR}/generated/pspc_version.h.tmp ${CMAKE_BINARY_DIR}/generated/pspc_version.h ONLY_IF_DIFFERENT)

add_executable(pspc ${CMAKE_SOURCE_DIR}/src/frontend/main.cpp)
target_link_libraries(pspc PRIVATE pspc_runtime)
target_include_directories(pspc PRIVATE ${CMAKE_BINARY_DIR}/generated)
if(WIN32)
	target_sources(pspc PRIVATE ${CMAKE_SOURCE_DIR}/src/frontend/pspc.rc)
	add_custom_command(TARGET pspc POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL2_ROOT}/lib/x64/SDL2.dll $<TARGET_FILE_DIR:pspc>)
endif()

if(WIN32)
	add_executable(pspc-launcher WIN32
		${CMAKE_SOURCE_DIR}/src/frontend/launcher.cpp
		${CMAKE_SOURCE_DIR}/src/frontend/network_dialog.cpp
		${CMAKE_SOURCE_DIR}/src/frontend/online_page.cpp
		${CMAKE_SOURCE_DIR}/src/frontend/plugins_dialog.cpp
		${CMAKE_SOURCE_DIR}/src/frontend/server_dialogs.cpp
		${CMAKE_SOURCE_DIR}/src/frontend/servers.cpp
		${CMAKE_SOURCE_DIR}/src/frontend/settings_dialog.cpp
		${CMAKE_SOURCE_DIR}/src/frontend/ui_theme.cpp
		${CMAKE_SOURCE_DIR}/src/frontend/launcher.rc
	)
	target_link_libraries(pspc-launcher PRIVATE pspc_fs pspc_net pspc_common comctl32 gdiplus dwmapi uxtheme)
	# Wide-character Win32 macros (ListView_InsertItem & co. take LVITEMW).
	target_compile_definitions(pspc-launcher PRIVATE UNICODE _UNICODE)
else()
	pkg_check_modules(PSPC_GTK REQUIRED IMPORTED_TARGET gtk+-3.0>=3.22)
	add_executable(pspc-launcher
		${CMAKE_SOURCE_DIR}/src/frontend/launcher_gtk.cpp
		${CMAKE_SOURCE_DIR}/src/frontend/gtk_dialogs.cpp
		${CMAKE_SOURCE_DIR}/src/frontend/servers.cpp)
	target_link_libraries(pspc-launcher PRIVATE PkgConfig::PSPC_GTK pspc_net)
endif()
target_link_libraries(pspc-launcher PRIVATE pspc_game_library)
# PSPC_SEM_VERSION: the launcher tells current AOT DLLs from stale ones; pspc_version.h.
target_include_directories(pspc-launcher PRIVATE ${CMAKE_BINARY_DIR}/generated)

# sceUtility dialogs (src/runtime/dialogs/).
file(GLOB PSPC_DIALOG_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dialogs/*.cpp)
target_sources(pspc_runtime PRIVATE ${PSPC_DIALOG_SOURCES})

if(TARGET pspc_audio)
	target_link_libraries(pspc_runtime PUBLIC pspc_audio)
endif()
if(TARGET pspc_media)
	target_link_libraries(pspc_runtime PUBLIC pspc_media)
endif()
