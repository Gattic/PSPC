# PSP media: the Media Engine's video side (program stream demuxing, H.264 through the Windows
# Media Foundation decoder), JPEG through WIC, and colour space conversion. ATRAC3+ audio in
# movies goes through pspc_audio's decoders.
# Pure library: guest memory is reached through the accessor in guest_memory.h, which the runtime
# installs.

file(GLOB PSPC_MEDIA_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(pspc_media STATIC ${PSPC_MEDIA_SOURCES})
target_include_directories(pspc_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pspc_media PUBLIC pspc_common pspc_audio PRIVATE mfplat mfuuid wmcodecdspuuid ole32 oleaut32 windowscodecs)
if(MSVC)
	target_compile_options(pspc_media PRIVATE $<$<CONFIG:RelWithDebInfo>:/O2 /Ob2>)
endif()
