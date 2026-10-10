// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/Dialog/SavedataParam.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later. See save_crypto.h.
#include "runtime/dialogs/save_crypto.h"

#include <cstdint>
#include <cstring>

#include "common/log.h"
#include "runtime/dialogs/save_fs.h"
#include "runtime/hle/modules/chnnlsv.h"
#include "runtime/hle/modules/kirk_client.h"

namespace savecrypto {

namespace {

size_t Align16(size_t n) { return (n + 15) & ~(size_t)15; }

// The MAC of `alignedLen` bytes (the caller zero-pads to a multiple of 16).
bool BuildHash(u8 out[16], const u8 *data, size_t alignedLen, int mode) {
	chnnlsv::MacContext ctx{};
	memset(out, 0, 16);
	if (chnnlsv::MacInit(ctx, mode & 0xFF) < 0) return false;
	if (chnnlsv::MacUpdate(ctx, data, (int)alignedLen) < 0) return false;
	return chnnlsv::MacFinal(ctx, out, nullptr) >= 0;
}

}  // namespace

bool Available() { return kirk::Available(); }

bool Encrypt(int mode, const u8 *key, const u8 *data, size_t size, std::vector<u8> &out, u8 hash[16]) {
	const size_t aligned = Align16(size);
	std::vector<u8> buf(aligned + 0x10, 0);
	if (size) memcpy(buf.data() + 0x10, data, size);
	chnnlsv::MacContext mac{};
	chnnlsv::CipherContext cipher{};
	memset(hash, 0, 16);
	// The IV (random, encrypted) goes in front of the data.
	if (chnnlsv::CipherInit(cipher, mode, 1, buf.data(), key) < 0) return false;
	if (chnnlsv::MacInit(mac, mode) < 0) return false;
	if (chnnlsv::MacUpdate(mac, buf.data(), 0x10) < 0) return false;
	if (chnnlsv::CipherUpdate(cipher, buf.data() + 0x10, (int)aligned) < 0) return false;
	// The MAC covers the encrypted data with its padding zeroed.
	memset(buf.data() + 0x10 + size, 0, aligned - size);
	if (chnnlsv::MacUpdate(mac, buf.data() + 0x10, (int)aligned) < 0) return false;
	if (chnnlsv::CipherFinal(cipher) < 0) return false;
	if (chnnlsv::MacFinal(mac, hash, key) < 0) return false;
	buf.resize(size + 0x10);
	out = std::move(buf);
	return true;
}

bool Decrypt(int mode, const u8 *key, const std::vector<u8> &file, std::vector<u8> &out, u8 hash[16]) {
	const size_t aligned = Align16(file.size());
	if (aligned <= 0x10) return false;
	std::vector<u8> buf(aligned, 0);
	memcpy(buf.data(), file.data(), file.size());
	const size_t dataLen = file.size() - 0x10, alignedLen = aligned - 0x10;
	chnnlsv::MacContext mac{};
	chnnlsv::CipherContext cipher{};
	if (chnnlsv::MacInit(mac, mode) < 0) return false;
	if (chnnlsv::CipherInit(cipher, mode, 2, buf.data(), key) < 0) return false;
	if (chnnlsv::MacUpdate(mac, buf.data(), 0x10) < 0) return false;
	if (chnnlsv::MacUpdate(mac, buf.data() + 0x10, (int)alignedLen) < 0) return false;
	if (chnnlsv::CipherUpdate(cipher, buf.data() + 0x10, (int)alignedLen) < 0) return false;
	if (chnnlsv::CipherFinal(cipher) < 0) return false;
	if (chnnlsv::MacFinal(mac, hash, key) < 0) return false;
	out.assign(buf.begin() + 0x10, buf.begin() + 0x10 + dataLen);
	return true;
}

bool UpdateHash(std::vector<u8> &sfo, size_t paramsOffset, int encryptMode, bool keep0x40) {
	if (paramsOffset + 0x80 > sfo.size()) return false;
	const size_t size = sfo.size(), aligned = Align16(size);
	sfo.resize(aligned, 0);  // the hashes cover the file padded to 16 bytes
	u8 *params = sfo.data() + paramsOffset;
	u8 marker[16];
	memcpy(marker, params + 0x40, 16);
	memset(params, 0, 0x80);
	if (keep0x40) memcpy(params + 0x40, marker, 16);

	int firstHashMode = (encryptMode & 2) ? 4 : 2;
	int secondHashMode = (encryptMode & 2) ? 3 : 0;
	if (encryptMode & 4) {
		firstHashMode = 6;
		secondHashMode = 5;
	}
	u8 h[16];
	bool ok = true;
	// The console-keyed hash ("11D0") over the whole file, and the flag that says it's there.
	ok &= BuildHash(h, sfo.data(), aligned, firstHashMode);
	memcpy(params + 0x20, h, 16);
	params[0] |= 0x01;
	// The newer modes add the "1220" hash.
	if (encryptMode & 6) {
		params[0] |= (u8)((encryptMode & 6) << 4);
		ok &= BuildHash(h, sfo.data(), aligned, secondHashMode);
		memcpy(params + 0x70, h, 16);
	}
	// And the "11C0" hash, which covers the others.
	ok &= BuildHash(h, sfo.data(), aligned, 1);
	memcpy(params + 0x10, h, 16);
	sfo.resize(size);
	if (!ok) LOGW("UTIL", "Savedata: PARAM.SFO hash (mode %d) failed", encryptMode);
	return ok;
}

int AddMissingSfoHashes() {
	static const char kRoot[] = "ms0:/PSP/SAVEDATA/";
	static const u8 kMarker[15] = {'P', 'S', 'P', 'C', '-', 'P', 'L', 'A', 'I', 'N', 'T', 'E', 'X', 'T', 0};
	int fixed = 0;
	for (const utility::savefs::FileInfo &dir : utility::savefs::List(kRoot)) {
		if (!dir.isDir) continue;
		const std::string path = kRoot + dir.name + "/PARAM.SFO";
		std::vector<u8> sfo;
		if (!utility::savefs::ReadAll(path, sfo)) continue;
		const size_t at = SfoValueOffset(sfo, "SAVEDATA_PARAMS");
		if (at == SIZE_MAX || at + 0x80 > sfo.size()) continue;
		u8 *params = sfo.data() + at;
		bool hashed = false;
		for (int i = 0x10; i < 0x20; i++) hashed |= params[i] != 0;
		if (hashed) continue;
		if (!Available()) return fixed;
		bool ok;
		if (memcmp(params + 0x40, kMarker, sizeof(kMarker)) == 0) {
			// Secure files PSPC keeps in the clear: hashed like a PSP save of the same mode, marker kept.
			const int mode = (params[0] & 0x40) ? 5 : (params[0] & 0x20) ? 3 : 1;
			ok = UpdateHash(sfo, at, mode, true);
		} else if (params[0] == 0) {
			// Nothing encrypted (PSPC's unencrypted saves, PPSSPP's): only the hash the XMB checks, so
			// every reader still sees an unencrypted save.
			const size_t size = sfo.size();
			sfo.resize(Align16(size), 0);
			u8 h[16];
			ok = BuildHash(h, sfo.data(), sfo.size(), 1);
			sfo.resize(size);
			memcpy(sfo.data() + at + 0x10, h, 16);
		} else {
			continue;  // flagged as hashed but without hashes: not ours to fix
		}
		if (ok && utility::savefs::WriteAll(path, sfo.data(), sfo.size())) {
			LOGI("UTIL", "Savedata %s: added the PARAM.SFO hashes the XMB checks", dir.name.c_str());
			fixed++;
		}
	}
	return fixed;
}

size_t SfoValueOffset(const std::vector<u8> &sfo, const char *key) {
	auto rd32 = [&](size_t at) -> u32 {
		u32 v;
		memcpy(&v, sfo.data() + at, 4);
		return v;
	};
	if (sfo.size() < 0x14 || memcmp(sfo.data(), "\0PSF", 4) != 0) return SIZE_MAX;
	const u32 keyTable = rd32(8), dataTable = rd32(12), count = rd32(16);
	const size_t keyLen = strlen(key);
	for (u32 i = 0; i < count && 0x14 + (size_t)i * 16 + 16 <= sfo.size(); i++) {
		const size_t idx = 0x14 + (size_t)i * 16;
		const size_t nameAt = keyTable + (size_t)(sfo[idx] | (sfo[idx + 1] << 8));
		if (nameAt + keyLen + 1 > sfo.size() || memcmp(sfo.data() + nameAt, key, keyLen + 1) != 0) continue;
		const size_t at = (size_t)dataTable + rd32(idx + 12);
		return at <= sfo.size() ? at : SIZE_MAX;
	}
	return SIZE_MAX;
}

}  // namespace savecrypto
