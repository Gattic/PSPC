# Linux's flat application prefix is relocatable; libraries come from the host distro.
# Never ship a developer's dependency search path or silently bundle its library tree.
set_target_properties(pspc pspc-launcher pspc-recomp pspc-iso pspc-decrypt PROPERTIES
	INSTALL_RPATH "" INSTALL_RPATH_USE_LINK_PATH FALSE INSTALL_REMOVE_ENVIRONMENT_RPATH TRUE)

install(TARGETS pspc pspc-launcher RUNTIME DESTINATION . COMPONENT Runtime)
set(PSPC_RUNTIME_DATA
	data/fw_boot_modules.csv data/infrastructure.json data/m33_nid_resolver.csv
	data/nids_by_library.json data/nids.csv data/psplibdoc.xml
	data/pspc.png) # Existing 256px PNG extracted unchanged from src/frontend/pspc.ico.
install(FILES ${PSPC_RUNTIME_DATA} DESTINATION data COMPONENT Runtime)
install(DIRECTORY server/pspc_network server/pspc_medius server/pspc_gamespy
	DESTINATION server COMPONENT Runtime FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE)
install(FILES server/README.md DESTINATION server COMPONENT Runtime)
install(FILES tools/pspc_install_fw.py tools/pspc_desktop.py DESTINATION tools COMPONENT Runtime)
install(FILES README.md PLAN.md DESTINATION . COMPONENT Runtime)
install(DIRECTORY docs/ DESTINATION docs COMPONENT Runtime FILES_MATCHING PATTERN "*.md")
install(FILES src/audio/third_party/at3_standalone/LICENSE DESTINATION licenses/at3_standalone COMPONENT Runtime)

# A source snapshot from the very tree used to build this package, including local changes.
# Allow code/build-input formats only; never traverse the repository's build/private/reference/
# Games directories or sweep executable-local saves, firmware, caches or databases into an archive.
install(DIRECTORY src tools tests cmake DESTINATION source COMPONENT Sources FILES_MATCHING
	PATTERN "*.cpp" PATTERN "*.c" PATTERN "*.h" PATTERN "*.inc" PATTERN "*.S"
	PATTERN "*.py" PATTERN "*.sh" PATTERN "CMakeLists.txt" PATTERN "*.cmake" PATTERN "*.in"
	PATTERN "*.rc" PATTERN "*.rc2" PATTERN "*.ico"
	PATTERN "LICENSE" PATTERN "COPYING" PATTERN "NOTICE" PATTERN "README*"
	PATTERN "__pycache__" EXCLUDE PATTERN ".pytest_cache" EXCLUDE)
install(FILES CMakeLists.txt build.sh LICENSE THIRD_PARTY.md README.md PLAN.md DESTINATION source COMPONENT Sources)
install(FILES .github/workflows/linux-package.yml DESTINATION source/.github/workflows COMPONENT Sources)
install(FILES ${PSPC_RUNTIME_DATA} DESTINATION source/data COMPONENT Sources)
install(DIRECTORY docs/ DESTINATION source/docs COMPONENT Sources FILES_MATCHING PATTERN "*.md")
install(DIRECTORY server/pspc_network server/pspc_medius server/pspc_gamespy
	DESTINATION source/server COMPONENT Sources FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE)
install(FILES server/README.md DESTINATION source/server COMPONENT Sources)

# One archive containing Runtime + AOT + Sources, not a source-tree archive with a denylist.
set(CPACK_GENERATOR TGZ)
set(CPACK_SOURCE_GENERATOR "")
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_PACKAGE_VENDOR "PSPC Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PSPC Linux runtime, GTK launcher and AOT SDK (host dependencies)")
set(CPACK_PACKAGE_FILE_NAME "PSPC-${PROJECT_VERSION}-Linux-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_STRIP_FILES TRUE)
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_VERBATIM_VARIABLES TRUE)
include(CPack)
