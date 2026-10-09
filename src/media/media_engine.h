// SPDX-License-Identifier: GPL-2.0-or-later
//
// The PSP Media Engine as sceMpeg and scePsmfPlayer use it: a program stream goes in as the game
// supplies it, decoded video pictures and ATRAC3+ audio frames come out.
//
// Ported from PPSSPP's Core/HW/MediaEngine.{h,cpp} (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later). PPSSPP demuxes the video through
// FFmpeg's libavformat and decodes it with libavcodec; here the video side is our own program
// stream demuxer (mpeg_demux.h) and the Windows H.264 decoder (h264_decoder.h). The accounting the
// games see (how much of the ringbuffer is still held, in what steps it frees up) follows what
// PPSSPP's FFmpeg setup does: the stream is read 8KB at a time, as its AVIO context does.
#pragma once

#include <memory>
#include <vector>

#include "common/common.h"
#include "media/buffer_queue.h"
#include "media/h264_decoder.h"
#include "media/mpeg_demux.h"

namespace audio {
class AudioDecoder;
}

namespace media {

class MediaEngine {
public:
	MediaEngine();
	~MediaEngine();
	MediaEngine(const MediaEngine &) = delete;
	MediaEngine &operator=(const MediaEngine &) = delete;

	void closeMedia();
	bool loadStream(const u8 *buffer, int readSize, int ringbufferSize);
	bool reloadStream();
	bool addVideoStream(int streamNum, int streamId = -1);
	bool openContext(bool keepReadPos = false);
	void closeContext();

	// Returns the number of bytes actually added (0 if the queue is full).
	int addStreamData(const u8 *buffer, int addSize);
	bool seekTo(s64 timestamp, int videoPixelMode);

	bool setVideoStream(int streamNum, bool force = false);
	bool setAudioStream(int streamNum);

	int getRemainSize();
	int getAudioRemainSize();

	bool stepVideo(int videoPixelMode, bool skipFrame = false);
	// Guest addresses; return the number of bytes written (0 if refused).
	int writeVideoImage(u32 bufferPtr, int frameWidth = 512, int videoPixelMode = 3);
	int writeVideoImageWithRange(u32 bufferPtr, int frameWidth, int videoPixelMode, int xpos, int ypos, int width,
	                             int height);
	int getAudioSamples(u32 bufferPtr);

	s64 getVideoTimeStamp() const { return m_videopts; }
	s64 getAudioTimeStamp() const { return m_demux ? m_audiopts - 4180 : -1; }
	s64 getLastTimeStamp() const;

	bool IsVideoEnd() const { return m_isVideoEnd; }
	// The picture stepVideo last returned was the last one: the whole stream has arrived and
	// nothing is left to decode.
	bool IsLastFrameDecoded() const { return m_lastFrameDecoded; }
	// Whether any packet of video stream streamNum / audio channel has arrived so far.
	bool HasVideoStreamData(int streamNum) const;
	bool HasAudioStreamData(int channel) const { return channel >= 0 && channel < 16 && (m_seenAudio & (1u << channel)) != 0; }
	bool IsNoAudioData();
	bool IsActuallyPlayingAudio() const { return getAudioTimeStamp() >= 0; }
	int VideoWidth() const { return m_desWidth; }
	int VideoHeight() const { return m_desHeight; }
	bool IsContextOpen() const { return m_contextOpen; }

	// The picture the last non-skipped stepVideo produced (planar 4:2:0), for the YCbCr APIs.
	const YuvFrame &CurrentFrame() const { return m_frame; }
	bool HasFrame() const { return m_frame.Valid(); }

	int m_desWidth = 0;
	int m_desHeight = 0;
	s64 m_videopts = 0;
	s64 m_firstTimeStamp = 0;
	s64 m_lastTimeStamp = 0;
	bool m_isVideoEnd = false;

private:
	bool SetupStreams();
	int getNextAudioFrame(u8 **buf, int *headerCode1, int *headerCode2);
	bool StreamComplete() const;
	void NoteStreams(const u8 *data, int size);
	int VideoStreamIdFor(int streamNum) const;

	int m_bufSize = 0x2000;
	int m_videoStream = -1;
	int m_expectedVideoStreams = 0;
	int m_audioStream = -1;
	int m_decodingsize = 0;
	std::unique_ptr<BufferQueue> m_pdata;
	s64 m_lastPts = -1;
	std::unique_ptr<MpegDemux> m_demux;
	audio::AudioDecoder *m_audioContext = nullptr;
	s64 m_audiopts = 0;
	int m_ringbuffersize = 0;
	std::vector<u8> m_mpegheader;
	int m_mpegheaderSize = 0;

	// The video side (what PPSSPP's AVFormatContext and AVCodecContext do).
	bool m_contextOpen = false;
	std::vector<int> m_videoStreamIds;  // from the PSMF header, in order
	VideoDemux m_video;
	H264Decoder m_decoder;
	bool m_decoderFailed = false;
	bool m_drained = false;
	s64 m_lastAuPts = -1;
	YuvFrame m_frame;
	// For telling the end of the stream from the ringbuffer running dry.
	s64 m_streamSize = 0;
	s64 m_bytesPushed = 0;
	bool m_lastFrameDecoded = false;
	u32 m_seenVideo = 0;  // bit n: a packet of stream 0xE0 | n went by
	u32 m_seenAudio = 0;  // bit n: a private stream 1 packet of channel n went by
};

}  // namespace media
