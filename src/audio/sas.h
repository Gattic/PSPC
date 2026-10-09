// SPDX-License-Identifier: GPL-2.0-or-later
//
// The sceSas software mixer (it runs on the Media Engine on hardware): 32 voices of VAG ADPCM,
// PCM, noise or ATRAC3, ADSR envelopes, pitch, loops and the reverb send.
// Ported from PPSSPP's Core/HW/SasAudio.{h,cpp} (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#pragma once

#include <deque>

#include "audio/sas_reverb.h"
#include "common/common.h"

namespace audio {

enum {
	PSP_SAS_VOICES_MAX = 32,
	PSP_SAS_VOL_MAX = 0x1000,
	PSP_SAS_MAX_GRAIN = 2048,
	PSP_SAS_PITCH_MIN = 0x0000,
	PSP_SAS_PITCH_BASE = 0x1000,
	PSP_SAS_PITCH_MASK = 0xFFF,
	PSP_SAS_PITCH_BASE_SHIFT = 12,
	PSP_SAS_PITCH_MAX = 0x4000,
	PSP_SAS_ENVELOPE_HEIGHT_MAX = 0x40000000,
	PSP_SAS_ENVELOPE_FREQ_MAX = 0x7FFFFFFF,
};

enum SasADSRCurveMode : int {
	PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE = 0,
	PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE = 1,
	PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT = 2,
	PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE = 3,
	PSP_SAS_ADSR_CURVE_MODE_EXPONENT_INCREASE = 4,
	PSP_SAS_ADSR_CURVE_MODE_DIRECT = 5,
};

enum SasEffectType {
	PSP_SAS_EFFECT_TYPE_OFF = -1,
	PSP_SAS_EFFECT_TYPE_ROOM = 0,
	PSP_SAS_EFFECT_TYPE_ECHO = 6,
	PSP_SAS_EFFECT_TYPE_DELAY = 7,
	PSP_SAS_EFFECT_TYPE_MAX = 8,
};

enum SasOutputMode {
	PSP_SAS_OUTPUTMODE_MIXED = 0,
	PSP_SAS_OUTPUTMODE_RAW = 1,
};

struct WaveformEffect {
	int type;
	int delay;
	int feedback;
	int leftVol;
	int rightVol;
	int isDryOn;
	int isWetOn;
};

enum VoiceType {
	VOICETYPE_OFF,
	VOICETYPE_VAG,
	VOICETYPE_NOISE,
	VOICETYPE_TRIWAVE,
	VOICETYPE_PULSEWAVE,
	VOICETYPE_PCM,
	VOICETYPE_ATRAC3,
};

// How a sceSas ATRAC3 voice reaches sceAtrac (installed by the runtime's sceAtrac module).
struct SasAtracHooks {
	int (*bindContext)(u32 contextAddr) = nullptr;                       // returns the atrac ID
	void (*decode)(int atracID, s16 *out, int *samples, int *finish) = nullptr;  // one frame, mono
	int (*addStreamData)(int atracID, u32 bufPtr, u32 bytes) = nullptr;
};
void SetSasAtracHooks(const SasAtracHooks &hooks);

// Sony's ADPCM: 28 samples in each 16-byte block.
class VagDecoder {
public:
	void Start(u32 dataPtr, u32 vagSize, bool loopEnabled);
	void GetSamples(s16 *outSamples, int numSamples);
	void DecodeBlock(const u8 *&readp);
	bool End() const { return end_; }
	u32 GetReadPtr() const { return read_; }

private:
	s16 samples[28]{};
	int curSample = 0;
	u32 data_ = 0;
	u32 read_ = 0;
	int curBlock_ = -1;
	int loopStartBlock_ = -1;
	int numBlocks_ = 0;
	int s_1 = 0;
	int s_2 = 0;
	bool loopEnabled_ = false;
	bool loopAtNextBlock_ = false;
	bool end_ = true;
};

class SasAtrac3 {
public:
	int AtracID() const { return atracID_; }
	int SetContext(u32 context);
	void GetNextSamples(s16 *outbuf, int wantedSamples);
	int Concatenate(u32 bufPtr, u32 addbytes);
	bool End() const { return end_; }

private:
	u32 contextAddr_ = 0;
	int atracID_ = -1;
	std::deque<s16> queue_;
	bool end_ = false;
	s16 buf_[0x800]{};
};

class ADSREnvelope {
public:
	void SetSimpleEnvelope(u32 ADSREnv1, u32 ADSREnv2);
	void SetEnvelope(int flag, int a, int d, int s, int r);
	void SetRate(int flag, int a, int d, int s, int r);
	void SetSustainLevel(int sl) { sustainLevel = sl; }

	void WalkCurve(int type, int rate);
	void KeyOn();
	void KeyOff();
	void End();
	void Step();

	int GetHeight() const {
		return (int)(height_ > (s64)PSP_SAS_ENVELOPE_HEIGHT_MAX ? PSP_SAS_ENVELOPE_HEIGHT_MAX : height_);
	}
	bool NeedsKeyOn() const { return state_ == STATE_KEYON; }
	bool HasEnded() const { return state_ == STATE_OFF; }

	int attackRate = 0;
	int decayRate = 0;
	int sustainRate = 0;
	int sustainLevel = 0;
	int releaseRate = 0;

	SasADSRCurveMode attackType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE;
	SasADSRCurveMode decayType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
	SasADSRCurveMode sustainType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
	SasADSRCurveMode releaseType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;

private:
	enum ADSRState {
		STATE_KEYON_STEP = -42,
		STATE_KEYON = -2,
		STATE_OFF = -1,
		STATE_ATTACK = 0,
		STATE_DECAY = 1,
		STATE_SUSTAIN = 2,
		STATE_RELEASE = 3,
	};
	void SetState(ADSRState state);

	ADSRState state_ = STATE_OFF;
	s64 height_ = 0;
};

struct SasVoice {
	void Reset();
	void KeyOn();
	void KeyOff();

	void ReadSamples(s16 *output, int numSamples);
	bool HaveSamplesEnded() const;

	bool playing = false;
	bool paused = false;  // a voice can be playing AND paused; then it doesn't play
	bool on = false;      // key-on / key-off

	VoiceType type = VOICETYPE_OFF;

	u32 vagAddr = 0;
	u32 vagSize = 0;
	u32 pcmAddr = 0;
	int pcmSize = 0;
	int pcmIndex = 0;
	int pcmLoopPos = 0;
	int sampleRate = 44100;

	u32 sampleFrac = 0;
	int pitch = PSP_SAS_PITCH_BASE;
	bool loop = false;

	int noiseFreq = 0;

	int volumeLeft = PSP_SAS_VOL_MAX;
	int volumeRight = PSP_SAS_VOL_MAX;
	int effectLeft = PSP_SAS_VOL_MAX;
	int effectRight = PSP_SAS_VOL_MAX;
	s16 resampleHist[2]{};

	ADSREnvelope envelope;
	VagDecoder vag;
	SasAtrac3 atrac3;
};

class SasInstance {
public:
	SasInstance();
	~SasInstance();

	void ClearGrainSize();
	void SetGrainSize(int newGrainSize);
	int GetGrainSize() const { return grainSize; }
	// Media Engine time for one mix at 222MHz, measured on hardware.
	int EstimateMixUs() const;

	int maxVoices = PSP_SAS_VOICES_MAX;
	int sampleRate = 44100;
	int outputMode = PSP_SAS_OUTPUTMODE_MIXED;

	int *mixBuffer = nullptr;
	int *sendBuffer = nullptr;
	s16 *sendBufferDownsampled = nullptr;
	s16 *sendBufferProcessed = nullptr;

	// Mixes one grain into outAddr (stereo s16, or 4 planes in raw mode). inAddr, if nonzero, is
	// mixed in scaled by leftVol/rightVol (sceSasCoreWithMix).
	void Mix(u32 outAddr, u32 inAddr, int leftVol, int rightVol, bool mute);
	void MixVoice(SasVoice &voice);

	void ApplyWaveformEffect();
	void SetWaveformEffectType(int type);
	void SetWaveformEffectParams(int delay, int feedback);
	void WriteMixedOutput(s16 *outp, const s16 *inp, int leftVol, int rightVol);

	SasVoice voices[PSP_SAS_VOICES_MAX];
	WaveformEffect waveformEffect;

private:
	SasReverb reverb_;
	int grainSize = 0;
	s16 mixTemp_[PSP_SAS_MAX_GRAIN * 4 + 2 + 16];
};

}  // namespace audio
