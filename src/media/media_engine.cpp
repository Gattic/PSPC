// SPDX-License-Identifier: GPL-2.0-or-later
//
// Ported from PPSSPP's Core/HW/MediaEngine.cpp (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later). See media_engine.h.
#include "media/media_engine.h"

#include <algorithm>
#include <cstring>

#include "audio/decoder.h"
#include "common/log.h"
#include "media/guest_memory.h"
#include "media/ycbcr.h"

namespace media {

namespace {

constexpr int kVideoFrameDuration = 3003;  // 29.97 fps in 90kHz units
constexpr int kAudioFrameDuration = 4180;  // 2048 samples at 44.1kHz

// -1 if the size is unreasonable. Swizzling writes whole 8-row tiles, at least one.
int VideoImageSize(int videoLineSize, int height, bool swizzle) {
	if (videoLineSize < 0 || height < 0) return -1;
	if (swizzle) height = std::max(8, (height + 7) & ~7);
	const s64 size = (s64)videoLineSize * height;
	return size > 0x10000000 ? -1 : (int)size;
}

int VideoLineSize(int frameWidth, int videoPixelMode) {
	switch (videoPixelMode) {
	case PIXEL_8888: return frameWidth * 4;
	case PIXEL_5650:
	case PIXEL_5551:
	case PIXEL_4444: return frameWidth * 2;
	default: return 0;
	}
}

}  // namespace

MediaEngine::MediaEngine() : m_mpegheader(0x10000) { m_mpegheaderSize = (int)m_mpegheader.size(); }

MediaEngine::~MediaEngine() { closeMedia(); }

void MediaEngine::closeMedia() {
	closeContext();
	m_pdata.reset();
	m_demux.reset();
	delete m_audioContext;
	m_audioContext = nullptr;
	m_isVideoEnd = false;
}

bool MediaEngine::SetupStreams() {
	const u32 magic = (u32)m_mpegheader[0] | ((u32)m_mpegheader[1] << 8) | ((u32)m_mpegheader[2] << 16) |
	                  ((u32)m_mpegheader[3] << 24);
	m_videoStreamIds.clear();
	if (magic != PSMF_MAGIC) {
		LOGW("MEDIA", "Could not set up streams, bad magic: %08x", magic);
		return false;
	}
	const int numStreams = ReadBE16(&m_mpegheader[0x80]);
	if (numStreams <= 0 || numStreams > 8) {
		LOGW("MEDIA", "Could not set up streams, unexpected stream count: %d", numStreams);
		return false;
	}
	for (int i = 0; i < numStreams; i++) {
		const int streamId = m_mpegheader[0x82 + i * 16];
		// Only video streams; the audio is demuxed separately.
		if ((streamId & PSMF_VIDEO_STREAM_ID) == PSMF_VIDEO_STREAM_ID) m_videoStreamIds.push_back(streamId);
	}
	// Add streams to meet the expectation.
	for (int i = (int)m_videoStreamIds.size(); i < m_expectedVideoStreams; i++)
		m_videoStreamIds.push_back(PSMF_VIDEO_STREAM_ID | i);
	return true;
}

int MediaEngine::VideoStreamIdFor(int streamNum) const {
	if (streamNum >= 0 && streamNum < (int)m_videoStreamIds.size()) return m_videoStreamIds[streamNum];
	return PSMF_VIDEO_STREAM_ID | (streamNum & 0xF);
}

bool MediaEngine::openContext(bool keepReadPos) {
	(void)keepReadPos;
	if (m_contextOpen || !m_pdata) return false;
	m_decodingsize = 0;
	m_bufSize = std::max(m_bufSize, m_mpegheaderSize);

	if (!SetupStreams()) {
		// Without a usable header, assume the plain layout: video stream n is 0xE0 | n.
		for (int i = 0; i < std::max(1, m_expectedVideoStreams); i++) m_videoStreamIds.push_back(PSMF_VIDEO_STREAM_ID | i);
	}
	if (m_videoStream >= (int)m_videoStreamIds.size()) {
		LOGW("MEDIA", "Bad video stream %d", m_videoStream);
		m_videoStream = -1;
	}
	if (m_videoStream == -1) m_videoStream = 0;

	m_video.Reset();
	m_video.SetReader([this](u8 *dst, int size) {
		if (!m_pdata) return 0;
		const int n = m_pdata->pop_front(dst, std::min(size, m_bufSize));
		if (n > 0) m_decodingsize = n;
		return n;
	});
	m_video.SetStream(VideoStreamIdFor(m_videoStream) & 0xF);
	m_decoderFailed = !m_decoder.Open();
	m_drained = false;
	m_lastAuPts = -1;
	m_contextOpen = true;
	if (!setVideoStream(m_videoStream, true)) return false;

	delete m_audioContext;
	m_audioContext = audio::CreateAudioDecoder(audio::PSP_CODEC_AT3PLUS);
	m_isVideoEnd = false;
	return true;
}

void MediaEngine::closeContext() {
	m_decoder.Close();
	m_video.Reset();
	m_video.SetReader(nullptr);
	m_contextOpen = false;
	m_frame = YuvFrame();
}

bool MediaEngine::loadStream(const u8 *buffer, int readSize, int ringbufferSize) {
	closeMedia();

	m_videopts = 0;
	m_lastPts = -1;
	m_audiopts = 0;
	m_ringbuffersize = ringbufferSize;
	m_pdata = std::make_unique<BufferQueue>(ringbufferSize + 2048);
	m_pdata->push(buffer, readSize);
	m_bytesPushed = readSize;
	m_lastFrameDecoded = false;
	m_seenVideo = 0;
	m_seenAudio = 0;
	const int headerSize = (int)ReadBE32(buffer + PSMF_STREAM_OFFSET_OFFSET);
	if (headerSize >= 0 && headerSize < readSize) NoteStreams(buffer + headerSize, readSize - headerSize);
	m_firstTimeStamp = GetMpegTimeStamp(buffer + PSMF_FIRST_TIMESTAMP_OFFSET);
	m_lastTimeStamp = GetMpegTimeStamp(buffer + PSMF_LAST_TIMESTAMP_OFFSET);
	m_streamSize = (s64)ReadBE32(buffer + PSMF_STREAM_SIZE_OFFSET);
	const int mpegoffset = (int)ReadBE32(buffer + PSMF_STREAM_OFFSET_OFFSET);
	m_demux = std::make_unique<MpegDemux>(ringbufferSize + 2048, mpegoffset);
	m_demux->addStreamData(buffer, readSize);
	return true;
}

bool MediaEngine::reloadStream() { return loadStream(m_mpegheader.data(), 2048, m_ringbuffersize); }

bool MediaEngine::addVideoStream(int streamNum, int streamId) {
	if (m_contextOpen) {
		// No need to add an existing stream.
		if (streamNum < (int)m_videoStreamIds.size()) return true;
		if (streamId == -1) streamId = PSMF_VIDEO_STREAM_ID | streamNum;
		while ((int)m_videoStreamIds.size() < streamNum) m_videoStreamIds.push_back(PSMF_VIDEO_STREAM_ID | (int)m_videoStreamIds.size());
		m_videoStreamIds.push_back(streamId);
		if (streamNum >= m_expectedVideoStreams) ++m_expectedVideoStreams;
		return true;
	}
	if (streamNum >= m_expectedVideoStreams) ++m_expectedVideoStreams;
	return false;
}

int MediaEngine::addStreamData(const u8 *buffer, int addSize) {
	int size = addSize;
	if (size > 0 && m_pdata) {
		if (!m_pdata->push(buffer, size)) {
			size = 0;
		} else {
			m_bytesPushed += size;
			m_lastFrameDecoded = false;
			NoteStreams(buffer, size);
		}
		if (m_demux) m_demux->addStreamData(buffer, addSize);
		if (!m_contextOpen && m_pdata->getQueueSize() >= 2048) {
			m_mpegheaderSize = m_pdata->get_front(m_mpegheader.data(), (int)m_mpegheader.size());
			const int streamOffset = (int)ReadBE32(&m_mpegheader[PSMF_STREAM_OFFSET_OFFSET]);
			if (streamOffset <= m_mpegheaderSize) {
				m_mpegheaderSize = streamOffset;
				m_pdata->pop_front(nullptr, m_mpegheaderSize);
				openContext();
			}
		}
		// We added data, so... not the end anymore.
		m_isVideoEnd = false;
	}
	return size;
}

void MediaEngine::NoteStreams(const u8 *data, int size) {
	// Packets start right after a pack header, so a plain scan for their start codes is enough.
	for (int i = 0; i + 4 <= size; i++) {
		if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) continue;
		const u8 code = data[i + 3];
		if ((code & 0xF0) == 0xE0) {
			m_seenVideo |= 1u << (code & 0xF);
		} else if (code == 0xBD && i + 9 <= size) {
			// MPEG-2 PES header, then the substream (channel) byte.
			const int at = i + 9 + data[i + 8];
			if ((data[i + 6] & 0xC0) == 0x80 && at < size) m_seenAudio |= 1u << (data[at] & 0xF);
		}
	}
}

bool MediaEngine::HasVideoStreamData(int streamNum) const {
	if (streamNum < 0) return false;
	return (m_seenVideo & (1u << (VideoStreamIdFor(streamNum) & 0xF))) != 0;
}

bool MediaEngine::StreamComplete() const {
	if (m_streamSize <= 0) return true;
	return m_bytesPushed - m_mpegheaderSize >= m_streamSize;
}

bool MediaEngine::seekTo(s64 timestamp, int videoPixelMode) {
	if (timestamp <= 0) return true;

	// Just doing it the not so great way, to be sure audio is in sync.
	int timeout = 1000;
	while (getVideoTimeStamp() < timestamp - kVideoFrameDuration) {
		if (getAudioTimeStamp() < getVideoTimeStamp() - kAudioFrameDuration * 2) getNextAudioFrame(nullptr, nullptr, nullptr);
		if (!stepVideo(videoPixelMode, true)) return false;
		if (--timeout <= 0) return true;
	}
	while (getAudioTimeStamp() < getVideoTimeStamp() - kAudioFrameDuration * 2) {
		if (getNextAudioFrame(nullptr, nullptr, nullptr) == 0) return false;
		if (--timeout <= 0) return true;
	}
	return true;
}

bool MediaEngine::setVideoStream(int streamNum, bool force) {
	if (m_videoStream == streamNum && !force) return true;
	if (m_contextOpen) {
		if (streamNum < 0 || streamNum >= (int)m_videoStreamIds.size()) return false;
		const int id = VideoStreamIdFor(streamNum) & 0xF;
		if (id != m_video.Stream()) {
			m_video.SetStream(id);
			m_decoder.Flush();
			m_decoderFailed = !m_decoder.Open();
			m_drained = false;
			m_lastAuPts = -1;
		}
	}
	m_videoStream = streamNum;
	return true;
}

bool MediaEngine::setAudioStream(int streamNum) {
	if (m_audioStream == streamNum) return true;
	m_audioStream = streamNum;
	if (m_demux) m_demux->setAudioChannel(streamNum);
	return true;
}

int MediaEngine::getRemainSize() {
	if (!m_pdata) return 0;
	// Until the context opens, the queue holds the PSMF header rather than ringbuffer data: the
	// ringbuffer is still all free (video/mpeg/basic starts at 512 of 512).
	if (!m_contextOpen) return std::max(m_pdata->getRemainSize() + std::min(m_pdata->getQueueSize(), 2048) - 2048, 0);
	return std::max(m_pdata->getRemainSize() - m_decodingsize - 2048, 0);
}

int MediaEngine::getAudioRemainSize() {
	// No audio, so it can't be full: return the video's instead.
	if (!m_demux) return getRemainSize();
	return m_demux->getRemainSize();
}

bool MediaEngine::stepVideo(int videoPixelMode, bool skipFrame) {
	(void)videoPixelMode;
	if (!m_contextOpen) return false;

	YuvFrame frame;
	bool got = false;
	// Normally one access unit gives one picture; a decoder that holds pictures back for
	// reordering takes a few more.
	for (int guard = 0; guard < 64 && !got; guard++) {
		if (m_decoder.PopFrame(&frame)) {
			got = true;
			break;
		}
		AccessUnit au;
		const bool endOfStream = m_pdata && m_pdata->getQueueSize() == 0 && StreamComplete();
		if (!m_video.NextAccessUnit(&au, endOfStream)) {
			// Even with all frames read, some may have been held back for reordering.
			if (endOfStream && !m_drained) {
				m_decoder.Drain();
				m_drained = true;
				if (m_decoder.PopFrame(&frame)) {
					got = true;
					break;
				}
			}
			// The end, unless the game adds more data.
			m_isVideoEnd = !got && (!m_pdata || m_pdata->getQueueSize() == 0);
			if (m_isVideoEnd) m_decodingsize = 0;
			break;
		}
		s64 pts = au.pts;
		if (pts < 0) pts = m_lastAuPts >= 0 ? m_lastAuPts + kVideoFrameDuration : m_firstTimeStamp;
		m_lastAuPts = pts;
		m_drained = false;
		if (m_decoderFailed) {
			// No decoder on this machine: keep time moving (as PPSSPP does without FFmpeg), so
			// games still reach the end of their movies.
			frame = YuvFrame();
			frame.pts = pts;
			got = true;
			break;
		}
		m_decoder.Decode(au.data.data(), (int)au.data.size(), pts);
	}
	if (!got) return false;

	if (frame.pts >= 0) {
		m_videopts = frame.pts + kVideoFrameDuration - m_firstTimeStamp;
	} else {
		m_videopts += kVideoFrameDuration;
	}
	m_lastPts = m_videopts;
	m_lastFrameDecoded = m_pdata && m_pdata->getQueueSize() == 0 && StreamComplete() && m_video.Empty() &&
	                     m_decoder.PendingFrames() == 0;
	if (frame.Valid() && (m_desWidth == 0 || m_desHeight == 0 || !skipFrame)) {
		m_desWidth = frame.width;
		m_desHeight = frame.height;
	}
	if (!skipFrame && frame.Valid()) m_frame = std::move(frame);
	return true;
}

int MediaEngine::writeVideoImage(u32 bufferPtr, int frameWidth, int videoPixelMode) {
	const int videoLineSize = VideoLineSize(frameWidth, videoPixelMode);
	const int height = m_desHeight;
	const bool swizzle = IsVramAddress(bufferPtr) && (bufferPtr & 0x00200000) == 0x00200000;
	const int videoImageSize = VideoImageSize(videoLineSize, height, swizzle);
	if (frameWidth <= 0 || frameWidth > 2048 || videoImageSize < 0 || !GuestValid(bufferPtr, (u32)videoImageSize)) {
		LOGW("MEDIA", "Ignoring invalid video decode address %08x/%x", bufferPtr, frameWidth);
		return 0;
	}
	if (!m_frame.Valid() || videoLineSize == 0) return 0;

	u8 *buffer = GuestPtr(bufferPtr, (u32)videoImageSize);
	// Rows can't be wider than the stride, or they'd overlap.
	const int width = std::min({m_desWidth, frameWidth, m_frame.width});
	const int rows = std::min(height, m_frame.height);
	std::vector<u8> tmp;
	u8 *imgbuf = buffer;
	if (swizzle) {
		// Zeroed: the padding past width (and the rows of the last tile) gets copied too.
		tmp.assign(videoImageSize, 0);
		imgbuf = tmp.data();
	}
	YCbCr420ToPixels(imgbuf, frameWidth, videoPixelMode, m_frame.y.data(), m_frame.cb.data(), m_frame.cr.data(),
	                 m_frame.width, 0, 0, width, rows);
	if (swizzle) {
		const int bxc = videoLineSize / 16;
		int byc = (height + 7) / 8;
		if (byc == 0) byc = 1;
		SwizzleTex16((const u32 *)imgbuf, buffer, bxc, byc, (u32)videoLineSize);
	}
	return videoImageSize;
}

int MediaEngine::writeVideoImageWithRange(u32 bufferPtr, int frameWidth, int videoPixelMode, int xpos, int ypos,
                                          int width, int height) {
	const int videoLineSize = VideoLineSize(frameWidth, videoPixelMode);
	const bool swizzle = IsVramAddress(bufferPtr) && (bufferPtr & 0x00200000) == 0x00200000;
	const int videoImageSize = VideoImageSize(videoLineSize, height, swizzle);
	if (frameWidth <= 0 || frameWidth > 2048 || videoImageSize < 0 || !GuestValid(bufferPtr, (u32)videoImageSize)) {
		LOGW("MEDIA", "Ignoring invalid video decode address %08x/%x", bufferPtr, frameWidth);
		return 0;
	}
	if (!m_frame.Valid() || videoLineSize == 0) return 0;

	u8 *buffer = GuestPtr(bufferPtr, (u32)videoImageSize);
	std::vector<u8> tmp;
	u8 *imgbuf = buffer;
	if (swizzle) {
		tmp.assign(videoImageSize, 0);
		imgbuf = tmp.data();
	}
	if (width > m_desWidth - xpos) width = m_desWidth - xpos;
	if (width > frameWidth) width = frameWidth;
	if (height > m_desHeight - ypos) height = m_desHeight - ypos;
	width = std::min(width, m_frame.width - xpos);
	height = std::min(height, m_frame.height - ypos);
	if (width > 0 && height > 0 && xpos >= 0 && ypos >= 0) {
		YCbCr420ToPixels(imgbuf, frameWidth, videoPixelMode, m_frame.y.data(), m_frame.cb.data(), m_frame.cr.data(),
		                 m_frame.width, xpos, ypos, width, height);
	}
	if (swizzle) {
		const int bxc = videoLineSize / 16;
		int byc = (height + 7) / 8;
		if (byc <= 0) byc = 1;
		SwizzleTex16((const u32 *)imgbuf, buffer, bxc, byc, (u32)videoLineSize);
	}
	return videoImageSize;
}

int MediaEngine::getNextAudioFrame(u8 **buf, int *headerCode1, int *headerCode2) {
	// Getting a frame advances the pts.
	m_audiopts += kAudioFrameDuration;
	if (!m_demux) return 0;
	// Demux now (rather than on add data) so that we select the right stream.
	m_demux->demux(m_audioStream);
	s64 pts = 0;
	const int result = m_demux->getNextAudioFrame(buf, headerCode1, headerCode2, &pts);
	// m_audiopts is supposed to be after the returned frame.
	if (pts != 0) m_audiopts = pts - m_firstTimeStamp + kAudioFrameDuration;
	return result;
}

int MediaEngine::getAudioSamples(u32 bufferPtr) {
	s16 *buffer = (s16 *)GuestPtr(bufferPtr, 8192);
	if (!buffer) LOGW("MEDIA", "Ignoring bad audio decode address %08x during video playback", bufferPtr);
	if (!m_demux) return 0;

	u8 *audioFrame = nullptr;
	int headerCode1 = 0, headerCode2 = 0;
	const int frameSize = getNextAudioFrame(&audioFrame, &headerCode1, &headerCode2);
	if (frameSize == 0) return 0;
	if (m_audioContext) {
		// 0x24 means mono: tell the decoder before the first frame. It still outputs stereo.
		if (headerCode1 == 0x24) m_audioContext->SetChannels(1);
		int inbytesConsumed = 0;
		int outSamples = 0;
		if (!m_audioContext->Decode(audioFrame, frameSize, &inbytesConsumed, 2, buffer, &outSamples))
			LOGW("MEDIA", "Audio decode failed during video playback");
	}
	return 0x2000;
}

bool MediaEngine::IsNoAudioData() {
	if (!m_demux) return true;
	// Here should be a safe enough place to demux.
	m_demux->demux(m_audioStream);
	return !m_demux->hasNextAudioFrame(nullptr, nullptr, nullptr, nullptr);
}

s64 MediaEngine::getLastTimeStamp() const {
	if (!m_pdata) return 0;
	return m_lastTimeStamp - m_firstTimeStamp;
}

}  // namespace media
