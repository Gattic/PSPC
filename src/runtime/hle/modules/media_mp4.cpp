// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceMp4 and mp4msv: registered so games that import them link, with PPSSPP's placeholder
// behaviour (Core/HLE/sceMp4.cpp, https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP
// Project, GPL-2.0-or-later). MP4 playback itself isn't implemented.
#include <algorithm>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/media_hle.h"

namespace mediahle {

namespace {

u32 sceMp4Zero() {
	LOGD("MP4", "UNIMPL %s", hle::CurrentFunctionName());
	return 0;
}

u32 sceMp4GetNumberOfSpecificTrack() {
	LOGD("MP4", "UNIMPL sceMp4GetNumberOfSpecificTrack");
	return 1;
}

u32 sceMp4TrackSampleBufQueryMemSize(u32 unknown1, u32 unknown2, u32 unknown3, u32 unknown4, u32 unknown5) {
	(void)unknown1;
	return std::max(unknown2 * unknown3, unknown4 << 1) + (unknown2 << 6) + unknown5 + 256;
}

#define MP4ZERO(nid, name) hle::HleFunction{(u32)(nid), &hle::Wrap<&sceMp4Zero>, name, 0}

}  // namespace

void Register_sceMp4() {
	static const hle::HleFunction funcs[] = {
	    MP4ZERO(0x68651CBC, "sceMp4Init"),
	    MP4ZERO(0x9042B257, "sceMp4Finish"),
	    MP4ZERO(0xB1221EE7, "sceMp4Create"),
	    MP4ZERO(0x538C2057, "sceMp4Delete"),
	    MP4ZERO(0x113E9E7B, "sceMp4GetNumberOfMetaData"),
	    MP4ZERO(0x7443AF1D, "sceMp4GetMovieInfo"),
	    HLEFN(0x5EB65F26, sceMp4GetNumberOfSpecificTrack),
	    MP4ZERO(0x7ADFD01C, "sceMp4RegistTrack"),
	    HLEFN(0xBCA9389C, sceMp4TrackSampleBufQueryMemSize),
	    MP4ZERO(0x9C8F4FC1, "sceMp4TrackSampleBufConstruct"),
	    MP4ZERO(0x0F0187D2, "sceMp4GetAvcTrackInfoData"),
	    MP4ZERO(0x9CE6F5CF, "sceMp4GetAacTrackInfoData"),
	    MP4ZERO(0x4ED4AB1E, "sceMp4AacDecodeInitResource"),
	    MP4ZERO(0x10EE0D2C, "sceMp4AacDecodeInit"),
	    MP4ZERO(0x496E8A65, "sceMp4TrackSampleBufFlush"),
	    MP4ZERO(0xB4B400D1, "sceMp4GetSampleNumWithTimeStamp"),
	    MP4ZERO(0xF7C51EC1, "sceMp4GetSampleInfo"),
	    MP4ZERO(0x74A1CA3E, "sceMp4SearchSyncSampleNum"),
	    HLESTUB(0xD8250B75, "sceMp4PutSampleNum"),
	    MP4ZERO(0x8754ECB8, "sceMp4TrackSampleBufAvailableSize"),
	    HLESTUB(0x31BCD7E0, "sceMp4TrackSampleBufPut"),
	    MP4ZERO(0x5601A6F0, "sceMp4GetAacAu"),
	    MP4ZERO(0x7663CB5C, "sceMp4AacDecode"),
	    MP4ZERO(0x503A3CBA, "sceMp4GetAvcAu"),
	    HLESTUB(0x01C76489, "sceMp4TrackSampleBufDestruct"),
	    HLESTUB(0x6710FE77, "sceMp4UnregistTrack"),
	    HLESTUB(0x5D72B333, "sceMp4AacDecodeExit"),
	    HLESTUB(0x7D332394, "sceMp4AacDecodeTermResource"),
	    MP4ZERO(0x131BDE57, "sceMp4InitAu"),
	    HLESTUB(0x17EAA97D, "sceMp4GetAvcAuWithoutSampleBuf"),
	    HLESTUB(0x28CCB940, "sceMp4GetTrackEditList"),
	    HLESTUB(0x3069C2B5, "sceMp4GetAvcParamSet"),
	    HLESTUB(0xD2AC9A7E, "sceMp4GetMetaData"),
	    HLESTUB(0x4FB5B756, "sceMp4GetMetaDataInfo"),
	    HLESTUB(0x427BEF7F, "sceMp4GetTrackNumOfEditList"),
	    HLESTUB(0x532029B8, "sceMp4GetAacAuWithoutSampleBuf"),
	    HLESTUB(0xA6C724DC, "sceMp4GetSampleNum"),
	};
	hle::RegisterModule("sceMp4", funcs);
	static const hle::HleFunction msv[] = {
	    MP4ZERO(0x3C2183C7, "mp4msv_3C2183C7"),
	    MP4ZERO(0x9CA13D1A, "mp4msv_9CA13D1A"),
	};
	hle::RegisterModule("mp4msv", msv);
}

}  // namespace mediahle
