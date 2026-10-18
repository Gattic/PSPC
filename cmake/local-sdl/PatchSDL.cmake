# Refuse drift or a fuzzy patch; repeated configuration must be idempotent.
set(source "${SOURCE_DIR}/src/video/wayland/SDL_waylandwindow.c")
set(header "${SOURCE_DIR}/src/video/wayland/SDL_waylandwindow.h")
set(expected_source "a9bb03ac6fb358fa3ac7606ce40559f6e75bf54d9656a3bcae65e3f465fbeae7")
set(expected_header "61812d55eb9c4a3176820e56c2f1397c9e6385442f9f08408833b17473166f56")
file(SHA256 "${source}" source_hash)
file(SHA256 "${header}" header_hash)
if(source_hash STREQUAL "${expected_source}" AND header_hash STREQUAL "${expected_header}")
	return()
endif()
if(NOT source_hash STREQUAL "fb6639c85a2e6b2be35df2b0ad0692683f65217416a3a7ad6e62f02d602e10c7" OR
   NOT header_hash STREQUAL "b87f2db76ff058bc7b4970a138d48122c51e5d21ce14a3790701482734474fa1")
	message(FATAL_ERROR "Unexpected or stale SDL Wayland source/header; recreate the generated local-sdl build tree before applying the current patch")
endif()
find_program(PATCH_EXECUTABLE patch REQUIRED)
execute_process(COMMAND "${PATCH_EXECUTABLE}" --batch --fuzz=0 -p1
	-i "${CMAKE_CURRENT_LIST_DIR}/wayland-configure-size.patch"
	WORKING_DIRECTORY "${SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
file(SHA256 "${source}" source_hash)
file(SHA256 "${header}" header_hash)
if(NOT source_hash STREQUAL "${expected_source}" OR NOT header_hash STREQUAL "${expected_header}")
	message(FATAL_ERROR "SDL fullscreen patch result differs from the validated source/header")
endif()
