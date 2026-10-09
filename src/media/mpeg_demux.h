// SPDX-License-Identifier: GPL-2.0-or-later
//
// MPEG program stream (PSMF) demuxing.
//
// MpegDemux pulls the ATRAC3+ audio out of the private stream 1 packets. Ported from PPSSPP's
// Core/HW/MpegDemux.{h,cpp} (https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP
// Project, GPL-2.0-or-later), itself based on JPCSP.
//
// VideoDemux pulls one H.264 video stream (0xE0 | n) out of the same packets and splits it into
// access units at their access unit delimiters, the way the Media Engine hands them to the
// decoder (sceMpegGetAvcAu reports exactly these sizes on hardware).
#pragma once

#include <deque>
#include <functional>
#include <vector>

#include "common/common.h"
#include "media/buffer_queue.h"

namespace media {

constexpr u32 PSMF_MAGIC = 0x464D5350;
constexpr int PSMF_STREAM_VERSION_OFFSET = 0x4;
constexpr int PSMF_STREAM_OFFSET_OFFSET = 0x8;
constexpr int PSMF_STREAM_SIZE_OFFSET = 0xC;
constexpr int PSMF_FIRST_TIMESTAMP_OFFSET = 0x54;
constexpr int PSMF_LAST_TIMESTAMP_OFFSET = 0x5A;
constexpr int PSMF_VIDEO_STREAM_ID = 0xE0;
constexpr int PSMF_AUDIO_STREAM_ID = 0xBD;

inline s64 GetMpegTimeStamp(const u8 *buf) {
	return (s64)buf[5] | ((s64)buf[4] << 8) | ((s64)buf[3] << 16) | ((s64)buf[2] << 24) | ((s64)buf[1] << 32) |
	       ((s64)buf[0] << 36);
}

inline u32 ReadBE32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
inline u16 ReadBE16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }

class MpegDemux {
public:
	MpegDemux(int size, int offset);
	~MpegDemux();
	MpegDemux(const MpegDemux &) = delete;
	MpegDemux &operator=(const MpegDemux &) = delete;

	bool addStreamData(const u8 *buf, int addSize);
	// Returns whether the data looked like a valid program stream.
	bool demux(int audioChannel);
	void setAudioChannel(int audioChannel);

	// Returns the frame size (0 if no complete frame is buffered).
	int getNextAudioFrame(u8 **buf, int *headerCode1, int *headerCode2, s64 *pts = nullptr);
	bool hasNextAudioFrame(int *gotsizeOut, int *frameSizeOut, int *headerCode1, int *headerCode2);

	int getRemainSize() const { return len_ - readSize_; }

private:
	struct PesHeader {
		s64 pts = 0;
		s64 dts = 0;
		int channel;
		explicit PesHeader(int chan) : channel(chan) {}
	};

	int read8() {
		if (index_ >= len_) return 0;
		return buf_[index_++];
	}
	int read16() {
		int a = read8();
		return (a << 8) | read8();
	}
	int read24() {
		int a = read8();
		int b = read8();
		return (a << 16) | (b << 8) | read8();
	}
	s64 readPts() { return readPts(read8()); }
	s64 readPts(int c) {
		s64 hi = ((s64)(c & 0x0E)) << 29;
		s64 mid = (s64)(read16() >> 1) << 15;
		return hi | mid | (read16() >> 1);
	}
	bool isEOF() const { return index_ >= readSize_; }
	void skip(int n) {
		if (n > 0) {
			index_ += n;
			if (index_ > len_) index_ = len_;
		}
	}
	int readPesHeader(PesHeader &pesHeader, int length, int startCode);
	int demuxStream(bool bdemux, int startCode, int length, int channel);
	bool skipPackHeader();

	int index_;
	int len_;
	u8 *buf_;
	BufferQueue audioStream_;
	u8 audioFrame_[0x2000];
	int audioChannel_;
	int readSize_;
};

// One access unit of the video elementary stream.
struct AccessUnit {
	std::vector<u8> data;
	s64 pts = -1;  // -1 when no PES header gave this AU a timestamp
	s64 dts = -1;
};

class VideoDemux {
public:
	// read pulls up to `size` more bytes of program stream into dst and returns how many it
	// produced (0 when nothing is available right now).
	using ReadFn = std::function<int(u8 *dst, int size)>;

	void Reset();
	void SetReader(ReadFn fn) { read_ = std::move(fn); }
	// Which video stream to collect (0..15). Changing it drops what was collected.
	void SetStream(int streamNum);
	int Stream() const { return streamNum_; }

	// The next complete access unit. With endOfStream, the data that ran out ends the last one;
	// otherwise an AU is only complete once the next one's delimiter has been seen.
	bool NextAccessUnit(AccessUnit *au, bool endOfStream);
	// True once the program stream end code went by.
	bool SawEndCode() const { return sawEndCode_; }

private:
	// Parses one more PS packet out of ps_. Returns false if more input is needed.
	bool ParseOnePacket();
	bool Fill(int need);
	int FindAud(size_t from) const;

	ReadFn read_;
	int streamNum_ = 0;
	std::vector<u8> ps_;  // program stream not parsed yet
	size_t psPos_ = 0;
	std::vector<u8> es_;  // elementary stream not yet handed out
	struct Mark {
		size_t start, end;  // payload span in es_ coordinates
		s64 pts, dts;
		bool used;
	};
	std::deque<Mark> marks_;
	size_t esBase_ = 0;      // es_ coordinate of es_[0]
	size_t scanPos_ = 0;     // where to resume looking for the next delimiter (es_ index)
	bool sawEndCode_ = false;
};

}  // namespace media
