# Libraries are split so the recompiler tool, the runtime and the AOT module DLLs can link only
# what they need. pspc_sem (decode + semantics) is the only thing an AOT DLL links.

add_library(pspc_common STATIC
	common/log.cpp
	common/fileutil.cpp
	common/sha1.cpp
)
target_include_directories(pspc_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB PSPC_VFPU_EXTRA_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/allegrex/vfpu_*.cpp)
add_library(pspc_sem STATIC
	allegrex/decode.cpp
	allegrex/disasm.cpp
	allegrex/sem_tables.cpp
	${PSPC_VFPU_EXTRA_SOURCES}
)
target_include_directories(pspc_sem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pspc_sem PUBLIC pspc_common)

# Each component directory owns its CMakeLists.txt; order matters only for dependencies.
foreach(component fs loader gpu audio hle runtime recomp tools)
	if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${component}/CMakeLists.txt)
		add_subdirectory(${component})
	endif()
endforeach()
