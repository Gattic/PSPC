set(SDL2_ROOT ${CMAKE_SOURCE_DIR}/third_party/SDL2-2.32.10)

add_library(SDL2::SDL2 SHARED IMPORTED)
set_target_properties(SDL2::SDL2 PROPERTIES
	IMPORTED_LOCATION ${SDL2_ROOT}/lib/x64/SDL2.dll
	IMPORTED_IMPLIB ${SDL2_ROOT}/lib/x64/SDL2.lib
	INTERFACE_INCLUDE_DIRECTORIES ${SDL2_ROOT}/include
)

file(GLOB PSPC_HLE_MODULES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hle/modules/*.cpp)
file(GLOB PSPC_KERNEL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernel/*.cpp)

add_library(pspc_runtime STATIC
	boot.cpp
	runtime.cpp
	core/backtrace.cpp
	core/cpu.cpp
	core/dispatch.cpp
	core/interp.cpp
	core/memory.cpp
	hle/hle.cpp
	host/sdl_host.cpp
	io/io.cpp
	media/disc.cpp
	m33/m33.cpp
	${PSPC_KERNEL_SOURCES}
	${PSPC_HLE_MODULES}
)
target_include_directories(pspc_runtime PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pspc_runtime PUBLIC pspc_sem pspc_loader pspc_fs pspc_common SDL2::SDL2 winmm)
if(TARGET pspc_gpu)
	target_link_libraries(pspc_runtime PUBLIC pspc_gpu)
	target_compile_definitions(pspc_runtime PUBLIC PSPC_HAVE_GPU=1)
endif()

add_executable(pspc ${CMAKE_SOURCE_DIR}/src/frontend/main.cpp)
target_link_libraries(pspc PRIVATE pspc_runtime)
add_custom_command(TARGET pspc POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL2_ROOT}/lib/x64/SDL2.dll $<TARGET_FILE_DIR:pspc>
)

add_executable(pspc-iso ${CMAKE_SOURCE_DIR}/src/frontend/iso_tool.cpp)
target_link_libraries(pspc-iso PRIVATE pspc_fs pspc_common)

add_executable(pspc-launcher WIN32 ${CMAKE_SOURCE_DIR}/src/frontend/launcher.cpp)
target_link_libraries(pspc-launcher PRIVATE pspc_fs pspc_common comctl32 gdiplus)

# sceUtility dialogs (src/runtime/dialogs/).
file(GLOB PSPC_DIALOG_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dialogs/*.cpp)
target_sources(pspc_runtime PRIVATE ${PSPC_DIALOG_SOURCES})

if(TARGET pspc_audio)
	target_link_libraries(pspc_runtime PUBLIC pspc_audio)
endif()
if(TARGET pspc_media)
	target_link_libraries(pspc_runtime PUBLIC pspc_media)
endif()
