// SPDX-License-Identifier: GPL-3.0-or-later
// A small DEFLATE decoder (RFC 1951) with the zlib (RFC 1950) and gzip (RFC 1952) wrappers.
// Written for pspc-decrypt; PRX payloads are gzip and PSAR contents are zlib.
#pragma once

#include "common.h"

enum class DeflateWrapper { Raw, Zlib, Gzip };

// Decodes one stream starting at `in`. On success `out` holds the data and `consumed` how many
// input bytes the stream (wrapper and trailer included) took up; anything after that is not
// touched. Checksums in the wrapper are verified. `maxOut` bounds the output size.
bool InflateStream(const u8 *in, size_t inSize, DeflateWrapper wrapper, Bytes *out, size_t *consumed,
	std::string *error, size_t maxOut = 512u * 1024 * 1024);

inline bool IsGzipMagic(const u8 *p, size_t size) { return size >= 3 && p[0] == 0x1F && p[1] == 0x8B && p[2] == 8; }
// A zlib header: deflate method, window <= 32K, valid check bits, no preset dictionary.
bool IsZlibHeader(const u8 *p, size_t size);

u32 Crc32(const u8 *data, size_t size, u32 crc = 0);
u32 Adler32(const u8 *data, size_t size, u32 adler = 1);
