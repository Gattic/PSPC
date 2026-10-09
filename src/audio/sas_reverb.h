// SPDX-License-Identifier: GPL-2.0-or-later
// The sceSas reverb: the PSX SPU reverb formula with the PSP's own presets.
// Ported from PPSSPP's Core/HW/SasReverb.{h,cpp} (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#pragma once

#include <cstddef>
#include <cstdint>

namespace audio {

struct SasReverbData;

class SasReverb {
public:
	SasReverb();
	~SasReverb();
	SasReverb(const SasReverb &) = delete;
	SasReverb &operator=(const SasReverb &) = delete;

	void SetPreset(int preset);
	// sceSasRevParam's delay and feedback (0..127); only Echo and Delay use them.
	void SetParams(int delay, int feedback);
	int GetPreset() const { return preset_; }
	static const char *GetPresetName(int preset);

	// Input is a mixdown of the send buffers at 22kHz; output is written at 44kHz (4 values
	// per input frame).
	void ProcessReverb(int16_t *output, const int16_t *input, size_t inputSize, int volLeft, int volRight);

private:
	enum { BUFSIZE = 0x20000 };
	void ApplyParams();

	int16_t *workspace_;
	SasReverbData *data_;
	int preset_;
	int pos_;
	int delay_ = 0;
	int feedback_ = 0;
};

}  // namespace audio
