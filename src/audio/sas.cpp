// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP's Core/HW/SasAudio.cpp (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later). The envelope rate tables originate
// from JPCSP's sceSasCore.
#include "audio/sas.h"

#include <algorithm>
#include <cstring>

#include "audio/audio_common.h"
#include "audio/guest_memory.h"
#include "common/log.h"

namespace audio {

static SasAtracHooks g_atracHooks;

void SetSasAtracHooks(const SasAtracHooks &hooks) { g_atracHooks = hooks; }

static const u8 f[16][2] = {
	{0, 0}, {60, 0}, {115, 52}, {98, 55}, {122, 60},
	// Past index 4 the hardware reads off the end of its own table.
	{0, 0}, {0, 0}, {52, 0}, {55, 2}, {60, 125}, {0, 0}, {0, 91}, {0, 0}, {2, 216}, {125, 6}, {0, 151},
};

void VagDecoder::Start(u32 data, u32 vagSize, bool loopEnabled) {
	loopEnabled_ = loopEnabled;
	loopAtNextBlock_ = false;
	loopStartBlock_ = -1;
	numBlocks_ = (int)(vagSize / 16);
	end_ = false;
	data_ = data;
	read_ = data;
	curSample = 28;
	curBlock_ = -1;
	s_1 = 0;
	s_2 = 0;
}

void VagDecoder::DecodeBlock(const u8 *&read_pointer) {
	if (curBlock_ == numBlocks_ - 1) {
		end_ = true;
		return;
	}
	const u8 *readp = read_pointer;
	int predict_nr = *readp++;
	int shift_factor = predict_nr & 0xf;
	predict_nr >>= 4;
	int flags = *readp++;
	if (flags == 7) {
		end_ = true;
		return;
	} else if (flags == 6) {
		loopStartBlock_ = curBlock_;
	} else if (flags == 3) {
		if (loopEnabled_) loopAtNextBlock_ = true;
	}

	int s1 = s_1;
	int s2 = s_2;
	int coef1 = f[predict_nr][0];
	int coef2 = -f[predict_nr][1];
	for (int i = 0; i < 28; i += 2) {
		u8 d = *readp++;
		int sample1 = (short)((d & 0xf) << 12) >> shift_factor;
		int sample2 = (short)((d & 0xf0) << 8) >> shift_factor;
		s2 = ClampS16(sample1 + ((s1 * coef1 + s2 * coef2) >> 6));
		s1 = ClampS16(sample2 + ((s2 * coef1 + s1 * coef2) >> 6));
		samples[i] = (s16)s2;
		samples[i + 1] = (s16)s1;
	}
	s_1 = s1;
	s_2 = s2;
	curSample = 0;
	curBlock_++;
	read_pointer = readp;
}

void VagDecoder::GetSamples(s16 *outSamples, int numSamples) {
	if (end_) {
		memset(outSamples, 0, numSamples * sizeof(s16));
		return;
	}
	const u32 end = data_ + (u32)std::max(numBlocks_, 0) * 16;
	const u8 *base = GuestPtr(read_, end > read_ + 16 ? end - read_ : 16);
	if (!base) {
		LOGW("SAS", "Bad VAG samples address? %08x / %d", read_, numBlocks_);
		memset(outSamples, 0, numSamples * sizeof(s16));
		return;
	}
	const u8 *readp = base;
	const u8 *origp = readp;

	for (int i = 0; i < numSamples; i++) {
		if (curSample == 28) {
			if (loopAtNextBlock_) {
				// data_ starts at curBlock = -1.
				read_ = data_ + 16 * loopStartBlock_ + 16;
				readp = GuestPtr(read_, 16);
				if (!readp) {
					end_ = true;
					memset(&outSamples[i], 0, (numSamples - i) * sizeof(s16));
					return;
				}
				origp = readp;
				curBlock_ = loopStartBlock_;
				loopAtNextBlock_ = false;
			}
			DecodeBlock(readp);
			if (end_) {
				memset(&outSamples[i], 0, (numSamples - i) * sizeof(s16));
				return;
			}
		}
		outSamples[i] = samples[curSample++];
	}
	if (readp > origp) read_ += (u32)(readp - origp);
}

int SasAtrac3::SetContext(u32 contextAddr) {
	contextAddr_ = contextAddr;
	atracID_ = g_atracHooks.bindContext ? g_atracHooks.bindContext(contextAddr) : -1;
	queue_.clear();
	end_ = false;
	return 0;
}

void SasAtrac3::GetNextSamples(s16 *outbuf, int wantedSamples) {
	if (atracID_ < 0 || !g_atracHooks.decode) {
		end_ = true;
		memset(outbuf, 0, wantedSamples * sizeof(s16));
		return;
	}
	int finish = 0;
	while (!finish && (int)queue_.size() < wantedSamples) {
		int numSamples = 0;
		g_atracHooks.decode(atracID_, buf_, &numSamples, &finish);
		if (numSamples > 0)
			queue_.insert(queue_.end(), buf_, buf_ + std::min(numSamples, (int)(sizeof(buf_) / sizeof(buf_[0]))));
		else
			finish = 1;
	}
	int n = std::min(wantedSamples, (int)queue_.size());
	for (int i = 0; i < n; i++) {
		outbuf[i] = queue_.front();
		queue_.pop_front();
	}
	if (n < wantedSamples) memset(outbuf + n, 0, (wantedSamples - n) * sizeof(s16));
	end_ = finish == 1;
}

int SasAtrac3::Concatenate(u32 bufPtr, u32 addbytes) {
	if (atracID_ >= 0 && g_atracHooks.addStreamData) g_atracHooks.addStreamData(atracID_, bufPtr, addbytes);
	return 0;
}

// ---- ADSR ----

static int simpleRate(int n) {
	n &= 0x7F;
	if (n == 0x7F) return 0;
	int rate = ((7 - (n & 0x3)) << 26) >> (n >> 2);
	return rate == 0 ? 1 : rate;
}

static int exponentRate(int n) {
	n &= 0x7F;
	if (n == 0x7F) return 0;
	int rate = ((7 - (n & 0x3)) << 24) >> (n >> 2);
	return rate == 0 ? 1 : rate;
}

static int getAttackRate(int bitfield1) { return simpleRate(bitfield1 >> 8); }

static int getAttackType(int bitfield1) {
	return (bitfield1 & 0x8000) == 0 ? PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE : PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT;
}

static int getDecayRate(int bitfield1) {
	int n = (bitfield1 >> 4) & 0x000F;
	if (n == 0) return 0x7FFFFFFF;
	return (int)(0x80000000u >> n);
}

static int getSustainType(int bitfield2) { return (bitfield2 >> 14) & 3; }

static int getSustainRate(int bitfield2) {
	if (getSustainType(bitfield2) == PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE) return exponentRate(bitfield2 >> 6);
	return simpleRate(bitfield2 >> 6);
}

static int getReleaseType(int bitfield2) {
	return (bitfield2 & 0x0020) == 0 ? PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE : PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE;
}

static int getReleaseRate(int bitfield2) {
	int n = bitfield2 & 0x001F;
	if (n == 31) return 0;
	if (getReleaseType(bitfield2) == PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE) {
		if (n == 30) return 0x40000000;
		if (n == 29) return 1;
		return 0x10000000 >> n;
	}
	if (n == 0) return 0x7FFFFFFF;
	return (int)(0x80000000u >> n);
}

static int getSustainLevel(int bitfield1) { return ((bitfield1 & 0x000F) + 1) << 26; }

void ADSREnvelope::SetEnvelope(int flag, int a, int d, int s, int r) {
	if (flag & 0x1) attackType = (SasADSRCurveMode)a;
	if (flag & 0x2) decayType = (SasADSRCurveMode)d;
	if (flag & 0x4) sustainType = (SasADSRCurveMode)s;
	if (flag & 0x8) releaseType = (SasADSRCurveMode)r;
}

void ADSREnvelope::SetRate(int flag, int a, int d, int s, int r) {
	if (flag & 0x1) attackRate = a;
	if (flag & 0x2) decayRate = d;
	if (flag & 0x4) sustainRate = s;
	if (flag & 0x8) releaseRate = r;
}

void ADSREnvelope::SetSimpleEnvelope(u32 ADSREnv1, u32 ADSREnv2) {
	attackRate = getAttackRate((int)ADSREnv1);
	attackType = (SasADSRCurveMode)getAttackType((int)ADSREnv1);
	decayRate = getDecayRate((int)ADSREnv1);
	decayType = PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE;
	sustainRate = getSustainRate((int)ADSREnv2);
	sustainType = (SasADSRCurveMode)getSustainType((int)ADSREnv2);
	releaseRate = getReleaseRate((int)ADSREnv2);
	releaseType = (SasADSRCurveMode)getReleaseType((int)ADSREnv2);
	sustainLevel = getSustainLevel((int)ADSREnv1);
}

void ADSREnvelope::WalkCurve(int type, int rate) {
	s64 expDelta;
	switch (type) {
	case PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE: height_ += rate; break;
	case PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE: height_ -= rate; break;
	case PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT:
		if (height_ <= (s64)PSP_SAS_ENVELOPE_HEIGHT_MAX * 3 / 4)
			height_ += rate;
		else
			height_ += rate / 4;
		break;
	case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE:
		expDelta = height_ - PSP_SAS_ENVELOPE_HEIGHT_MAX;
		expDelta += (-expDelta * rate) >> 32;
		height_ = expDelta + PSP_SAS_ENVELOPE_HEIGHT_MAX - (rate + 3UL) / 4UL;
		break;
	case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_INCREASE:
		expDelta = height_ - PSP_SAS_ENVELOPE_HEIGHT_MAX;
		expDelta += (-expDelta * rate) >> 32;
		height_ = expDelta + 0x4000 + PSP_SAS_ENVELOPE_HEIGHT_MAX;
		break;
	case PSP_SAS_ADSR_CURVE_MODE_DIRECT: height_ = rate; break;
	}
}

void ADSREnvelope::SetState(ADSRState state) {
	if (height_ > PSP_SAS_ENVELOPE_HEIGHT_MAX) height_ = PSP_SAS_ENVELOPE_HEIGHT_MAX;
	state_ = state;
}

void ADSREnvelope::Step() {
	switch (state_) {
	case STATE_ATTACK:
		WalkCurve(attackType, attackRate);
		if (height_ >= PSP_SAS_ENVELOPE_HEIGHT_MAX || height_ < 0) SetState(STATE_DECAY);
		break;
	case STATE_DECAY:
		WalkCurve(decayType, decayRate);
		if (height_ < sustainLevel) SetState(STATE_SUSTAIN);
		break;
	case STATE_SUSTAIN:
		WalkCurve(sustainType, sustainRate);
		if (height_ <= 0) {
			height_ = 0;
			SetState(STATE_RELEASE);
		}
		break;
	case STATE_RELEASE:
		WalkCurve(releaseType, releaseRate);
		if (height_ <= 0) {
			height_ = 0;
			SetState(STATE_OFF);
		}
		break;
	case STATE_OFF: break;
	case STATE_KEYON:
		height_ = 0;
		SetState(STATE_KEYON_STEP);
		break;
	case STATE_KEYON_STEP:
		// 32 steps at 0 before key-on kicks in, as on hardware.
		height_++;
		if (height_ >= 31) {
			height_ = 0;
			SetState(STATE_ATTACK);
		}
		break;
	}
}

void ADSREnvelope::KeyOn() { SetState(STATE_KEYON); }
void ADSREnvelope::KeyOff() { SetState(STATE_RELEASE); }
void ADSREnvelope::End() {
	SetState(STATE_OFF);
	height_ = 0;
}

// ---- Voices ----

void SasVoice::Reset() {
	resampleHist[0] = 0;
	resampleHist[1] = 0;
}

void SasVoice::KeyOn() {
	envelope.KeyOn();
	if (type == VOICETYPE_VAG) {
		if (GuestValid(vagAddr, 1)) {
			vag.Start(vagAddr, vagSize, loop);
		} else {
			LOGE("SAS", "Invalid VAG address %08x", vagAddr);
			return;
		}
	}
	playing = true;
	on = true;
	paused = false;
	sampleFrac = 0;
}

void SasVoice::KeyOff() {
	on = false;
	envelope.KeyOff();
}

void SasVoice::ReadSamples(s16 *output, int numSamples) {
	switch (type) {
	case VOICETYPE_VAG: vag.GetSamples(output, numSamples); break;
	case VOICETYPE_PCM: {
		int needed = numSamples;
		s16 *out = output;
		while (needed > 0) {
			int size = std::min(pcmSize - pcmIndex, needed);
			if (!on) {
				pcmIndex = 0;
				break;
			}
			if (size > 0) {
				const u8 *src = GuestPtr(pcmAddr + pcmIndex * 2, (u32)size * 2);
				if (src)
					memcpy(out, src, size * 2);
				else
					memset(out, 0, size * 2);
			}
			pcmIndex += size;
			needed -= size;
			out += size;
			if (pcmIndex >= pcmSize) {
				if (!loop) break;
				pcmIndex = pcmLoopPos;
			}
		}
		if (needed > 0) memset(out, 0, needed * sizeof(s16));
		break;
	}
	case VOICETYPE_ATRAC3: atrac3.GetNextSamples(output, numSamples); break;
	case VOICETYPE_NOISE: {
		// nocash's PSX SPU noise generator: bits 0-1 of the clock are the step (4..7), bits 2-5
		// the shift. The PSP's exact sequence is unverified, but it is the same kind of clock.
		const int step = 4 + (noiseFreq & 3);
		const int shift = (noiseFreq >> 2) & 0xF;
		for (int i = 0; i < numSamples; i++) {
			noiseTimer -= step;
			const int parity = ((noiseLevel >> 15) ^ (noiseLevel >> 12) ^ (noiseLevel >> 11) ^ (noiseLevel >> 10) ^ 1) & 1;
			if (noiseTimer < 0) {
				noiseLevel = (u16)((noiseLevel << 1) | parity);
				noiseTimer += 0x20000 >> shift;
				if (noiseTimer < 0) noiseTimer += 0x20000 >> shift;
			}
			output[i] = (s16)noiseLevel;
		}
		break;
	}
	default: memset(output, 0, numSamples * sizeof(s16)); break;
	}
}

bool SasVoice::HaveSamplesEnded() const {
	switch (type) {
	case VOICETYPE_VAG: return vag.End();
	case VOICETYPE_PCM: return pcmIndex >= pcmSize;
	case VOICETYPE_ATRAC3: return atrac3.End();
	default: return false;
	}
}

// ---- Instance ----

SasInstance::SasInstance() {
	memset(&waveformEffect, 0, sizeof(waveformEffect));
	waveformEffect.type = PSP_SAS_EFFECT_TYPE_OFF;
	waveformEffect.isDryOn = 1;
	memset(mixTemp_, 0, sizeof(mixTemp_));
}

SasInstance::~SasInstance() { ClearGrainSize(); }

void SasInstance::ClearGrainSize() {
	delete[] mixBuffer;
	delete[] sendBuffer;
	delete[] sendBufferDownsampled;
	delete[] sendBufferProcessed;
	mixBuffer = nullptr;
	sendBuffer = nullptr;
	sendBufferDownsampled = nullptr;
	sendBufferProcessed = nullptr;
}

void SasInstance::SetGrainSize(int newGrainSize) {
	grainSize = newGrainSize;
	ClearGrainSize();
	mixBuffer = new int[grainSize * 2]();
	sendBuffer = new int[grainSize * 2]();
	sendBufferDownsampled = new s16[grainSize]();
	sendBufferProcessed = new s16[grainSize * 2]();
}

// Measured on a PSP at 222MHz (pspautotests audio/timing/sastiming): a fixed 110us plus 0.49us
// per grain sample, plus per playing voice and sample 0.445us + 0.0675us per unit of pitch
// ratio for VAG, 0.405us + 0.0675us for PCM and 0.41us for noise, plus 0.64us per sample with
// a reverb type set.
int SasInstance::EstimateMixUs() const {
	float us = 110.0f + 0.49f * grainSize;
	for (int v = 0; v < PSP_SAS_VOICES_MAX; v++) {
		const SasVoice &voice = voices[v];
		if (!voice.playing || voice.paused) continue;
		const float pitchRatio = voice.pitch / (float)PSP_SAS_PITCH_BASE;
		float perSample;
		switch (voice.type) {
		case VOICETYPE_PCM: perSample = 0.405f + 0.0675f * pitchRatio; break;
		case VOICETYPE_NOISE:
		case VOICETYPE_TRIWAVE:
		case VOICETYPE_PULSEWAVE: perSample = 0.41f; break;
		default: perSample = 0.445f + 0.0675f * pitchRatio; break;
		}
		us += perSample * grainSize;
	}
	if (waveformEffect.type >= 0 && waveformEffect.isWetOn) us += 0.64f * grainSize;
	return (int)us;
}

void SasInstance::MixVoice(SasVoice &voice) {
	switch (voice.type) {
	case VOICETYPE_VAG:
		if (!voice.vagAddr) break;
		[[fallthrough]];
	case VOICETYPE_PCM:
		if (voice.type == VOICETYPE_PCM && !voice.pcmAddr) break;
		[[fallthrough]];
	default: {
		// The first 32 samples after a key-on are zeros.
		int delay = 0;
		if (voice.envelope.NeedsKeyOn()) {
			const bool ignorePitch = voice.type == VOICETYPE_PCM && voice.pitch > PSP_SAS_PITCH_BASE;
			delay = ignorePitch ? 32 : (int)((32 * (u32)voice.pitch) >> PSP_SAS_PITCH_BASE_SHIFT);
			// VAG has one more sample of delay than PCM.
			if (voice.type == VOICETYPE_VAG) ++delay;
		}

		mixTemp_[0] = voice.resampleHist[0];
		mixTemp_[1] = voice.resampleHist[1];

		const int voicePitch = voice.pitch;
		u32 sampleFrac = voice.sampleFrac;
		int samplesToRead = (int)((sampleFrac + (u32)voicePitch * (u32)std::max(0, grainSize - delay)) >> PSP_SAS_PITCH_BASE_SHIFT);
		if (samplesToRead > (int)(sizeof(mixTemp_) / sizeof(mixTemp_[0])) - 2) {
			samplesToRead = (int)(sizeof(mixTemp_) / sizeof(mixTemp_[0])) - 2;
		}
		int readPos = 2;
		if (voice.envelope.NeedsKeyOn()) {
			readPos = 0;
			samplesToRead += 2;
		}
		voice.ReadSamples(&mixTemp_[readPos], samplesToRead);
		int tempPos = readPos + samplesToRead;

		for (int i = 0; i < delay; ++i) voice.envelope.Step();

		const bool needsInterp = voicePitch != PSP_SAS_PITCH_BASE || (sampleFrac & PSP_SAS_PITCH_MASK) != 0;
		for (int i = delay; i < grainSize; i++) {
			const s16 *s = mixTemp_ + (sampleFrac >> PSP_SAS_PITCH_BASE_SHIFT);
			int sample = s[0];
			if (needsInterp) {
				int fr = (int)(sampleFrac & PSP_SAS_PITCH_MASK);
				sample = s[0] - (((s[0] - s[1]) * fr) >> PSP_SAS_PITCH_BASE_SHIFT);
			}
			sampleFrac += (u32)voicePitch;

			// The envelope max is (1 << 30) - 1; reduce it to 14 bits, rounding.
			int envelopeValue = voice.envelope.GetHeight();
			voice.envelope.Step();
			envelopeValue = (envelopeValue + (1 << 14)) >> 15;
			sample = ((sample * envelopeValue) + (1 << 14)) >> 15;

			mixBuffer[i * 2] += (sample * voice.volumeLeft) >> 12;
			mixBuffer[i * 2 + 1] += (sample * voice.volumeRight) >> 12;
			sendBuffer[i * 2] += sample * voice.effectLeft >> 12;
			sendBuffer[i * 2 + 1] += sample * voice.effectRight >> 12;
		}

		voice.resampleHist[0] = mixTemp_[tempPos - 2];
		voice.resampleHist[1] = mixTemp_[tempPos - 1];
		voice.sampleFrac = sampleFrac - (u32)(tempPos - 2) * PSP_SAS_PITCH_BASE;

		if (voice.HaveSamplesEnded()) voice.envelope.End();
		if (voice.envelope.HasEnded()) {
			voice.playing = false;
			voice.on = false;
		}
	}
	}
}

void SasInstance::Mix(u32 outAddr, u32 inAddr, int leftVol, int rightVol, bool mute) {
	if (!mixBuffer) return;
	for (int v = 0; v < PSP_SAS_VOICES_MAX; v++) {
		SasVoice &voice = voices[v];
		if (!voice.playing || voice.paused) continue;
		MixVoice(voice);
	}
	if (mute) {
		memset(mixBuffer, 0, grainSize * sizeof(int) * 2);
		memset(sendBuffer, 0, grainSize * sizeof(int) * 2);
	}

	s16 *outp = (s16 *)GuestPtr(outAddr, 4 * grainSize);
	const s16 *inp = inAddr ? (const s16 *)GuestPtr(inAddr, 4 * grainSize) : nullptr;
	if (!outp) {
		LOGW("SAS", "Bad SAS Mix output address: %08x, grain=%d", outAddr, grainSize);
	} else if (outputMode == PSP_SAS_OUTPUTMODE_MIXED) {
		WriteMixedOutput(outp, inp, leftVol, rightVol);
	} else {
		s16 *rawOut = (s16 *)GuestPtr(outAddr, 8 * grainSize);
		if (rawOut) {
			s16 *outpL = rawOut + grainSize * 0;
			s16 *outpR = rawOut + grainSize * 1;
			s16 *outpSendL = rawOut + grainSize * 2;
			s16 *outpSendR = rawOut + grainSize * 3;
			for (int i = 0; i < grainSize * 2; i += 2) {
				*outpL++ = ClampS16(mixBuffer[i + 0]);
				*outpR++ = ClampS16(mixBuffer[i + 1]);
				*outpSendL++ = ClampS16(sendBuffer[i + 0]);
				*outpSendR++ = ClampS16(sendBuffer[i + 1]);
			}
		}
	}
	memset(mixBuffer, 0, grainSize * sizeof(int) * 2);
	memset(sendBuffer, 0, grainSize * sizeof(int) * 2);
}

void SasInstance::WriteMixedOutput(s16 *outp, const s16 *inp, int leftVol, int rightVol) {
	const bool dry = waveformEffect.isDryOn != 0;
	const bool wet = waveformEffect.isWetOn != 0;
	if (wet) ApplyWaveformEffect();

	for (int i = 0; i < grainSize * 2; i += 2) {
		int sampleL = 0, sampleR = 0;
		if (inp) {
			sampleL = ((*inp++) * leftVol >> 12);
			sampleR = ((*inp++) * rightVol >> 12);
		}
		if (dry) {
			sampleL += mixBuffer[i + 0];
			sampleR += mixBuffer[i + 1];
		}
		if (wet) {
			sampleL += sendBufferProcessed[i + 0];
			sampleR += sendBufferProcessed[i + 1];
		}
		*outp++ = ClampS16(sampleL);
		*outp++ = ClampS16(sampleR);
	}
}

void SasInstance::SetWaveformEffectType(int type) {
	if (type != waveformEffect.type) {
		waveformEffect.type = type;
		reverb_.SetPreset(type);
	}
}

void SasInstance::SetWaveformEffectParams(int delay, int feedback) {
	waveformEffect.delay = delay;
	waveformEffect.feedback = feedback;
	reverb_.SetParams(delay, feedback);
}

void SasInstance::ApplyWaveformEffect() {
	// Downsample the send buffer to 22kHz, naively.
	for (int i = 0; i < grainSize / 2; i++) {
		sendBufferDownsampled[i * 2] = ClampS16(sendBuffer[i * 4]);
		sendBufferDownsampled[i * 2 + 1] = ClampS16(sendBuffer[i * 4 + 1]);
	}
	// Volume max is 0x1000 while the factor is up to 0x8000; << 3 matches them.
	reverb_.ProcessReverb(sendBufferProcessed, sendBufferDownsampled, grainSize / 2, (u16)(waveformEffect.leftVol << 3),
	                      (u16)(waveformEffect.rightVol << 3));
}

}  // namespace audio
