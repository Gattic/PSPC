// SPDX-License-Identifier: GPL-2.0-or-later
// Tests for pspc_audio: MPEG audio header parsing, MP3/ATRAC3+ decoding on the pspautotests
// sample files (skipped when the reference tree isn't present), the VAG decoder and the sceSas
// mixer over a fake guest memory.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "audio/atrac_track.h"
#include "audio/decoder.h"
#include "audio/guest_memory.h"
#include "audio/mf_decoder.h"
#include "audio/sas.h"
#include "testing.h"

namespace {

std::vector<u8> LoadRef(const char *rel) {
	namespace fs = std::filesystem;
	fs::path p = fs::path(__FILE__).parent_path() / ".." / ".." / "reference" / "pspautotests" / "tests" / "audio" / rel;
	std::ifstream f(p, std::ios::binary);
	if (!f) return {};
	return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A flat fake guest address space starting at 0x08800000.
std::vector<u8> g_fakeRam(1 << 20);
constexpr u32 kFakeBase = 0x08800000;
u8 *FakeGuest(u32 addr, u32 size) {
	if (addr < kFakeBase || (u64)addr + size > kFakeBase + g_fakeRam.size()) return nullptr;
	return g_fakeRam.data() + (addr - kFakeBase);
}

int CountNonZero(const s16 *p, int n) {
	int c = 0;
	for (int i = 0; i < n; i++) c += p[i] != 0;
	return c;
}

}  // namespace

TEST(audio_mp3_header) {
	audio::Mp3FrameInfo info;
	const u8 h1[4] = {0xFF, 0xFB, 0x90, 0x64};  // MPEG1 L3 128kbps 44.1kHz joint stereo
	CHECK(audio::ParseMp3Header(h1, &info));
	CHECK_EQ(info.versionBits, 3);
	CHECK_EQ(info.layerBits, 1);
	CHECK_EQ(info.bitrateKbps, 128);
	CHECK_EQ(info.sampleRate, 44100);
	CHECK_EQ(info.channels, 2);
	CHECK_EQ(info.samplesPerFrame, 1152);
	CHECK_EQ(info.frameBytes, 417);
	const u8 h2[4] = {0xFF, 0xF3, 0x40, 0xC4};  // MPEG2 L3 32kbps 22.05kHz mono
	CHECK(audio::ParseMp3Header(h2, &info));
	CHECK_EQ(info.versionBits, 2);
	CHECK_EQ(info.sampleRate, 22050);
	CHECK_EQ(info.channels, 1);
	CHECK_EQ(info.samplesPerFrame, 576);
	const u8 bad[4] = {0xFF, 0xEB, 0x90, 0x64};  // reserved version
	CHECK(!audio::ParseMp3Header(bad, &info));
}

TEST(audio_mp3_decode_sample) {
	std::vector<u8> file = LoadRef("mp3/sample.mp3");
	if (file.empty()) {
		printf("  (skipped: reference/pspautotests not present)\n");
		return;
	}
	audio::AudioDecoder *dec = audio::CreateMp3Audio();
	std::vector<s16> pcm(1152 * 2);
	size_t pos = 0;
	int frames = 0, nonzeroFrames = 0;
	int firstNonzero = -1;
	while (pos + 4 < file.size() && frames < 40) {
		int consumed = 0, samples = 0;
		bool ok = dec->Decode(file.data() + pos, (int)(file.size() - pos), &consumed, 2, pcm.data(), &samples);
		if (!ok || consumed <= 0) break;
		CHECK_EQ(samples, 1152);
		int nz = CountNonZero(pcm.data(), samples * 2);
		if (nz && firstNonzero < 0) firstNonzero = frames;
		nonzeroFrames += nz > 0;
		pos += consumed;
		frames++;
	}
	printf("  mp3: %d frames, %d with sound, first at %d\n", frames, nonzeroFrames, firstNonzero);
	CHECK_EQ(frames, 40);
	CHECK(nonzeroFrames > 30);
	delete dec;
}

TEST(audio_atrac3plus_decode_sample) {
	std::vector<u8> file = LoadRef("atrac/sample.at3");
	if (file.empty()) {
		printf("  (skipped: reference/pspautotests not present)\n");
		return;
	}
	audio::Track track;
	std::string error;
	CHECK_EQ(audio::AnalyzeAtracTrack(file.data(), (u32)file.size(), (u32)file.size(), &track, &error), 0);
	CHECK_EQ(track.codecType, (u32)audio::PSP_CODEC_AT3PLUS);
	CHECK_EQ(track.channels, 2);
	CHECK(track.bytesPerFrame > 0);
	audio::AudioDecoder *dec = audio::CreateAtrac3PlusAudio(track.channels, track.bytesPerFrame);
	std::vector<s16> pcm(2048 * 2);
	int nonzero = 0;
	for (int f = 0; f < 8; f++) {
		const size_t off = track.dataByteOffset + (size_t)f * track.bytesPerFrame;
		if (off + track.bytesPerFrame > file.size()) break;
		int consumed = 0, samples = 0;
		CHECK(dec->Decode(file.data() + off, track.bytesPerFrame, &consumed, 2, pcm.data(), &samples));
		CHECK_EQ(samples, 2048);
		CHECK_EQ(consumed, (int)track.bytesPerFrame);
		nonzero += CountNonZero(pcm.data(), samples * 2) > 0;
	}
	CHECK(nonzero >= 6);
	delete dec;
}

TEST(audio_sas_vag_voice) {
	audio::SetGuestMemory(&FakeGuest);
	// A VAG stream: 4 blocks of a square-ish wave, then an end block.
	const u32 vagAddr = kFakeBase + 0x1000;
	u8 *vag = FakeGuest(vagAddr, 16 * 5);
	for (int b = 0; b < 4; b++) {
		vag[b * 16 + 0] = 0x00;  // predictor 0, shift 0
		vag[b * 16 + 1] = 0x00;
		for (int i = 2; i < 16; i++) vag[b * 16 + i] = 0x77;  // +7 nibbles
	}
	vag[4 * 16 + 1] = 7;  // end
	audio::SasInstance *sas = new audio::SasInstance();
	sas->SetGrainSize(256);
	audio::SasVoice &v = sas->voices[0];
	v.type = audio::VOICETYPE_VAG;
	v.vagAddr = vagAddr;
	v.vagSize = 16 * 5;
	v.envelope.SetSimpleEnvelope(0x000F, 0x1FC0);  // fast attack, no decay to speak of
	v.KeyOn();
	const u32 outAddr = kFakeBase + 0x10000;
	sas->Mix(outAddr, 0, 0, 0, false);
	const s16 *out = (const s16 *)FakeGuest(outAddr, 256 * 4);
	// The first 33 samples after key-on are silent, then the envelope ramps up.
	CHECK_EQ(CountNonZero(out, 32 * 2), 0);
	CHECK(CountNonZero(out + 64 * 2, 64 * 2) > 0);
	// 4 blocks = 112 samples, then the voice ends.
	sas->Mix(outAddr, 0, 0, 0, false);
	CHECK(!v.playing);
	delete sas;
}


// What the Media Foundation MP3 decoder does with the first frames (Mp3Audio compensates for it).
TEST(audio_mf_mp3_latency) {
	std::vector<u8> file = LoadRef("mp3/sample.mp3");
	if (file.empty()) return;
	audio::MFDecoder mf;
	if (!mf.Open(audio::MFDecoder::Format::MP3, 44100, 2)) {
		printf("  (skipped: no Media Foundation MP3 decoder)\n");
		return;
	}
	size_t pos = 0;
	while (pos + 4 < file.size() && !(file[pos] == 0xFF && (file[pos + 1] & 0xE0) == 0xE0)) pos++;
	for (int f = 0; f < 6; f++) {
		audio::Mp3FrameInfo info;
		if (!audio::ParseMp3Header(file.data() + pos, &info)) break;
		std::vector<s16> out;
		mf.Decode(file.data() + pos, info.frameBytes, out);
		printf("  frame %d: %d bytes in, %d samples out, %d nonzero\n", f, info.frameBytes, (int)out.size() / 2, CountNonZero(out.data(), (int)out.size()));
		pos += info.frameBytes;
	}
}


// Not a correctness test: prints what the mixers and decoders cost, for the performance notes.
TEST(audio_perf_report) {
	using clk = std::chrono::steady_clock;
	audio::SetGuestMemory(&FakeGuest);
	// SAS: 32 looping VAG voices at a 256-sample grain (a busy game).
	const u32 vagAddr = kFakeBase + 0x1000;
	u8 *vag = FakeGuest(vagAddr, 16 * 64);
	for (int b = 0; b < 64; b++) {
		vag[b * 16 + 0] = (u8)(0x10 | (b & 7));
		vag[b * 16 + 1] = b == 0 ? 6 : (b == 63 ? 3 : 0);
		for (int i = 2; i < 16; i++) vag[b * 16 + i] = (u8)(b * 37 + i * 11);
	}
	audio::SasInstance *sas = new audio::SasInstance();
	sas->SetGrainSize(256);
	for (int v = 0; v < 32; v++) {
		audio::SasVoice &voice = sas->voices[v];
		voice.type = audio::VOICETYPE_VAG;
		voice.vagAddr = vagAddr;
		voice.vagSize = 16 * 64;
		voice.loop = true;
		voice.pitch = 0x0800 + v * 0x80;
		voice.envelope.SetSimpleEnvelope(0x000F, 0x1FC0);
		voice.KeyOn();
	}
	const int mixes = 2000;
	auto t0 = clk::now();
	for (int i = 0; i < mixes; i++) sas->Mix(kFakeBase + 0x10000, 0, 0, 0, false);
	const double sasUs = std::chrono::duration<double, std::micro>(clk::now() - t0).count() / mixes;
	printf("  sceSas: 32 VAG voices, grain 256: %.1f us per mix (%.1f us per 60Hz frame)\n", sasUs, sasUs * 44100.0 / 256 / 60);
	delete sas;

	std::vector<u8> file = LoadRef("atrac/sample.at3");
	if (!file.empty()) {
		audio::Track track;
		std::string error;
		audio::AnalyzeAtracTrack(file.data(), (u32)file.size(), (u32)file.size(), &track, &error);
		audio::AudioDecoder *dec = audio::CreateAtrac3PlusAudio(track.channels, track.bytesPerFrame);
		std::vector<s16> pcm(2048 * 2);
		const int frames = (int)((file.size() - track.dataByteOffset) / track.bytesPerFrame);
		auto t1 = clk::now();
		for (int f = 0; f < frames; f++) {
			int consumed = 0, samples = 0;
			dec->Decode(file.data() + track.dataByteOffset + (size_t)f * track.bytesPerFrame, track.bytesPerFrame, &consumed, 2, pcm.data(), &samples);
		}
		const double us = std::chrono::duration<double, std::micro>(clk::now() - t1).count() / frames;
		printf("  ATRAC3+ stereo %d-byte frames: %.1f us per 2048-sample frame (%.2f%% of real time)\n", track.bytesPerFrame, us, us / (2048 * 1e6 / 44100) * 100);
		delete dec;
	}
	file = LoadRef("mp3/sample.mp3");
	if (!file.empty()) {
		audio::AudioDecoder *dec = audio::CreateMp3Audio();
		std::vector<s16> pcm(1152 * 2);
		size_t pos = 0;
		int frames = 0;
		auto t2 = clk::now();
		while (pos + 4 < file.size()) {
			int consumed = 0, samples = 0;
			if (!dec->Decode(file.data() + pos, (int)(file.size() - pos), &consumed, 2, pcm.data(), &samples) || consumed <= 0) break;
			pos += consumed;
			frames++;
		}
		const double us = std::chrono::duration<double, std::micro>(clk::now() - t2).count() / std::max(frames, 1);
		printf("  MP3 (Media Foundation): %d frames, %.1f us per 1152-sample frame\n", frames, us);
		delete dec;
	}
}
