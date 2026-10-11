# pspc_fs: storage layer under IoFileMgrForUser / sceUmd (disc images, ISO9660, host directories,
# PBP and PARAM.SFO).
add_library(pspc_fs STATIC
	block_device.cpp
	decompress.cpp
	fat_names.cpp
	filesystem.cpp
	host_fs.cpp
	iso_fs.cpp
	param_sfo.cpp
	path_util.cpp
	pbp.cpp
)
target_include_directories(pspc_fs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pspc_fs PUBLIC pspc_common)
# Narrowing conversions (C4244, C4267) are errors here, unlike the rest of the tree: sizes and
# offsets of disc images go past 4 GB, and a silent truncation is a wrong read.
if(MSVC)
	target_compile_options(pspc_fs PRIVATE /we4244 /we4267)
endif()
