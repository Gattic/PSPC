// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/guest_memory.h"

#include "audio/audio_common.h"

namespace audio {

static GuestPtrFn g_guestPtr = nullptr;

void SetGuestMemory(GuestPtrFn fn) { g_guestPtr = fn; }

u8 *GuestPtr(u32 addr, u32 size) {
	if (!g_guestPtr) return nullptr;
	return g_guestPtr(addr, size);
}

const char *CodecName(int codec) {
	static const char *const names[4] = {"AT3+", "AT3", "MP3", "AAC"};
	if (codec >= PSP_CODEC_AT3PLUS && codec <= PSP_CODEC_AAC) return names[codec - PSP_CODEC_AT3PLUS];
	return "(unk)";
}

}  // namespace audio
