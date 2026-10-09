// SPDX-License-Identifier: GPL-3.0-or-later
// LZRC ("2RLZ") decompression, as used by some PSP firmware files.
// Ported from PPSSPP's Core/FileSystems/tlzrc.cpp (GPL-2.0-or-later), "based on benhur's code,
// rewrite by tpu".
#pragma once

#include "common.h"

// `in` points at the stream header (the type byte), i.e. past a "2RLZ" magic if there was one.
// Returns the number of bytes written, or -1 on a malformed stream or overflow. `consumed`
// receives how many input bytes were used.
int DecompressLZRC(u8 *out, int outLen, const u8 *in, int inLen, int *consumed);
