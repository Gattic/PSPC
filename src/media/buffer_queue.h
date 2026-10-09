// SPDX-License-Identifier: GPL-2.0-or-later
//
// A byte FIFO over a fixed ring, with presentation timestamps attached to positions in it.
// Ported from PPSSPP's Core/HW/BufferQueue.h (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2013- PPSSPP Project, GPL-2.0-or-later).
#pragma once

#include <cstring>
#include <map>

#include "common/common.h"

namespace media {

class BufferQueue {
public:
	explicit BufferQueue(int size = 0x20000) { alloc(size); }
	~BufferQueue() { delete[] bufQueue_; }
	BufferQueue(const BufferQueue &) = delete;
	BufferQueue &operator=(const BufferQueue &) = delete;

	void alloc(int size) {
		if (size <= 0) size = 1;
		delete[] bufQueue_;
		bufQueue_ = new u8[size];
		bufQueueSize_ = size;
		clear();
	}

	void clear() {
		start_ = 0;
		end_ = 0;
		filled_ = 0;
		ptsMarks_.clear();
	}

	int getQueueSize() const { return filled_; }
	int getRemainSize() const { return bufQueueSize_ - filled_; }
	int capacity() const { return bufQueueSize_; }

	bool push(const u8 *buf, int addsize, s64 pts = 0) {
		const int space = getRemainSize();
		if (space < addsize || addsize < 0) return false;
		if (pts != 0) ptsMarks_[end_] = pts;
		if (end_ + addsize <= bufQueueSize_) {
			memcpy(bufQueue_ + end_, buf, addsize);
			end_ += addsize;
			if (end_ == bufQueueSize_) end_ = 0;
		} else {
			const int firstSize = bufQueueSize_ - end_;
			memcpy(bufQueue_ + end_, buf, firstSize);
			memcpy(bufQueue_, buf + firstSize, addsize - firstSize);
			end_ = addsize - firstSize;
		}
		filled_ += addsize;
		return true;
	}

	// buf may be null to just drop the bytes.
	int pop_front(u8 *buf, int wantedsize, s64 *pts = nullptr) {
		if (wantedsize <= 0) return 0;
		int bytesgot = getQueueSize();
		if (wantedsize < bytesgot) bytesgot = wantedsize;
		if (pts) *pts = findPts(bytesgot);
		else findPts(bytesgot);
		const int firstSize = bufQueueSize_ - start_;
		if (buf) {
			if (bytesgot <= firstSize) {
				memcpy(buf, bufQueue_ + start_, bytesgot);
			} else {
				memcpy(buf, bufQueue_ + start_, firstSize);
				memcpy(buf + firstSize, bufQueue_, bytesgot - firstSize);
			}
		}
		if (bytesgot <= firstSize)
			start_ += bytesgot;
		else
			start_ = bytesgot - firstSize;
		if (start_ == bufQueueSize_) start_ = 0;
		filled_ -= bytesgot;
		return bytesgot;
	}

	int get_front(u8 *buf, int wantedsize) const {
		if (wantedsize <= 0) return 0;
		int bytesgot = getQueueSize();
		if (wantedsize < bytesgot) bytesgot = wantedsize;
		const int firstSize = bufQueueSize_ - start_;
		if (bytesgot <= firstSize) {
			memcpy(buf, bufQueue_ + start_, bytesgot);
		} else {
			memcpy(buf, bufQueue_ + start_, firstSize);
			memcpy(buf + firstSize, bufQueue_, bytesgot - firstSize);
		}
		return bytesgot;
	}

private:
	s64 findPts(std::map<int, s64>::iterator earliest, std::map<int, s64>::iterator latest) {
		s64 pts = 0;
		// The first mark in the range is the pts of this packet.
		if (earliest != latest) pts = earliest->second;
		ptsMarks_.erase(earliest, latest);
		return pts;
	}

	s64 findPts(int packetSize) {
		auto earliest = ptsMarks_.lower_bound(start_);
		auto latest = ptsMarks_.lower_bound(start_ + packetSize);
		s64 pts = findPts(earliest, latest);
		// Wrapping around: look at the other half too.
		if (start_ + packetSize > bufQueueSize_) {
			earliest = ptsMarks_.begin();
			latest = ptsMarks_.lower_bound(start_ + packetSize - bufQueueSize_);
			s64 latePts = findPts(earliest, latest);
			if (pts == 0) pts = latePts;
		}
		return pts;
	}

	u8 *bufQueue_ = nullptr;
	// end_ may be less than start_: the space between end_ and start_ is free.
	int start_ = 0, end_ = 0;
	int filled_ = 0;
	int bufQueueSize_ = 0;
	std::map<int, s64> ptsMarks_;
};

}  // namespace media
