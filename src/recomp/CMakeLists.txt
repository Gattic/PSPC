add_library(pspc_recomp_lib STATIC
	analysis.cpp
	codegen.cpp
	image.cpp
)
target_include_directories(pspc_recomp_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pspc_recomp_lib PUBLIC pspc_loader pspc_sem pspc_common)

add_executable(pspc-recomp main.cpp)
target_link_libraries(pspc-recomp PRIVATE pspc_recomp_lib)
