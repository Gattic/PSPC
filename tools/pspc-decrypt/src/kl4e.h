// SPDX-License-Identifier: GPL-3.0-or-later
// KL4E / KL3E - Sony's LZ77-with-arithmetic-coding scheme for compressed PSP executables, the
// alternative to gzip inside a ~PSP module. KL3E is the same bitstream with one constant changed.
// Ported from PPSSPP's Core/Util/KL4E.h (GPL-2.0-or-later, Copyright (c) 2026- PPSSPP Project).
#pragma once

#include "common.h"

constexpr int KL4E_ERROR_INVALID_SIZE = (int)0x80000104;    // output didn't fit
constexpr int KL4E_ERROR_INVALID_FORMAT = (int)0x80000108;  // stream is malformed

// Decompresses one KL4E/KL3E stream. `in` points at the five-byte stream header, i.e. *past* the
// four-byte "KL4E"/"KL3E" magic. `inSize` bounds reads so a truncated file can't run off the end.
// Returns the number of bytes written, or one of the negative errors above. If `end` is non-null
// it receives a pointer just past the last input byte consumed.
int DecompressKL4E(u8 *out, int outSize, const u8 *in, size_t inSize, const u8 **end, bool isKL3E);

// True if the buffer starts with a "KL4E" or "KL3E" magic. Sets *isKL3E when it does.
bool IsKL4EMagic(const u8 *data, size_t size, bool *isKL3E);
