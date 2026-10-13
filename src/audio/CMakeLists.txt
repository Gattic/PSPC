# PSP audio: codecs (ATRAC3/ATRAC3+, MP3, AAC), the sceSas software mixer and helpers.
# Pure library: it never touches the runtime directly. Guest memory is reached through the
# accessor in guest_memory.h, which the runtime installs.

file(GLOB PSPC_AT3_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/at3_standalone/*.cpp)
add_library(pspc_at3_standalone STATIC ${PSPC_AT3_SOURCES})
target_include_directories(pspc_at3_standalone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pspc_at3_standalone PRIVATE pspc_common)
if(MSVC)
	target_compile_options(pspc_at3_standalone PRIVATE /wd4305 /wd4101 /wd4018 /wd4554
		$<$<CONFIG:RelWithDebInfo>:/O2 /Ob2>)
endif()

file(GLOB PSPC_AUDIO_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(WIN32)
	list(FILTER PSPC_AUDIO_SOURCES EXCLUDE REGEX "/ffmpeg_decoder.cpp$")
else()
	list(FILTER PSPC_AUDIO_SOURCES EXCLUDE REGEX "/mf_decoder.cpp$")
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET GLOBAL libavcodec libavutil libswresample libswscale)
endif()
add_library(pspc_audio STATIC ${PSPC_AUDIO_SOURCES})
target_include_directories(pspc_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pspc_audio PUBLIC pspc_common PRIVATE pspc_at3_standalone)
if(WIN32)
	target_link_libraries(pspc_audio PRIVATE mfplat mfuuid wmcodecdspuuid ole32)
else()
	target_link_libraries(pspc_audio PRIVATE PkgConfig::AVCODEC)
endif()
if(MSVC)
	target_compile_options(pspc_audio PRIVATE $<$<CONFIG:RelWithDebInfo>:/O2 /Ob2>)
endif()
