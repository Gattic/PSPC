# The optional Direct3D 11 renderer backend (gpu/backend.h, d3d11_backend.h).
add_library(pspc_gpu_hw STATIC
	d3d11_backend.cpp
	d3d11_shaders.cpp
	d3d11_texcache.cpp
)
target_link_libraries(pspc_gpu_hw PUBLIC pspc_gpu PRIVATE d3d11 dxgi d3dcompiler)
if(MSVC)
	target_compile_options(pspc_gpu_hw PRIVATE $<$<CONFIG:RelWithDebInfo>:/O2 /Ob2>)
endif()
