// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP's Core/HW/SasReverb.cpp (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
//
// The formula is nocash's (http://problemkaputt.de/psx-spx.htm#spureverbformula); the preset
// constants are the PSP's own.
#include "audio/sas_reverb.h"

#include <cstring>

#include "audio/audio_common.h"

namespace audio {

struct SasReverbData {
	const char *name;
	int32_t size;

	int16_t dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL;
	int16_t vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2;
	int16_t dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4;
	int16_t dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2;
};

static const SasReverbData presets[9] = {
	{
		"Room",
		0x26C0,
		0x007D, 0x005B, 0x6D80, 0x54B8, (int16_t)0xBED0, 0x0000, 0x0000, (int16_t)0xBA80,
		0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
		0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5,
		0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C,
	},
	{
		"Studio Small",
		0x1F40,
		0x0033, 0x0025, 0x70F0, 0x4FA8, (int16_t)0xBCE0, 0x4410, (int16_t)0xC0F0, (int16_t)0x9C00,
		0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266,
		0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7,
		0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026,
	},
	{
		"Studio Medium",
		0x4840,
		0x00B1, 0x007F, 0x70F0, 0x4FA8, (int16_t)0xBCE0, 0x4510, (int16_t)0xBEF0, (int16_t)0xB4C0,
		0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
		0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
		0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080,
	},
	{
		"Studio Large",
		0x6FE0,
		0x00E3, 0x00A9, 0x6F60, 0x4FA8, (int16_t)0xBCE0, 0x4510, (int16_t)0xBEF0, (int16_t)0xA680,
		0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
		0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2,
		0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA,
	},
	{
		"Hall",
		0xADE0,
		0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, (int16_t)0xB800, (int16_t)0xBC00, (int16_t)0xC000,
		0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1,
		0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD,
		0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A,
	},
	{
		"Space Echo",
		0xF6C0,
		0x033D, 0x0231, 0x7E00, 0x5000, (int16_t)0xB400, (int16_t)0xB000, 0x4C00, (int16_t)0xB000,
		0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2,
		0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D,
		0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232,
	},
	// Echo and Delay are recomputed from sceSasRevParam's delay and feedback (ApplyParams).
	{
		"Echo (almost infinite)",
		0x18040,
		0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, (int16_t)0x8100,
		0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
		0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE,
		0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004,
	},
	{
		"Delay (one - shot echo)",
		0x18040,
		0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000,
		0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
		0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE,
		0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004,
	},
	{
		"Half Echo",
		0x3C00,
		0x0017, 0x0013, 0x70F0, 0x4FA8, (int16_t)0xBCE0, 0x4510, (int16_t)0xBEF0, (int16_t)0x8500,
		0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7,
		0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059,
		0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014,
	},
};

static const int kNumPresets = (int)(sizeof(presets) / sizeof(presets[0]));
enum { EFFECT_ECHO = 6, EFFECT_DELAY = 7 };

SasReverb::SasReverb() : preset_(-1), pos_(0) {
	workspace_ = new int16_t[BUFSIZE];
	memset(workspace_, 0, sizeof(int16_t) * BUFSIZE);
	data_ = new SasReverbData{};
}

SasReverb::~SasReverb() {
	delete[] workspace_;
	delete data_;
}

const char *SasReverb::GetPresetName(int preset) {
	if (preset == -1) return "Off";
	if (preset < 0 || preset >= kNumPresets) return "Invalid";
	return presets[preset].name;
}

void SasReverb::SetPreset(int preset) {
	if (preset >= -1 && preset < kNumPresets) preset_ = preset;
	if (preset_ != -1) {
		pos_ = BUFSIZE - presets[preset_].size;
		memset(workspace_, 0, sizeof(int16_t) * BUFSIZE);
	} else {
		pos_ = 0;
	}
	ApplyParams();
}

void SasReverb::SetParams(int delay, int feedback) {
	delay_ = delay;
	feedback_ = feedback;
	ApplyParams();
}

void SasReverb::ApplyParams() {
	if (preset_ == -1) return;
	*data_ = presets[preset_];
	if (preset_ != EFFECT_ECHO && preset_ != EFFECT_DELAY) return;
	const int d16 = (delay_ + 1) * 16;
	data_->mLSAME = (int16_t)(d16 * 2 - data_->dAPF1);
	data_->mRSAME = (int16_t)(d16 - data_->dAPF2);
	data_->mLCOMB1 = (int16_t)(data_->mRCOMB1 + d16);
	data_->dLSAME = (int16_t)(data_->dRSAME + d16);
	data_->mLAPF1 = (int16_t)(data_->mLAPF2 + d16);
	data_->mRAPF1 = (int16_t)(d16 + (data_->mLAPF2 >> 1));
	data_->vWALL = (int16_t)(-(feedback_ << 8));
}

namespace {

// Wraps around the upper part of a buffer.
template <int bufsize>
class BufferWrapper {
public:
	BufferWrapper(int16_t *buffer, int position, int usedSize)
		: buf_(buffer), pos_(position), end_(bufsize), base_(bufsize - usedSize), size_(usedSize) {}
	int16_t &operator[](int index) {
		int addr = pos_ + index;
		if (addr >= end_) addr -= size_;
		if (addr < base_) addr += size_;
		return buf_[addr];
	}
	int GetPosition() const { return pos_; }
	void Next() {
		pos_++;
		if (pos_ >= end_) pos_ -= size_;
	}

private:
	int16_t *buf_;
	int pos_;
	int end_;
	int base_;
	int size_;
};

}  // namespace

void SasReverb::ProcessReverb(int16_t *output, const int16_t *input, size_t inputSize, int volLeft, int volRight) {
	if (preset_ == -1) {
		// OFF is special-cased: the input is replicated, not zero-filled every other frame.
		for (size_t i = 0; i < inputSize; ++i) {
			output[i * 4 + 0] = ClampS16((int)input[i * 2 + 0] * volLeft >> 15);
			output[i * 4 + 1] = ClampS16((int)input[i * 2 + 1] * volRight >> 15);
			output[i * 4 + 2] = ClampS16((int)input[i * 2 + 0] * volLeft >> 15);
			output[i * 4 + 3] = ClampS16((int)input[i * 2 + 1] * volRight >> 15);
		}
		return;
	}

	// The ME's reverb return is (evol * out) >> 11; the caller passes vol << 3 for >> 15.
	volLeft <<= 1;
	volRight <<= 1;

	const SasReverbData &d = *data_;
	BufferWrapper<BUFSIZE> b(workspace_, pos_, d.size);

	for (size_t i = 0; i < inputSize; i++) {
		// The ME feeds the reverb at a quarter of the mix level.
		int16_t LeftInput = input[i * 2] >> 2;
		int16_t RightInput = input[i * 2 + 1] >> 2;
		int16_t Lin = LeftInput;
		int16_t Rin = RightInput;

		b[d.mLSAME] = ClampS16(Lin + (b[d.dLSAME] * d.vWALL >> 15) - (b[d.mLSAME - 1] * d.vIIR >> 15) + b[d.mLSAME - 1]);
		b[d.mRSAME] = ClampS16(Rin + (b[d.dRSAME] * d.vWALL >> 15) - (b[d.mRSAME - 1] * d.vIIR >> 15) + b[d.mRSAME - 1]);
		b[d.mLDIFF] = ClampS16(Lin + (b[d.dRDIFF] * d.vWALL >> 15) - (b[d.mLDIFF - 1] * d.vIIR >> 15) + b[d.mLDIFF - 1]);
		b[d.mRDIFF] = ClampS16(Rin + (b[d.dLDIFF] * d.vWALL >> 15) - (b[d.mRDIFF - 1] * d.vIIR >> 15) + b[d.mRDIFF - 1]);
		int32_t Lout = ((d.vCOMB1 * b[d.mLCOMB1] + d.vCOMB2 * b[d.mLCOMB2] + d.vCOMB3 * b[d.mLCOMB3] + d.vCOMB4 * b[d.mLCOMB4]) >> 15);
		int32_t Rout = ((d.vCOMB1 * b[d.mRCOMB1] + d.vCOMB2 * b[d.mRCOMB2] + d.vCOMB3 * b[d.mRCOMB3] + d.vCOMB4 * b[d.mRCOMB4]) >> 15);
		b[d.mLAPF1] = ClampS16(Lout - (d.vAPF1 * b[(d.mLAPF1 - d.dAPF1)] >> 15));
		Lout = b[(d.mLAPF1 - d.dAPF1)] + (b[d.mLAPF1] * d.vAPF1 >> 15);
		b[d.mRAPF1] = ClampS16(Rout - (d.vAPF1 * b[(d.mRAPF1 - d.dAPF1)] >> 15));
		Rout = b[(d.mRAPF1 - d.dAPF1)] + (b[d.mRAPF1] * d.vAPF1 >> 15);
		b[d.mLAPF2] = ClampS16(Lout - (d.vAPF2 * b[(d.mLAPF2 - d.dAPF2)] >> 15));
		Lout = b[(d.mLAPF2 - d.dAPF2)] + (b[d.mLAPF2] * d.vAPF2 >> 15);
		b[d.mRAPF2] = ClampS16(Rout - (d.vAPF2 * b[(d.mRAPF2 - d.dAPF2)] >> 15));
		Rout = b[(d.mRAPF2 - d.dAPF2)] + (b[d.mRAPF2] * d.vAPF2 >> 15);
		output[i * 4 + 0] = ClampS16((Lout * volLeft) >> 15);
		output[i * 4 + 1] = ClampS16((Rout * volRight) >> 15);
		output[i * 4 + 2] = 0;
		output[i * 4 + 3] = 0;
		b.Next();
	}
	pos_ = b.GetPosition();
}

}  // namespace audio
