# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 PSPC contributors
#
# Standalone, out-of-tree build of Humberto Naves' pspdecompiler.
#
# pspdecompiler itself is licensed under the Academic Free License v2.0 and
# libexpat under the MIT license. Both are built here only as a separate
# reverse-engineering helper tool; neither is ever linked into PSPC. Nothing
# under reference/ is modified: the sources are compiled in place and any
# portability fixes live in this directory (compile definitions / compat/).
#
# Usage (from the repo root; tools/build_pspdecompiler.cmd does this):
#   cmake -S tools/pspdecompiler -B build/tools/pspdecompiler -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/tools/pspdecompiler
# Result: build/tools/pspdecompiler/pspdecompiler[.exe]

cmake_minimum_required(VERSION 3.20)
project(pspdecompiler_tool LANGUAGES C)

set(PSPDECOMPILER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../reference/pspdecompiler"
    CACHE PATH "Path to the pspdecompiler source checkout")
get_filename_component(PSPDECOMPILER_SRC "${PSPDECOMPILER_SRC}" ABSOLUTE)

set(PSPDECOMPILER_EXPAT_SRC "${PSPDECOMPILER_SRC}/libexpat/expat"
    CACHE PATH "Path to the libexpat 'expat' directory (pspdecompiler submodule)")
get_filename_component(PSPDECOMPILER_EXPAT_SRC "${PSPDECOMPILER_EXPAT_SRC}" ABSOLUTE)

if(NOT EXISTS "${PSPDECOMPILER_SRC}/main.c")
  message(FATAL_ERROR "pspdecompiler sources not found at ${PSPDECOMPILER_SRC} "
                      "(pass -DPSPDECOMPILER_SRC=...)")
endif()
if(NOT EXISTS "${PSPDECOMPILER_EXPAT_SRC}/lib/xmlparse.c")
  message(FATAL_ERROR "libexpat not found at ${PSPDECOMPILER_EXPAT_SRC} "
                      "(run 'git submodule update --init' in pspdecompiler, "
                      "or pass -DPSPDECOMPILER_EXPAT_SRC=...)")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Self-contained executable: static CRT on MSVC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Put the executable directly in the build directory for every generator.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${cfg}" CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} "${CMAKE_BINARY_DIR}")
endforeach()

# --- libexpat (static, only the three library translation units) -----------
# The bundled expat (2.2.x) declares cmake_minimum_required(3.1.3), which
# CMake >= 4 refuses to add_subdirectory(); compiling lib/*.c directly avoids
# that. On _WIN32 expat pulls its configuration from lib/winconfig.h; other
# hosts get the equivalent settings as compile definitions.
add_library(pspdc_expat STATIC
  "${PSPDECOMPILER_EXPAT_SRC}/lib/xmlparse.c"
  "${PSPDECOMPILER_EXPAT_SRC}/lib/xmlrole.c"
  "${PSPDECOMPILER_EXPAT_SRC}/lib/xmltok.c"
)
target_include_directories(pspdc_expat PUBLIC "${PSPDECOMPILER_EXPAT_SRC}/lib")
target_compile_definitions(pspdc_expat PUBLIC XML_STATIC)
if(NOT WIN32)
  if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
    set(_expat_byteorder 4321)
  else()
    set(_expat_byteorder 1234)
  endif()
  target_compile_definitions(pspdc_expat PRIVATE
    XML_NS=1 XML_DTD=1 XML_CONTEXT_BYTES=1024 XML_DEV_URANDOM=1
    BYTEORDER=${_expat_byteorder})
endif()
if(MSVC)
  target_compile_definitions(pspdc_expat PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(pspdc_expat PRIVATE /W1)
endif()

# --- pspdecompiler -----------------------------------------------------------
# Same object list as the upstream Makefile. abi.c is an unused stub and
# allefast.c is #included by allegrex.c, so neither is compiled on its own.
set(PSPDECOMPILER_SOURCES
  allegrex.c analyser.c decoder.c switches.c subroutines.c liveness.c
  structures.c cfg.c dataflow.c operations.c ssa.c graph.c outgraph.c
  outcode.c output.c prx.c nids.c hash.c lists.c alloc.c utils.c
  constants.c relocs.c module.c main.c
)
list(TRANSFORM PSPDECOMPILER_SOURCES PREPEND "${PSPDECOMPILER_SRC}/")

add_executable(pspdecompiler ${PSPDECOMPILER_SOURCES})
# nids.c includes "libexpat/expat/lib/expat.h" relative to its own directory.
target_include_directories(pspdecompiler PRIVATE "${PSPDECOMPILER_SRC}")
target_link_libraries(pspdecompiler PRIVATE pspdc_expat)
set_target_properties(pspdecompiler PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)

# Upstream hashpool_destroy() free()s pointers read from never-allocated
# (uninitialised) fixedpool slots, which corrupts the heap on Windows as soon
# as -n is used. Make fixedpool chunk allocation zero-filling; see
# compat/zalloc.c. The macro also renames the xmalloc prototype pulled in
# from utils.h, so the redeclaration stays consistent.
target_sources(pspdecompiler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat/zalloc.c")
set_source_files_properties("${PSPDECOMPILER_SRC}/alloc.c" PROPERTIES
  COMPILE_DEFINITIONS "xmalloc=pspdc_xzalloc")

if(WIN32)
  # Upstream splits output base names on '/' only; compat/win_main.c
  # normalises '\' in argv and then calls upstream main (renamed).
  target_sources(pspdecompiler PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat/win_main.c")
  set_source_files_properties("${PSPDECOMPILER_SRC}/main.c" PROPERTIES
    COMPILE_DEFINITIONS "main=pspdecompiler_main")
endif()

# Diagnostics are prefixed with __FILE__; strip the source directory so
# messages read "subroutines.c: ..." instead of an absolute path.
file(TO_NATIVE_PATH "${PSPDECOMPILER_SRC}/" _pspdc_src_prefix)
if(MSVC)
  target_compile_definitions(pspdecompiler PRIVATE
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
  # Upstream code is C89 from the 32-bit era: silence the noisy but harmless
  # size_t -> int / signed-unsigned conversion warnings.
  target_compile_options(pspdecompiler PRIVATE /W3 /wd4244 /wd4267 /wd4018
    "/d1trimfile:${_pspdc_src_prefix}")
else()
  target_compile_options(pspdecompiler PRIVATE -Wall
    "-ffile-prefix-map=${_pspdc_src_prefix}=")
  target_link_libraries(pspdecompiler PRIVATE m)
endif()
