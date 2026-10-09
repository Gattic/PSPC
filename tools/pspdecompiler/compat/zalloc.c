/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 PSPC contributors */

/*
 * Zero-filling allocator for the out-of-tree pspdecompiler build.
 *
 * pspdecompiler (AFL-2.0, reference/pspdecompiler) is built as a separate
 * tool and never linked into PSPC. This file is our own glue, not part of
 * pspdecompiler.
 *
 * Upstream hashpool_destroy() (hash.c) runs destroy_hashtable() over every
 * slot of every fixedpool chunk, including slots that were never handed
 * out, and free()s their uninitialised 'table' pointer. On glibc the chunks
 * usually come from fresh, zeroed pages so this is free(NULL); on the Windows
 * CRT heap it is garbage and the process dies with STATUS_HEAP_CORRUPTION
 * (0xC0000374) whenever a NIDs file is loaded with -n. Upstream alloc.c is
 * compiled with -Dxmalloc=pspdc_xzalloc so fixedpool chunks are zeroed.
 */

#include <stdlib.h>

#include "utils.h"

void *pspdc_xzalloc (size_t size)
{
  void *ptr = calloc (1, size ? size : 1);
  if (!ptr) fatal ("zalloc.c: memory exhausted");
  return ptr;
}
