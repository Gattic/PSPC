// SPDX-License-Identifier: GPL-2.0-or-later
// AacRawDataBlockBytes: the raw_data_block syntax of ISO 14496-3 (AAC-LC), read only as far as
// it takes to know where each element ends. The Huffman codebooks and band tables are FFmpeg's
// (third_party/aac_tables, LGPL-2.1-or-later).
#include "audio/aac_frame.h"

#include <array>
#include <unordered_map>

#include "audio/third_party/aac_tables/aac_tables.h"

namespace audio {

namespace {

enum Element { ID_SCE, ID_CPE, ID_CCE, ID_LFE, ID_DSE, ID_PCE, ID_FIL, ID_END };
constexpr int EIGHT_SHORT_SEQUENCE = 2;
constexpr int ZERO_HCB = 0, ESC_HCB = 11, NOISE_HCB = 13, INTENSITY_HCB2 = 14, INTENSITY_HCB = 15;
constexpr int kMaxSfb = 64;

struct Bits {
	const u8 *data;
	int size;  // bytes
	int pos = 0;  // bits
	bool bad = false;

	u32 Read(int n) {
		u32 v = 0;
		for (int i = 0; i < n; i++) v = (v << 1) | Bit();
		return v;
	}
	u32 Bit() {
		if (pos >= size * 8) {
			bad = true;
			return 0;
		}
		const u32 b = (data[pos >> 3] >> (7 - (pos & 7))) & 1;
		pos++;
		return b;
	}
	void Skip(int n) {
		pos += n;
		if (pos > size * 8) bad = true;
	}
	void Align() { pos = (pos + 7) & ~7; }
};

// A Huffman codebook as (length, code) -> index.
class Codebook {
public:
	Codebook(const u32 *codes32, const u16 *codes16, const u8 *bits, int n) {
		for (int i = 0; i < n; i++) {
			const u32 code = codes32 ? codes32[i] : codes16[i];
			map_[Key(bits[i], code)] = i;
			if (bits[i] > maxLen_) maxLen_ = bits[i];
		}
	}
	// The index, or -1.
	int Decode(Bits &b) const {
		u32 code = 0;
		for (int len = 1; len <= maxLen_; len++) {
			code = (code << 1) | b.Bit();
			if (b.bad) return -1;
			auto it = map_.find(Key(len, code));
			if (it != map_.end()) return it->second;
		}
		return -1;
	}

private:
	static u32 Key(int len, u32 code) { return ((u32)len << 24) | code; }
	std::unordered_map<u32, int> map_;
	int maxLen_ = 0;
};

const Codebook &ScalefactorBook() {
	static const Codebook book(aac_tables::kScalefactorCode, nullptr, aac_tables::kScalefactorBits, 121);
	return book;
}

const Codebook &SpectralBook(int cb) {
	static const std::array<Codebook, 11> books = [] {
		return std::array<Codebook, 11>{
			Codebook(nullptr, aac_tables::kSpectralCodes[0], aac_tables::kSpectralBits[0], aac_tables::kSpectralSizes[0]),
			Codebook(nullptr, aac_tables::kSpectralCodes[1], aac_tables::kSpectralBits[1], aac_tables::kSpectralSizes[1]),
			Codebook(nullptr, aac_tables::kSpectralCodes[2], aac_tables::kSpectralBits[2], aac_tables::kSpectralSizes[2]),
			Codebook(nullptr, aac_tables::kSpectralCodes[3], aac_tables::kSpectralBits[3], aac_tables::kSpectralSizes[3]),
			Codebook(nullptr, aac_tables::kSpectralCodes[4], aac_tables::kSpectralBits[4], aac_tables::kSpectralSizes[4]),
			Codebook(nullptr, aac_tables::kSpectralCodes[5], aac_tables::kSpectralBits[5], aac_tables::kSpectralSizes[5]),
			Codebook(nullptr, aac_tables::kSpectralCodes[6], aac_tables::kSpectralBits[6], aac_tables::kSpectralSizes[6]),
			Codebook(nullptr, aac_tables::kSpectralCodes[7], aac_tables::kSpectralBits[7], aac_tables::kSpectralSizes[7]),
			Codebook(nullptr, aac_tables::kSpectralCodes[8], aac_tables::kSpectralBits[8], aac_tables::kSpectralSizes[8]),
			Codebook(nullptr, aac_tables::kSpectralCodes[9], aac_tables::kSpectralBits[9], aac_tables::kSpectralSizes[9]),
			Codebook(nullptr, aac_tables::kSpectralCodes[10], aac_tables::kSpectralBits[10], aac_tables::kSpectralSizes[10]),
		};
	}();
	return books[cb - 1];
}

struct IcsInfo {
	int windowSequence = 0;
	int maxSfb = 0;
	int numWindowGroups = 1;
	int groupLen[8] = {1};
	int numSwb = 0;
	const u16 *swbOffset = nullptr;
	bool Short() const { return windowSequence == EIGHT_SHORT_SEQUENCE; }
};

bool ReadIcsInfo(Bits &b, int sfi, IcsInfo *info) {
	b.Skip(1);  // ics_reserved_bit
	info->windowSequence = (int)b.Read(2);
	b.Skip(1);  // window_shape
	if (info->Short()) {
		info->maxSfb = (int)b.Read(4);
		const u32 grouping = b.Read(7);
		info->numWindowGroups = 1;
		info->groupLen[0] = 1;
		for (int i = 0; i < 7; i++) {
			if (grouping & (0x40 >> i)) {
				info->groupLen[info->numWindowGroups - 1]++;
			} else {
				info->groupLen[info->numWindowGroups++] = 1;
			}
		}
		info->numSwb = aac_tables::kNumSwb128[sfi];
		info->swbOffset = aac_tables::kSwbOffset128[sfi];
	} else {
		info->maxSfb = (int)b.Read(6);
		// Prediction is AAC Main or LTP, not LC.
		if (b.Read(1)) return false;
		info->numWindowGroups = 1;
		info->groupLen[0] = 1;
		info->numSwb = aac_tables::kNumSwb1024[sfi];
		info->swbOffset = aac_tables::kSwbOffset1024[sfi];
	}
	return !b.bad && info->maxSfb <= info->numSwb;
}

bool ReadIcs(Bits &b, int sfi, bool commonWindow, IcsInfo *info) {
	b.Skip(8);  // global_gain
	if (!commonWindow && !ReadIcsInfo(b, sfi, info)) return false;

	// section_data
	int sfbCb[8][kMaxSfb] = {};
	struct Section {
		int cb, start, end;
	};
	std::array<std::array<Section, kMaxSfb>, 8> sections{};
	int numSections[8] = {};
	const int sectBits = info->Short() ? 3 : 5;
	const u32 sectEsc = (1u << sectBits) - 1;
	for (int g = 0; g < info->numWindowGroups; g++) {
		int k = 0;
		while (k < info->maxSfb) {
			const int cb = (int)b.Read(4);
			if (cb == 12) return false;  // reserved
			int len = 0;
			u32 incr;
			while ((incr = b.Read(sectBits)) == sectEsc) {
				len += (int)sectEsc;
				if (b.bad) return false;
			}
			len += (int)incr;
			if (len == 0 || k + len > info->maxSfb || numSections[g] >= kMaxSfb) return false;
			sections[g][numSections[g]++] = Section{cb, k, k + len};
			for (int sfb = k; sfb < k + len; sfb++) sfbCb[g][sfb] = cb;
			k += len;
		}
	}

	// scale_factor_data
	bool firstNoise = true;
	for (int g = 0; g < info->numWindowGroups; g++) {
		for (int sfb = 0; sfb < info->maxSfb; sfb++) {
			const int cb = sfbCb[g][sfb];
			if (cb == ZERO_HCB) continue;
			if (cb == NOISE_HCB && firstNoise) {
				firstNoise = false;
				b.Skip(9);
				continue;
			}
			if (ScalefactorBook().Decode(b) < 0) return false;
		}
	}

	// pulse_data
	if (b.Read(1)) {
		if (info->Short()) return false;
		const int numPulse = (int)b.Read(2);
		b.Skip(6);
		b.Skip((numPulse + 1) * (5 + 4));
	}
	// tns_data
	if (b.Read(1)) {
		const int windows = info->Short() ? 8 : 1;
		for (int w = 0; w < windows; w++) {
			const int nFilt = (int)b.Read(info->Short() ? 1 : 2);
			if (!nFilt) continue;
			const int coefRes = (int)b.Read(1);
			for (int f = 0; f < nFilt; f++) {
				b.Skip(info->Short() ? 4 : 6);  // length
				const int order = (int)b.Read(info->Short() ? 3 : 5);
				if (!order) continue;
				b.Skip(1);  // direction
				const int compress = (int)b.Read(1);
				b.Skip(order * (3 + coefRes - compress));
			}
		}
	}
	// gain_control_data: AAC SSR only.
	if (b.Read(1)) return false;

	// spectral_data
	for (int g = 0; g < info->numWindowGroups; g++) {
		for (int i = 0; i < numSections[g]; i++) {
			const Section &s = sections[g][i];
			if (s.cb == ZERO_HCB || s.cb == NOISE_HCB || s.cb == INTENSITY_HCB || s.cb == INTENSITY_HCB2) continue;
			const int coefs = (info->swbOffset[s.end] - info->swbOffset[s.start]) * info->groupLen[g];
			const bool quad = s.cb < 5;
			const bool isUnsigned = s.cb != 1 && s.cb != 2 && s.cb != 5 && s.cb != 6;
			const int base = s.cb <= 4 ? 3 : s.cb <= 6 ? 9 : s.cb <= 8 ? 8 : s.cb <= 10 ? 13 : 17;
			for (int k = 0; k < coefs; k += quad ? 4 : 2) {
				int idx = SpectralBook(s.cb).Decode(b);
				if (idx < 0) return false;
				int vals[4];
				const int n = quad ? 4 : 2;
				for (int j = n - 1; j >= 0; j--) {
					vals[j] = idx % base;
					idx /= base;
				}
				if (isUnsigned) {
					for (int j = 0; j < n; j++) {
						if (vals[j]) b.Skip(1);  // sign
					}
				}
				if (s.cb == ESC_HCB) {
					for (int j = 0; j < 2; j++) {
						if (vals[j] != 16) continue;
						int prefix = 0;
						while (b.Read(1)) {
							if (++prefix > 8 || b.bad) return false;
						}
						b.Skip(prefix + 4);
					}
				}
				if (b.bad) return false;
			}
		}
	}
	return !b.bad;
}

}  // namespace

int AacSamplingIndex(int rate) {
	static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
	for (int i = 0; i < 13; i++) {
		if (rates[i] == rate) return i;
	}
	return -1;
}

int AacRawDataBlockBytes(const u8 *data, int size, int samplingIndex) {
	if (!data || size <= 0 || samplingIndex < 0 || samplingIndex > 12) return -1;
	Bits b{data, size};
	for (;;) {
		const int id = (int)b.Read(3);
		if (b.bad) return -1;
		switch (id) {
		case ID_SCE:
		case ID_LFE: {
			b.Skip(4);
			IcsInfo info;
			if (!ReadIcs(b, samplingIndex, false, &info)) return -1;
			break;
		}
		case ID_CPE: {
			b.Skip(4);
			IcsInfo info;
			const bool common = b.Read(1) != 0;
			if (common) {
				if (!ReadIcsInfo(b, samplingIndex, &info)) return -1;
				const int msMask = (int)b.Read(2);
				if (msMask == 1) b.Skip(info.numWindowGroups * info.maxSfb);
			}
			IcsInfo second = info;
			if (!ReadIcs(b, samplingIndex, common, &info)) return -1;
			if (!ReadIcs(b, samplingIndex, common, &second)) return -1;
			break;
		}
		case ID_DSE: {
			b.Skip(4);
			const bool align = b.Read(1) != 0;
			int count = (int)b.Read(8);
			if (count == 255) count += (int)b.Read(8);
			if (align) b.Align();
			b.Skip(count * 8);
			break;
		}
		case ID_FIL: {
			int count = (int)b.Read(4);
			if (count == 15) count += (int)b.Read(8) - 1;
			b.Skip(count * 8);
			break;
		}
		case ID_END:
			b.Align();
			return b.bad || b.pos / 8 > size ? -1 : b.pos / 8;
		default:  // coupling channels and program configs aren't walked
			return -1;
		}
		if (b.bad) return -1;
	}
}

}  // namespace audio
