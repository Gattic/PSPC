add_library(pspc_loader STATIC
	elf.cpp
	module_tables.cpp
	psp_header.cpp
)
target_include_directories(pspc_loader PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pspc_loader PUBLIC pspc_common)
