// SPDX-License-Identifier: GPL-3.0-or-later
// pspc-decrypt kirk-service: see kirk_service.h for the protocol.
#include "kirk_service.h"

#include <cstdio>
#include <memory>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "../libkirk/AES.h"
#include "../libkirk/SHA1.h"
#include "../libkirk/kirk_engine.h"

struct KirkServiceState {
	KirkState kirk{};
	u8 fuseKey[16] = {};
};

namespace {

// KIRK commands 5 and 8: AES-128-CBC (zero IV) with the console's own key. A PSP derives that key
// inside KIRK from its fuse ID in a way that isn't public; PSPC uses a key derived from the fuse
// ID it was given, which serves the same purpose (data only "this console" can read back).
void DeriveFuseKey(u64 fuseId, u8 key[16]) {
	u8 msg[24] = {'P', 'S', 'P', 'C', '-', 'K', 'I', 'R', 'K', '-', 'F', 'U', 'S', 'E', '-', 'K'};
	for (int i = 0; i < 8; i++) msg[16 + i] = (u8)(fuseId >> (8 * i));
	SHA_CTX sha;
	SHAInit(&sha);
	SHAUpdate(&sha, msg, (int)sizeof(msg));
	u8 digest[20];
	SHAFinal(digest, &sha);
	memcpy(key, digest, 16);
}

int FuseCommand(KirkServiceState *s, bool encrypt, u8 *out, int outSize, const u8 *in, int inSize) {
	if (inSize < 0x14) return KIRK_INVALID_SIZE;
	const u32 mode = rd32(in), dataSize = rd32(in + 0x10);
	if (mode != (u32)(encrypt ? KIRK_MODE_ENCRYPT_CBC : KIRK_MODE_DECRYPT_CBC)) return KIRK_INVALID_MODE;
	if (dataSize == 0) return KIRK_DATA_SIZE_ZERO;
	if ((dataSize & 15) || 0x14 + (u64)dataSize > (u64)inSize) return KIRK_INVALID_SIZE;
	// Like commands 4/7: encryption writes after the header, decryption from the start.
	if ((encrypt ? 0x14 : 0) + (u64)dataSize > (u64)outSize) return KIRK_INVALID_SIZE;
	AES_ctx aes;
	AES_set_key(&aes, s->fuseKey, 128);
	if (encrypt)
		AES_cbc_encrypt(&aes, in + 0x14, out + 0x14, (int)dataSize);
	else
		AES_cbc_decrypt(&aes, in + 0x14, out, (int)dataSize);
	return KIRK_OPERATION_SUCCESS;
}

bool ReadAll(FILE *f, void *buf, size_t size) { return size == 0 || fread(buf, 1, size, f) == size; }
bool WriteAll(FILE *f, const void *buf, size_t size) { return size == 0 || fwrite(buf, 1, size, f) == size; }

}  // namespace

KirkServiceState *KirkServiceCreate(u64 fuseId) {
	auto *s = new KirkServiceState;
	static const char seed[] = "pspc-decrypt kirk-service";
	kirk_init2(&s->kirk, (u8 *)seed, (u32)sizeof(seed) - 1, (u32)(fuseId >> 32), (u32)fuseId);
	DeriveFuseKey(fuseId, s->fuseKey);
	return s;
}

void KirkServiceDestroy(KirkServiceState *s) { delete s; }

int KirkServiceCommand(KirkServiceState *s, int cmd, u8 *out, int outSize, u8 *in, int inSize) {
	switch (cmd) {
	case 0: return 0;
	case KIRK_CMD_ENCRYPT_IV_FUSE: return FuseCommand(s, true, out, outSize, in, inSize);
	case KIRK_CMD_DECRYPT_IV_FUSE: return FuseCommand(s, false, out, outSize, in, inSize);
	case KIRK_CMD_ENCRYPT_IV_0:
	case KIRK_CMD_DECRYPT_IV_0: {
		// libkirk trusts the header's size; keep it inside the buffers.
		if (inSize < 0x14) return KIRK_INVALID_SIZE;
		const u32 dataSize = rd32(in + 0x10);
		const u64 outNeed = (cmd == KIRK_CMD_ENCRYPT_IV_0 ? 0x14 : 0) + (u64)dataSize;
		if (0x14 + (u64)dataSize > (u64)inSize || outNeed > (u64)outSize) return KIRK_INVALID_SIZE;
		break;
	}
	case KIRK_CMD_SHA1_HASH:
		if (inSize < 4 || 4 + (u64)rd32(in) > (u64)inSize || outSize < 0x14) return KIRK_INVALID_SIZE;
		break;
	case KIRK_CMD_PRNG:
		break;
	default:
		// The signature and key commands have fixed-size buffers.
		if (cmd == KIRK_CMD_DECRYPT_PRIVATE || cmd == KIRK_CMD_PRIV_SIGN_CHECK) {
			if (inSize < 0x90) return KIRK_INVALID_SIZE;
			const u32 dataSize = rd32(in + 0x70), dataOffset = rd32(in + 0x74);
			if (0x90 + (u64)dataOffset + dataSize > (u64)inSize) return KIRK_INVALID_SIZE;
			if (cmd == KIRK_CMD_DECRYPT_PRIVATE && (u64)dataSize > (u64)outSize) return KIRK_INVALID_SIZE;
		} else if (cmd == KIRK_CMD_ECDSA_GEN_KEYS) {
			if (outSize < 0x3C) return KIRK_INVALID_SIZE;
		} else if (cmd == KIRK_CMD_ECDSA_MULTIPLY_POINT) {
			if (inSize < 0x3C || outSize < 0x28) return KIRK_INVALID_SIZE;
		} else if (cmd == KIRK_CMD_ECDSA_SIGN) {
			if (inSize < 0x34 || outSize < 0x28) return KIRK_INVALID_SIZE;
		} else if (cmd == KIRK_CMD_ECDSA_VERIFY) {
			if (inSize < 0x64) return KIRK_INVALID_SIZE;
		} else {
			return KIRK_INVALID_OPERATION;
		}
		break;
	}
	return kirk_sceUtilsBufferCopyWithRange(&s->kirk, out, outSize, in, inSize, cmd);
}

int RunKirkService(u64 fuseId) {
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	std::unique_ptr<KirkServiceState, void (*)(KirkServiceState *)> s(KirkServiceCreate(fuseId), KirkServiceDestroy);
	Bytes in, out;
	for (;;) {
		u8 head[20];
		if (!ReadAll(stdin, head, sizeof(head))) return 0;  // the runtime went away
		const u32 magic = rd32(head), cmd = rd32(head + 4), flags = rd32(head + 8);
		const u32 inSize = rd32(head + 12), outSize = rd32(head + 16);
		if (magic != KIRK_SERVICE_REQUEST || inSize > KIRK_SERVICE_MAX_BUFFER || outSize > KIRK_SERVICE_MAX_BUFFER ||
		    ((flags & KIRK_SERVICE_IN_PLACE) && outSize > inSize)) {
			fprintf(stderr, "pspc-decrypt kirk-service: bad request\n");
			return 2;
		}
		in.assign(inSize, 0);
		if (!ReadAll(stdin, in.data(), inSize)) return 0;
		int result;
		u8 *outPtr;
		if (flags & KIRK_SERVICE_IN_PLACE) {
			outPtr = in.data();
			result = KirkServiceCommand(s.get(), (int)cmd, outPtr, (int)outSize, in.data(), (int)inSize);
		} else {
			out.assign(outSize, 0);
			if (!ReadAll(stdin, out.data(), outSize)) return 0;
			outPtr = out.data();
			result = KirkServiceCommand(s.get(), (int)cmd, outPtr, (int)outSize, in.data(), (int)inSize);
		}
		u8 reply[12];
		wr32(reply, KIRK_SERVICE_RESPONSE);
		wr32(reply + 4, (u32)result);
		wr32(reply + 8, outSize);
		if (!WriteAll(stdout, reply, sizeof(reply)) || !WriteAll(stdout, outPtr, outSize) || fflush(stdout) != 0) return 0;
	}
}
