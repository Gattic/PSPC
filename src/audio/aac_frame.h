// SPDX-License-Identifier: GPL-2.0-or-later
// The length of one AAC-LC raw_data_block (ISO 14496-3), found by walking its syntax without
// decoding it. The PSP's AAC decoder consumes exactly one such block per call; Media Foundation
// can't say how much it used.
#pragma once

#include "common/common.h"

namespace audio {

// Bytes the raw_data_block at data takes, or -1 if it doesn't parse as AAC-LC (or doesn't fit in
// size bytes). samplingIndex is the AAC sampling frequency index (44100 Hz = 4).
int AacRawDataBlockBytes(const u8 *data, int size, int samplingIndex);
// The sampling frequency index of a rate, or -1.
int AacSamplingIndex(int rate);

}  // namespace audio
