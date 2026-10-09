# The PSP Graphics Engine: display list queue, command processor, software rasterizer.
add_library(pspc_gpu STATIC
	binner.cpp
	ge_core.cpp
	ge_state.cpp
	gemath.cpp
	raster_state.cpp
	rasterizer.cpp
	spline.cpp
	transform.cpp
	vertex.cpp
)
target_include_directories(pspc_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pspc_gpu PUBLIC pspc_sem)
if(MSVC)
	# The rasterizer is the hot path even in debug-info builds.
	target_compile_options(pspc_gpu PRIVATE $<$<CONFIG:RelWithDebInfo>:/O2 /Ob2>)
endif()
