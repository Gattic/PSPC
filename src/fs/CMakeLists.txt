# pspc_fs: storage layer under IoFileMgrForUser / sceUmd (disc images, ISO9660, host directories,
# PBP and PARAM.SFO).
add_library(pspc_fs STATIC
	block_device.cpp
	decompress.cpp
	host_fs.cpp
	iso_fs.cpp
	param_sfo.cpp
	path_util.cpp
	pbp.cpp
)
target_include_directories(pspc_fs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pspc_fs PUBLIC pspc_common)
