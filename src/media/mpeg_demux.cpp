// SPDX-License-Identifier: GPL-2.0-or-later
//
// MpegDemux is ported from PPSSPP's Core/HW/MpegDemux.cpp (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later). VideoDemux is new.
#include "media/mpeg_demux.h"

#include <algorithm>
#include <cstring>

#include "common/log.h"

namespace media {

namespace {

constexpr u32 PACKET_START_CODE_MASK = 0xffffff00;
constexpr u32 PACKET_START_CODE_PREFIX = 0x00000100;

// http://dvd.sourceforge.net/dvdinfo/mpeghdrs.html
constexpr u32 USER_DATA_START_CODE = 0x000001b2;
constexpr u32 ISO_11172_END_CODE = 0x000001b9;
constexpr u32 PACK_START_CODE = 0x000001ba;
constexpr u32 SYSTEM_HEADER_START_CODE = 0x000001bb;
constexpr u32 PROGRAM_STREAM_MAP = 0x000001bc;
constexpr u32 PRIVATE_STREAM_1 = 0x000001bd;
constexpr u32 PADDING_STREAM = 0x000001be;
constexpr u32 PRIVATE_STREAM_2 = 0x000001bf;

bool IsHeader(const u8 *audioStream, int offset) {
	return audioStream[offset] == 0x0F && audioStream[offset + 1] == 0xD0;
}

int GetNextHeaderPosition(const u8 *audioStream, int curpos, int limit, int frameSize) {
	const int endScan = limit - 1;
	// Most common case: the header can be found at each frameSize.
	const int offset = curpos + frameSize - 8;
	if (offset < endScan && IsHeader(audioStream, offset)) return offset;
	for (int scan = curpos; scan < endScan; scan++) {
		if (IsHeader(audioStream, scan)) return scan;
	}
	return -1;
}

}  // namespace

MpegDemux::MpegDemux(int size, int offset) : audioStream_(size) {
	buf_ = new u8[size];
	len_ = size;
	index_ = offset;
	audioChannel_ = -1;
	readSize_ = 0;
}

MpegDemux::~MpegDemux() { delete[] buf_; }

bool MpegDemux::addStreamData(const u8 *buf, int addSize) {
	if (readSize_ + addSize > len_) return false;
	memcpy(buf_ + readSize_, buf, addSize);
	readSize_ += addSize;
	return true;
}

int MpegDemux::readPesHeader(PesHeader &pesHeader, int length, int startCode) {
	int c = 0;
	while (length > 0) {
		c = read8();
		length--;
		if (c != 0xFF) break;
	}
	if ((c & 0xC0) == 0x40) {
		read8();
		c = read8();
		length -= 2;
	}
	pesHeader.pts = 0;
	pesHeader.dts = 0;
	if ((c & 0xE0) == 0x20) {
		pesHeader.dts = pesHeader.pts = readPts(c);
		length -= 4;
		if ((c & 0x10) != 0) {
			pesHeader.dts = readPts();
			length -= 5;
		}
	} else if ((c & 0xC0) == 0x80) {
		int flags = read8();
		int headerLength = read8();
		length -= 2;
		length -= headerLength;
		if ((flags & 0x80) != 0) {
			pesHeader.dts = pesHeader.pts = readPts();
			headerLength -= 5;
			if ((flags & 0x40) != 0) {
				pesHeader.dts = readPts();
				headerLength -= 5;
			}
		}
		if ((flags & 0x3F) != 0 && headerLength == 0) flags &= 0xC0;
		if ((flags & 0x01) != 0) {
			int pesExt = read8();
			headerLength--;
			int skipBytes = (pesExt >> 4) & 0x0B;
			skipBytes += skipBytes & 0x09;
			if ((pesExt & 0x40) != 0 || skipBytes > headerLength) pesExt = skipBytes = 0;
			skip(skipBytes);
			headerLength -= skipBytes;
			if ((pesExt & 0x01) != 0) {
				int ext2Length = read8();
				headerLength--;
				if ((ext2Length & 0x7F) != 0) {
					int idExt = read8();
					headerLength--;
					if ((idExt & 0x80) == 0) startCode = ((startCode & 0xFF) << 8) | idExt;
				}
			}
		}
		skip(headerLength);
	}
	if ((u32)startCode == PRIVATE_STREAM_1) {
		int channel = read8();
		pesHeader.channel = channel;
		length--;
		if (channel >= 0x80 && channel <= 0xCF) {
			// Standard PSP audio sub-streams have a 3 or 4 byte sub-header.
			skip(3);
			length -= 3;
			if (channel >= 0xB0 && channel <= 0xBF) {
				skip(1);
				length--;
			}
		} else {
			// PSP audio has an additional 3 bytes in the header.
			skip(3);
			length -= 3;
		}
	}
	return length;
}

int MpegDemux::demuxStream(bool bdemux, int startCode, int length, int channel) {
	if (bdemux) {
		PesHeader pesHeader(channel);
		length = readPesHeader(pesHeader, length, startCode);
		if (pesHeader.channel == channel || channel < 0) {
			channel = pesHeader.channel;
			if (length > 0 && index_ + length <= len_) audioStream_.push(buf_ + index_, length, pesHeader.pts);
		}
		skip(length);
	} else {
		skip(length);
	}
	return channel;
}

bool MpegDemux::skipPackHeader() {
	// MPEG version / SCR
	if ((read8() & 0xC4) != 0x44) return false;
	skip(1);
	if ((read8() & 0x04) != 0x04) return false;
	skip(1);
	if ((read8() & 0x04) != 0x04) return false;
	// SCR_ext
	if ((read8() & 0x01) != 0x01) return false;
	int muxrate = read24();
	if ((muxrate & 3) != 3) return false;
	int stuffing = read8() & 7;
	while (stuffing > 0) {
		if (read8() != 0xFF) return false;
		--stuffing;
	}
	return true;
}

void MpegDemux::setAudioChannel(int audioChannel) {
	if (audioChannel_ != audioChannel) {
		audioChannel_ = audioChannel;
		audioStream_.clear();
	}
}

bool MpegDemux::demux(int audioChannel) {
	setAudioChannel(audioChannel);

	bool looksValid = false;
	bool needMore = false;
	while (index_ < readSize_ && !needMore) {
		// Search for start code
		u32 startCode = 0xFF;
		while ((startCode & PACKET_START_CODE_MASK) != PACKET_START_CODE_PREFIX && !isEOF()) {
			startCode = (startCode << 8) | read8();
		}
		// Not enough data available yet.
		if (readSize_ - index_ < 16) {
			index_ = std::max(0, index_ - 4);
			break;
		}

		switch (startCode) {
		case PACK_START_CODE:
			if (skipPackHeader()) looksValid = true;
			break;
		case SYSTEM_HEADER_START_CODE:
		case PADDING_STREAM:
		case PRIVATE_STREAM_2: {
			looksValid = true;
			int length = read16();
			if (readSize_ - index_ < length) {
				index_ = std::max(0, index_ - (4 + 2));
				needMore = true;
				break;
			}
			skip(length);
			break;
		}
		case PRIVATE_STREAM_1: {
			// Audio stream
			int length = read16();
			if (readSize_ - index_ < length) {
				index_ = std::max(0, index_ - (4 + 2));
				needMore = true;
				break;
			}
			audioChannel_ = demuxStream(true, startCode, length, audioChannel_);
			looksValid = true;
			break;
		}
		case 0x1E0: case 0x1E1: case 0x1E2: case 0x1E3:
		case 0x1E4: case 0x1E5: case 0x1E6: case 0x1E7:
		case 0x1E8: case 0x1E9: case 0x1EA: case 0x1EB:
		case 0x1EC: case 0x1ED: case 0x1EE: case 0x1EF: {
			// Video stream
			int length = read16();
			// Check for PES header marker.
			looksValid = (buf_[std::min(index_, len_ - 1)] & 0xC0) == 0x80;
			if (readSize_ - index_ < length) {
				index_ = std::max(0, index_ - (4 + 2));
				needMore = true;
				break;
			}
			demuxStream(false, startCode, length, -1);
			break;
		}
		case USER_DATA_START_CODE:
			// User data, probably what sceMpegGetUserdataAu queries. Not demuxed.
			looksValid = true;
			break;
		}
	}
	if (index_ < readSize_) {
		int size = readSize_ - index_;
		memmove(buf_, buf_ + index_, size);
		index_ = 0;
		readSize_ = size;
	} else {
		index_ = 0;
		readSize_ = 0;
	}
	return looksValid;
}

int MpegDemux::getNextAudioFrame(u8 **buf, int *headerCode1, int *headerCode2, s64 *pts) {
	int gotsize;
	int frameSize;
	if (!hasNextAudioFrame(&gotsize, &frameSize, headerCode1, headerCode2)) return 0;
	int audioPos = 8;
	int nextHeader = GetNextHeaderPosition(audioFrame_, audioPos, gotsize, frameSize);
	if (nextHeader >= 0) {
		audioPos = nextHeader;
	} else {
		audioPos = gotsize;
	}
	audioStream_.pop_front(nullptr, audioPos, pts);
	if (buf) *buf = audioFrame_ + 8;
	return frameSize - 8;
}

bool MpegDemux::hasNextAudioFrame(int *gotsizeOut, int *frameSizeOut, int *headerCode1, int *headerCode2) {
	int gotsize = audioStream_.get_front(audioFrame_, 0x2000);
	if (gotsize < 4) return false;

	int offset = 0;
	if (!IsHeader(audioFrame_, 0)) {
		// Garbage before the first valid frame (chapter transitions): resync.
		offset = GetNextHeaderPosition(audioFrame_, 1, gotsize, 0);
		if (offset < 0 || gotsize - offset < 4) return false;
	}

	u8 code1 = audioFrame_[offset + 2];
	u8 code2 = audioFrame_[offset + 3];
	int frameSize = (((code1 & 0x03) << 8) | (code2 * 8)) + 0x10;
	if (frameSize > gotsize - offset) return false;

	if (gotsizeOut) *gotsizeOut = gotsize - offset;
	if (frameSizeOut) *frameSizeOut = frameSize;
	if (headerCode1) *headerCode1 = code1;
	if (headerCode2) *headerCode2 = code2;
	return true;
}

// ---- VideoDemux ----

void VideoDemux::Reset() {
	ps_.clear();
	psPos_ = 0;
	es_.clear();
	marks_.clear();
	esBase_ = 0;
	scanPos_ = 0;
	sawEndCode_ = false;
}

void VideoDemux::SetStream(int streamNum) {
	if (streamNum == streamNum_) return;
	streamNum_ = streamNum;
	es_.clear();
	marks_.clear();
	esBase_ = 0;
	scanPos_ = 0;
}

bool VideoDemux::Fill(int need) {
	while (ps_.size() - psPos_ < (size_t)need) {
		if (!read_) return false;
		if (psPos_ > 0 && psPos_ >= ps_.size() / 2) {
			ps_.erase(ps_.begin(), ps_.begin() + psPos_);
			psPos_ = 0;
		}
		const size_t old = ps_.size();
		ps_.resize(old + 0x10000);
		const int got = read_(ps_.data() + old, 0x10000);
		ps_.resize(old + std::max(got, 0));
		if (got <= 0) return false;
	}
	return true;
}

bool VideoDemux::ParseOnePacket() {
	// Find a start code.
	for (;;) {
		if (!Fill(4)) return false;
		const u8 *p = ps_.data() + psPos_;
		if (p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] >= 0xB9) break;
		psPos_++;
	}
	const u8 code = ps_[psPos_ + 3];
	if (code == 0xB9) {
		sawEndCode_ = true;
		psPos_ += 4;
		return true;
	}
	if (code == 0xBA) {
		if (!Fill(14)) return false;
		const u8 *p = ps_.data() + psPos_;
		if ((p[4] & 0xC0) == 0x40) {
			// MPEG-2: 14 bytes plus stuffing.
			const int stuffing = p[13] & 7;
			if (!Fill(14 + stuffing)) return false;
			psPos_ += 14 + stuffing;
		} else {
			// MPEG-1: 12 bytes.
			psPos_ += 12;
		}
		return true;
	}
	if (!Fill(6)) return false;
	const int length = ReadBE16(ps_.data() + psPos_ + 4);
	if (!Fill(6 + length)) return false;
	const u8 *pes = ps_.data() + psPos_;
	if (code == (u8)(0xE0 | streamNum_) && length > 0) {
		// Video PES header.
		int pos = 6;
		const int end = 6 + length;
		s64 pts = -1, dts = -1;
		auto readTs = [&](int at) -> s64 {
			const u8 *t = pes + at;
			return ((s64)((t[0] >> 1) & 7) << 30) | ((s64)t[1] << 22) | ((s64)(t[2] >> 1) << 15) | ((s64)t[3] << 7) |
			       (t[4] >> 1);
		};
		if ((pes[pos] & 0xC0) == 0x80) {
			// MPEG-2 PES header.
			if (pos + 3 > end) {
				pos = end;
			} else {
				const u8 flags = pes[pos + 1];
				const int hdrLen = pes[pos + 2];
				const int tsAt = pos + 3;
				if ((flags & 0x80) && tsAt + 5 <= end) {
					pts = dts = readTs(tsAt);
					if ((flags & 0x40) && tsAt + 10 <= end) dts = readTs(tsAt + 5);
				}
				pos += 3 + hdrLen;
			}
		} else {
			// MPEG-1 PES header.
			while (pos < end && pes[pos] == 0xFF) pos++;
			if (pos < end && (pes[pos] & 0xC0) == 0x40) pos += 2;
			if (pos < end && (pes[pos] & 0xE0) == 0x20) {
				if (pos + 5 <= end) pts = dts = readTs(pos);
				if ((pes[pos] & 0x10) && pos + 10 <= end) {
					dts = readTs(pos + 5);
					pos += 10;
				} else {
					pos += 5;
				}
			} else if (pos < end) {
				pos++;  // 0x0F: no timestamps
			}
		}
		if (pos < end) {
			const size_t start = esBase_ + es_.size();
			es_.insert(es_.end(), pes + pos, pes + end);
			marks_.push_back({start, esBase_ + es_.size(), pts, dts, false});
		}
	}
	psPos_ += 6 + length;
	return true;
}

int VideoDemux::FindAud(size_t from) const {
	for (size_t i = from; i + 3 < es_.size(); i++) {
		if (es_[i] == 0 && es_[i + 1] == 0 && es_[i + 2] == 1 && (es_[i + 3] & 0x1F) == 9) return (int)i;
	}
	return -1;
}

bool VideoDemux::NextAccessUnit(AccessUnit *au, bool endOfStream) {
	for (;;) {
		const int cur = FindAud(0);
		int next = -1;
		if (cur >= 0) {
			next = FindAud(std::max((size_t)cur + 4, scanPos_));
		}
		if (next < 0) {
			// Remember how far we looked, so the next attempt doesn't rescan.
			if (cur >= 0 && es_.size() > 3) scanPos_ = std::max((size_t)cur + 4, es_.size() - 3);
			if (ParseOnePacket()) continue;
			if (!endOfStream || es_.empty()) {
				// Something that never delimits its access units: don't grow without bound.
				if (es_.size() < 0x200000 || endOfStream) return false;
			}
			next = (int)es_.size();
		}

		const size_t audPos = esBase_ + (size_t)std::max(cur, 0) + 3;
		au->pts = -1;
		au->dts = -1;
		for (Mark &m : marks_) {
			if (audPos >= m.start && audPos < m.end) {
				if (!m.used && m.pts >= 0) {
					au->pts = m.pts;
					au->dts = m.dts;
					m.used = true;
				}
				break;
			}
		}
		au->data.assign(es_.begin(), es_.begin() + next);
		es_.erase(es_.begin(), es_.begin() + next);
		esBase_ += (size_t)next;
		scanPos_ = 0;
		while (!marks_.empty() && marks_.front().end <= esBase_) marks_.pop_front();
		return true;
	}
}

}  // namespace media
