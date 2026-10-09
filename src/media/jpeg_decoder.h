// SPDX-License-Identifier: GPL-2.0-or-later
//
// Baseline JPEG decoding for sceJpeg, through the Windows Imaging Component (WIC) codec that ships
// with Windows.
#pragma once

#include <vector>

#include "common/common.h"

namespace media {

struct JpegInfo {
	int width = 0;
	int height = 0;
	int components = 0;
	int hSamp[4] = {};
	int vSamp[4] = {};
	int sofType = -1;  // 0 baseline, 1 extended, 2 progressive...
};

// Reads the frame header (SOFn). False if there is none before the scan.
bool ParseJpegHeader(const u8 *data, size_t size, JpegInfo *info);

// Decodes to packed 8-bit R,G,B. False if WIC can't decode it.
bool DecodeJpegRgb(const u8 *data, size_t size, int *width, int *height, std::vector<u8> *rgb);

}  // namespace media
