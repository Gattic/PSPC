// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP's Core/Util/AtracTrack.cpp (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#include "audio/atrac_track.h"

#include <algorithm>

#include "common/log.h"
#include "runtime/kernel/errors.h"

namespace audio {

namespace {

constexpr u32 RIFF_CHUNK_MAGIC = 0x46464952;
constexpr u32 RIFF_WAVE_MAGIC = 0x45564157;
constexpr u32 FMT_CHUNK_MAGIC = 0x20746D66;
constexpr u32 DATA_CHUNK_MAGIC = 0x61746164;
constexpr u32 SMPL_CHUNK_MAGIC = 0x6C706D73;
constexpr u32 FACT_CHUNK_MAGIC = 0x74636166;

constexpr u32 WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
constexpr u32 WAVE_FORMAT_AT3 = 0x270;

u16 Read16(const u8 *buffer, u32 offset) {
	u16 value;
	memcpy(&value, buffer + offset, sizeof(u16));
	return value;
}

u32 Read32(const u8 *buffer, u32 offset) {
	u32 value;
	memcpy(&value, buffer + offset, sizeof(u32));
	return value;
}

inline u32 Read16p(const u8 *base, int *offset) {
	u32 value = 0;
	memcpy(&value, base + *offset, 2);
	*offset += 2;
	return value;
}

inline u32 Read32p(const u8 *base, int *offset) {
	u32 value = 0;
	memcpy(&value, base + *offset, 4);
	*offset += 4;
	return value;
}

}  // namespace

int AnalyzeAtracTrack(const u8 *buffer, u32 size, u32 maxReadable, Track *track, std::string *error) {
	if (size < 72) return (int)SCE_ERROR_ATRAC_SIZE_TOO_SMALL;
	if (!buffer) return (int)SCE_KERNEL_ERROR_INVALID_POINTER;
	if (Read32(buffer, 0) != RIFF_CHUNK_MAGIC) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;

	struct RIFFFmtChunk {
		u16 fmtTag;
		u16 channels;
		u32 samplerate;
		u32 avgBytesPerSec;
		u16 blockAlign;
	};

	u32 offset = 8;
	track->firstSampleOffset = 0;

	while (Read32(buffer, offset) != RIFF_WAVE_MAGIC) {
		int chunk = (int)Read32(buffer, offset - 4);
		if (chunk < 0 || (u64)offset + (u64)chunk + (chunk & 1) + 12 > size) {
			*error = "too small for WAVE chunk";
			return (int)SCE_ERROR_ATRAC_SIZE_TOO_SMALL;
		}
		offset += chunk + (chunk & 1);
		if (Read32(buffer, offset) != RIFF_CHUNK_MAGIC) {
			*error = "RIFF chunk did not contain WAVE";
			return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
		}
		offset += 8;
	}
	offset += 4;

	track->fileSize = Read32(buffer, offset - 8) + 8;

	// The RIFF size may be too low and still work on hardware, but reads must stay mapped.
	u32 maxSize = std::max(track->fileSize, size);
	if (maxReadable != 0 && maxSize > maxReadable) maxSize = maxReadable;

	bool foundData = false;
	u32 dataChunkSize = 0;
	int sampleOffsetAdjust = 0;

	while (maxSize >= offset + 8 && !foundData) {
		u32 chunkMagic = Read32(buffer, offset);
		u32 chunkSize = Read32(buffer, offset + 4);
		chunkSize += (chunkSize & 1);
		offset += 8;
		if ((chunkSize > maxSize - offset) && chunkMagic != DATA_CHUNK_MAGIC) break;
		switch (chunkMagic) {
		case FMT_CHUNK_MAGIC: {
			if (track->codecType != 0) {
				*error = "multiple fmt chunks";
				return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			}
			auto at3fmt = (const RIFFFmtChunk *)(buffer + offset);
			if (chunkSize < 32 || (at3fmt->fmtTag == WAVE_FORMAT_EXTENSIBLE && chunkSize < 52)) {
				*error = "fmt definition too small";
				return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			}
			if (at3fmt->fmtTag == WAVE_FORMAT_AT3)
				track->codecType = PSP_CODEC_AT3;
			else if (at3fmt->fmtTag == WAVE_FORMAT_EXTENSIBLE)
				track->codecType = PSP_CODEC_AT3PLUS;
			else {
				*error = "invalid fmt magic";
				return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			}
			track->channels = at3fmt->channels;
			if (track->channels != 1 && track->channels != 2) {
				*error = "unsupported channel count";
				return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			}
			if (at3fmt->samplerate != 44100) {
				*error = "unsupported sample rate";
				return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			}
			track->bitrate = at3fmt->avgBytesPerSec * 8;
			track->bytesPerFrame = at3fmt->blockAlign;
			if (track->bytesPerFrame == 0) {
				*error = "invalid bytes per frame";
				return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			}
			if (at3fmt->fmtTag == WAVE_FORMAT_AT3) track->jointStereo = Read16(buffer, offset + 24);
			break;
		}
		case FACT_CHUNK_MAGIC: {
			track->endSample = (int)Read32(buffer, offset);
			if (chunkSize >= 8) track->firstSampleOffset = (int)Read32(buffer, offset + 4);
			if (chunkSize >= 12) {
				u32 largerOffset = Read32(buffer, offset + 8);
				sampleOffsetAdjust = track->firstSampleOffset - (int)largerOffset;
			}
			break;
		}
		case SMPL_CHUNK_MAGIC: {
			if (chunkSize < 32) {
				*error = "smpl chunk too small";
				return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			}
			int checkNumLoops = (int)Read32(buffer, offset + 28);
			if (checkNumLoops != 0 && chunkSize < 36 + 20) {
				*error = "smpl chunk too small for loop";
				return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			}
			u32 maxLoops = chunkSize >= 36 ? (chunkSize - 36) / 24 : 0;
			if (checkNumLoops < 0 || (u32)checkNumLoops > maxLoops) {
				*error = "bad checkNumLoops";
				return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			}
			track->loopinfo.resize(checkNumLoops);
			u32 loopinfoOffset = offset + 36;
			for (int i = 0; i < checkNumLoops && 36 + (u32)i < chunkSize; i++, loopinfoOffset += 24) {
				track->loopinfo[i].cuePointID = (int)Read32(buffer, loopinfoOffset + 0);
				track->loopinfo[i].type = (int)Read32(buffer, loopinfoOffset + 4);
				track->loopinfo[i].startSample = (int)Read32(buffer, loopinfoOffset + 8);
				track->loopinfo[i].endSample = (int)Read32(buffer, loopinfoOffset + 12);
				track->loopinfo[i].fraction = (int)Read32(buffer, loopinfoOffset + 16);
				track->loopinfo[i].playCount = (int)Read32(buffer, loopinfoOffset + 20);
				if (i == 0 && track->loopinfo[i].startSample >= track->loopinfo[i].endSample) {
					*error = "loop starts after it ends";
					return (int)SCE_ERROR_ATRAC_BAD_CODEC_PARAMS;
				}
			}
			break;
		}
		case DATA_CHUNK_MAGIC: {
			foundData = true;
			track->dataByteOffset = (int)offset;
			dataChunkSize = chunkSize;
			if (track->fileSize < offset + chunkSize) track->fileSize = offset + chunkSize;
			break;
		}
		}
		offset += chunkSize;
	}

	if (track->codecType == 0) {
		*error = "could not detect codec";
		return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
	}
	if (!foundData) {
		*error = "no data chunk found";
		return (int)SCE_ERROR_ATRAC_SIZE_TOO_SMALL;
	}

	if (!track->loopinfo.empty()) {
		track->loopStartSample = track->loopinfo[0].startSample + track->FirstOffsetExtra() + sampleOffsetAdjust;
		track->loopEndSample = track->loopinfo[0].endSample + track->FirstOffsetExtra() + sampleOffsetAdjust;
	} else {
		track->loopStartSample = -1;
		track->loopEndSample = -1;
	}

	if (track->endSample <= 0 && track->bytesPerFrame != 0) {
		track->endSample = (int)(dataChunkSize / track->bytesPerFrame) * track->SamplesPerFrame();
		track->endSample -= track->FirstSampleOffsetFull();
	}
	track->endSample -= 1;

	if (track->loopEndSample != -1 && track->loopEndSample > track->endSample + track->FirstSampleOffsetFull()) {
		*error = "loop after end of data";
		return (int)SCE_ERROR_ATRAC_BAD_CODEC_PARAMS;
	}
	return 0;
}

int AnalyzeAA3Track(const u8 *buffer, u32 size, u32 fileSize, Track *track, std::string *error) {
	if (size < 10) return (int)SCE_ERROR_ATRAC_AA3_SIZE_TOO_SMALL;
	if (!buffer) return (int)SCE_KERNEL_ERROR_INVALID_POINTER;
	if (buffer[0] != 'e' || buffer[1] != 'a' || buffer[2] != '3') return (int)SCE_ERROR_ATRAC_AA3_INVALID_DATA;

	u32 tagSize = buffer[9] | (buffer[8] << 7) | (buffer[7] << 14) | (buffer[6] << 21);
	if (size < tagSize + 46) return (int)SCE_ERROR_ATRAC_AA3_SIZE_TOO_SMALL;

	buffer = buffer + 10 + tagSize;
	if (buffer[0] != 'E' || buffer[1] != 'A' || buffer[2] != '3') {
		*error = "invalid EA3 magic";
		return (int)SCE_ERROR_ATRAC_AA3_INVALID_DATA;
	}

	track->fileSize = fileSize;
	u32 codecParams = buffer[33] | (buffer[34] << 8) | (buffer[35] << 16);
	const u32 at3SampleRates[8] = {32000, 44100, 48000, 88200, 96000, 0, 0, 0};

	switch (buffer[32]) {
	case 0:
		track->codecType = PSP_CODEC_AT3;
		track->bytesPerFrame = (u16)((codecParams & 0x03FF) * 8);
		track->bitrate = at3SampleRates[(codecParams >> 13) & 7] * track->bytesPerFrame * 8 / 1024;
		track->channels = 2;
		track->jointStereo = (codecParams >> 17) & 1;
		break;
	case 1:
		track->codecType = PSP_CODEC_AT3PLUS;
		track->bytesPerFrame = (u16)(((codecParams & 0x03FF) * 8) + 8);
		track->bitrate = at3SampleRates[(codecParams >> 13) & 7] * track->bytesPerFrame * 8 / 2048;
		track->channels = (codecParams >> 10) & 7;
		break;
	default:
		*error = "unsupported AA3 codec type";
		return (int)SCE_ERROR_ATRAC_AA3_INVALID_DATA;
	}

	track->dataByteOffset = 10 + tagSize + 96;
	track->firstSampleOffset = 0;
	if (track->endSample < 0 && track->bytesPerFrame != 0) {
		track->endSample = (int)((track->fileSize - track->dataByteOffset) / track->bytesPerFrame) * track->SamplesPerFrame();
	}
	track->endSample -= 1;
	return 0;
}

static const u8 g_atrac3Checkbytes[] = {
	0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62,
};

static inline int RoundUpToEven(int size) { return (size + 1) & ~1; }

int ParseWaveAT3(const u8 *data, u32 dataLength, TrackInfo *track) {
	track->loopStart = 0xFFFFFFFF;
	track->loopEnd = 0xFFFFFFFF;
	track->firstSampleOffset = 0;
	track->endSample = 0;
	track->waveDataSize = 0;

	int retval = (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
	int offset = 0;

	while (true) {
		if ((u32)offset + 0xC >= dataLength) return (int)SCE_ERROR_ATRAC_SIZE_TOO_SMALL;
		const u32 blockID = Read32p(data, &offset);
		if (blockID != RIFF_CHUNK_MAGIC) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
		const u32 blockSize = (u32)RoundUpToEven((int)Read32p(data, &offset));
		const u32 waveID = Read32p(data, &offset);
		if (waveID == RIFF_WAVE_MAGIC) break;
		if (blockSize < 4 || (u64)offset + blockSize - 4 > dataLength) return (int)SCE_ERROR_ATRAC_SIZE_TOO_SMALL;
		offset += blockSize - 4;
	}

	bool modifiedSampleOffset = false;
	while (true) {
		if ((u32)offset + 8 >= dataLength) return (int)SCE_ERROR_ATRAC_SIZE_TOO_SMALL;
		const u32 chunkID = Read32p(data, &offset);
		const int chunkSize = RoundUpToEven((int)Read32p(data, &offset));
		const int nextOffset = offset + chunkSize;
		if ((s64)offset + chunkSize > (s64)dataLength && chunkID != DATA_CHUNK_MAGIC) return (int)SCE_ERROR_ATRAC_SIZE_TOO_SMALL;
		switch (chunkID) {
		case DATA_CHUNK_MAGIC: {
			track->waveDataSize = (u32)chunkSize;
			track->dataOff = (u32)offset;
			if (!track->firstSampleOffset) track->firstSampleOffset = retval == PSP_CODEC_AT3 ? 0x400 : 0x800;
			if (modifiedSampleOffset && retval == PSP_CODEC_AT3PLUS) {
				track->firstSampleOffset -= 0xb8;
				if (track->loopEnd != 0xFFFFFFFF) {
					track->loopEnd -= 0xb8;
					track->loopStart -= 0xb8;
				}
			}
			return retval;
		}
		case FMT_CHUNK_MAGIC: {
			if (retval != (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			if (chunkSize < 0x20) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			const u16 fmtTag = (u16)Read16p(data, &offset);
			const u16 chans = (u16)Read16p(data, &offset);
			track->numChans = chans;
			if (chans != 1 && chans != 2) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			const int sampleRate = (int)Read32p(data, &offset);
			if (sampleRate != 44100) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			offset += 4;
			track->blockAlign = (u16)Read16p(data, &offset);
			if (track->blockAlign == 0) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			if (fmtTag == WAVE_FORMAT_AT3) {
				offset += 4;
				const u16 jointStereo = (u16)Read16p(data, &offset);
				if (jointStereo != 1) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
				offset += 4;
				const u16 sampleSizeTailFlag = (u16)Read16p(data, &offset);
				track->sampleSizeMaybe = (u8)sampleSizeTailFlag;
				track->tailFlag = (u8)(sampleSizeTailFlag >> 8);
				const u16 unknown2 = (u16)Read16p(data, &offset);
				if ((sampleSizeTailFlag & 0xffff) != unknown2) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
				const u32 supposedToBeOne = Read32p(data, &offset);
				if (supposedToBeOne != 1) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
				retval = PSP_CODEC_AT3;
			} else if (fmtTag == WAVE_FORMAT_EXTENSIBLE) {
				if (chunkSize < 0x34) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
				if (memcmp(data + offset + 10, g_atrac3Checkbytes, sizeof(g_atrac3Checkbytes)) != 0)
					return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
				track->sampleSizeMaybe = data[offset + 0x1c];
				track->tailFlag = data[offset + 0x1d];
				if ((u32)((u32)track->sampleSizeMaybe << 27) >> 29 != track->numChans) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
				offset += 0x26;
				retval = PSP_CODEC_AT3PLUS;
			} else {
				return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			}
			break;
		}
		case SMPL_CHUNK_MAGIC: {
			if ((int)track->loopStart < 0) {
				if (chunkSize < 0x20) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
				offset += 0x1c;
				const int numLoops = (int)Read32p(data, &offset);
				if (numLoops != 0) {
					if (chunkSize < 0x34) return (int)SCE_ERROR_ATRAC_SIZE_TOO_SMALL;
					offset += 0xc;
					track->loopStart = Read32p(data, &offset);
					track->loopEnd = Read32p(data, &offset);
					if ((int)track->loopEnd <= (int)track->loopStart) return (int)SCE_ERROR_ATRAC_BAD_CODEC_PARAMS;
				}
			}
			break;
		}
		case FACT_CHUNK_MAGIC: {
			if (chunkSize < 4) return (int)SCE_ERROR_ATRAC_UNKNOWN_FORMAT;
			track->endSample = Read32p(data, &offset);
			int remainingBytes = chunkSize - 4;
			if (remainingBytes == 4) {
				track->firstSampleOffset = Read32p(data, &offset);
			} else if (remainingBytes >= 8) {
				offset += 4;
				track->firstSampleOffset = Read32p(data, &offset);
				modifiedSampleOffset = true;
			}
			break;
		}
		default:
			break;
		}
		offset = nextOffset;
	}
}

namespace {

struct AA3Info {
	int tagSize;
	int dataOff;
	int extraBytes;
	u8 codecType;
	u32 codecParams;
	u32 waveDataSize;
};

u16 ParseU16BE(const u8 *data, int *offset) {
	const u16 value = (u16)((data[*offset] << 8) | data[*offset + 1]);
	*offset += 2;
	return value;
}

u32 Parse3BytesBE(const u8 *data, int *offset) {
	const u32 id = (data[*offset] << 16) | (data[*offset + 1] << 8) | data[*offset + 2];
	*offset += 3;
	return id;
}

u32 Parse28BitIntBE(const u8 *data, int *offset) {
	const u32 value = ((data[*offset] & 0x7F) << 21) | ((data[*offset + 1] & 0x7F) << 14) | ((data[*offset + 2] & 0x7F) << 7) |
	                  (data[*offset + 3] & 0x7F);
	*offset += 4;
	return value;
}

u32 Parse4BytesBE(const u8 *data, int *offset) {
	const u32 value = ((u32)data[*offset] << 24) | (data[*offset + 1] << 16) | (data[*offset + 2] << 8) | data[*offset + 3];
	*offset += 4;
	return value;
}

int ParseAA3Headers(u32 readSize, AA3Info *info, u32 fileSize, const u8 *aa3Data) {
	if (readSize < 10) return (int)SCE_ERROR_ATRAC_AA3_SIZE_TOO_SMALL;
	int offset = 0;
	const u32 id = Parse3BytesBE(aa3Data, &offset);
	if (id == 0x656133 || id == 0x494433) {  // "ea3" or "id3"
		const u8 *dataPtr = aa3Data + offset;
		if ((*dataPtr != 3) || (offset = offset + 3, dataPtr[1] != 0)) return (int)SCE_ERROR_ATRAC_AA3_OTHER_FAILURE;
		info->tagSize = (int)Parse28BitIntBE(aa3Data, &offset);
		offset += info->tagSize;
		if ((u32)offset >= readSize) return (int)SCE_ERROR_ATRAC_AA3_SIZE_TOO_SMALL;
		if (aa3Data[offset] == 0) offset += 0x10;
		info->dataOff = offset;
	} else {
		info->tagSize = 0;
		info->dataOff = 0;
		offset = 0;
	}
	if ((u32)offset + 0x22 > readSize) return (int)SCE_ERROR_ATRAC_AA3_SIZE_TOO_SMALL;
	if (Parse3BytesBE(aa3Data, &offset) != 0x454133) return (int)SCE_ERROR_ATRAC_AA3_INVALID_DATA;
	offset++;
	info->extraBytes = ParseU16BE(aa3Data, &offset);
	if (ParseU16BE(aa3Data, &offset) != 0xffff) return (int)SCE_ERROR_ATRAC_AA3_INVALID_DATA;
	offset += 24;
	info->codecType = aa3Data[offset];
	int codec;
	switch (info->codecType) {
	case 0: codec = PSP_CODEC_AT3; break;
	case 1: codec = PSP_CODEC_AT3PLUS; break;
	default: return (int)SCE_ERROR_ATRAC_AA3_INVALID_DATA;
	}
	info->codecParams = Parse4BytesBE(aa3Data, &offset);
	const u32 dataOff = info->dataOff + info->extraBytes;
	info->waveDataSize = fileSize - dataOff;
	info->dataOff = (int)dataOff;
	return codec;
}

int ConvertAA3InfoToTrackInfo(const AA3Info *in, TrackInfo *out) {
	out->loopEnd = 0xffffffff;
	out->loopStart = 0xffffffff;
	out->waveDataSize = in->waveDataSize;
	out->dataOff = (u32)in->dataOff;
	out->numChans = 2;
	out->endSample = 0;
	const u32 codecParams = in->codecParams;
	if (in->codecType == 0) {
		if ((codecParams & 0xe000) != 0x2000) return (int)SCE_ERROR_ATRAC_AA3_BAD_CODEC_PARAMS;
		out->firstSampleOffset = 0x400;
		out->blockAlign = (u16)((codecParams & 0x3ff) << 3);
		out->sampleSizeMaybe = (codecParams & 0x20000) == 0 ? 0 : 1;
		out->tailFlag = 0;
		return PSP_CODEC_AT3;
	} else if (in->codecType == 1) {
		if ((codecParams & 0x1c00) != 0x800 || (codecParams & 0xe000) != 0x2000) return (int)SCE_ERROR_ATRAC_AA3_BAD_CODEC_PARAMS;
		out->firstSampleOffset = 0x800;
		out->blockAlign = (u16)((codecParams & 0x3ff) * 8 + 8);
		out->sampleSizeMaybe = (u8)(((codecParams >> 8) & 0x3) | 0x28);
		out->tailFlag = (u8)(codecParams & 0xFF);
		return PSP_CODEC_AT3PLUS;
	}
	return (int)SCE_ERROR_ATRAC_AA3_INVALID_DATA;
}

}  // namespace

int ParseAA3(const u8 *buffer, u32 readSize, u32 fileSize, TrackInfo *track) {
	AA3Info info{};
	int retval = ParseAA3Headers(readSize, &info, fileSize, buffer);
	if (retval < 0) return retval;
	return ConvertAA3InfoToTrackInfo(&info, track);
}

}  // namespace audio
