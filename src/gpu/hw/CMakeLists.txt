if(WIN32)
	add_library(pspc_gpu_hw STATIC d3d11_backend.cpp d3d11_shaders.cpp d3d11_texcache.cpp)
	target_link_libraries(pspc_gpu_hw PUBLIC pspc_gpu PRIVATE d3d11 dxgi d3dcompiler)
	if(MSVC)
		target_compile_options(pspc_gpu_hw PRIVATE $<$<CONFIG:RelWithDebInfo>:/O2 /Ob2>)
	endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	find_package(OpenGL REQUIRED)
	find_package(SDL2 REQUIRED CONFIG)
	find_package(Python3 REQUIRED COMPONENTS Interpreter)
	set(GL_SHADERS ${CMAKE_BINARY_DIR}/generated/opengl_shaders.h)
	add_custom_command(OUTPUT ${GL_SHADERS}
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gl_shaders.py
			${CMAKE_CURRENT_SOURCE_DIR}/d3d11_shaders.cpp ${GL_SHADERS}
		DEPENDS d3d11_shaders.cpp ${CMAKE_SOURCE_DIR}/tools/gl_shaders.py VERBATIM)
	add_library(pspc_gpu_gl STATIC opengl_backend.cpp ${GL_SHADERS})
	target_include_directories(pspc_gpu_gl PRIVATE ${CMAKE_BINARY_DIR}/generated)
	target_link_libraries(pspc_gpu_gl PUBLIC pspc_gpu PRIVATE OpenGL::GL SDL2::SDL2)
endif()
