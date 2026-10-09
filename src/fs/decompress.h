// SPDX-License-Identifier: GPL-2.0-or-later
// Self-contained decoders for the compressed disc image formats: DEFLATE (RFC 1951), its zlib
// wrapper (RFC 1950) and the LZ4 block format. Decode only; all bounds are checked.
#pragma once

#include "common/common.h"

namespace vfs {

// Raw DEFLATE stream. Returns the number of bytes written to `out`, or -1 on corrupt input or if
// the output would exceed `outCap`. `consumed` (optional) receives the input bytes used, rounded
// up to a whole byte.
s64 InflateRaw(const u8 *in, size_t inLen, u8 *out, size_t outCap, size_t *consumed = nullptr);

// zlib stream: checks and skips the 2-byte header, inflates, then verifies the Adler-32 trailer
// when it is present. Preset dictionaries are not supported.
s64 InflateZlib(const u8 *in, size_t inLen, u8 *out, size_t outCap);

// True if the first two bytes look like a zlib header (CM = 8, check bits valid, no dictionary).
bool IsZlibHeader(const u8 *in, size_t inLen);

// LZ4 block. Decodes until `outLen` bytes have been produced or the input ends, ignoring anything
// after that (CSO/ZSO blocks carry alignment padding). Returns the bytes produced, -1 if corrupt.
s64 Lz4DecompressBlock(const u8 *in, size_t inLen, u8 *out, size_t outLen);

}  // namespace vfs
