// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/Dialog/SavedataParam.cpp, Core/HW/MemoryStick.cpp), Copyright (c) 2012-
// PPSSPP Project, GPL-2.0-or-later.
#include "runtime/dialogs/savedata.h"

#include <algorithm>
#include <cstring>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "common/sha1.h"
#include "fs/param_sfo.h"
#include "runtime/core/memory.h"
#include "runtime/dialogs/save_fs.h"
#include "runtime/kernel/errors.h"

namespace utility {

const char *const Savedata::kSavePath = "ms0:/PSP/SAVEDATA/";

namespace {

const std::string ICON0_FILENAME = "ICON0.PNG";
const std::string ICON1_FILENAME = "ICON1.PMF";
const std::string PIC1_FILENAME = "PIC1.PNG";
const std::string SND0_FILENAME = "SND0.AT3";
const std::string SFO_FILENAME = "PARAM.SFO";

struct SfoFileListEntry {
	char filename[13];
	u8 hash[16];
	u8 pad[3];
};
static_assert(sizeof(SfoFileListEntry) == 32, "SAVEDATA_FILE_LIST entry");
constexpr int FILE_LIST_COUNT_MAX = 99;
constexpr u32 FILE_LIST_TOTAL_SIZE = sizeof(SfoFileListEntry) * FILE_LIST_COUNT_MAX;

constexpr u64 kMemoryStickSize = 16ull * 1024 * 1024 * 1024;

std::string FixedString(const char *s, size_t maxLen) { return std::string(s, strnlen(s, maxLen)); }

template <size_t N>
void CopyToField(char (&dst)[N], const std::string &src) {
	memset(dst, 0, N);
	memcpy(dst, src.data(), std::min(src.size(), N));
}

// Copies up to n bytes like strncpy (zero-padding the rest).
void StrNCopy(char *dst, const std::string &src, size_t n) {
	size_t len = std::min(src.size(), n);
	memcpy(dst, src.data(), len);
	if (len < n) memset(dst + len, 0, n - len);
}

void TruncateCopy(char *dst, size_t dstSize, const std::string &src) {
	size_t len = std::min(src.size(), dstSize - 1);
	memcpy(dst, src.data(), len);
	dst[len] = 0;
}

int SizeNormalized(u64 size) {
	return (int)(((size + kMemoryStickSectorSize - 1) / kMemoryStickSectorSize) * kMemoryStickSectorSize);
}

bool ValidBuf(u32 addr) { return addr != 0 && mem::IsValidAddress(addr); }

bool HasPathTraversal(const std::string &s) {
	if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return true;
	return s == "." || s == "..";
}

bool PSPMatch(std::string_view text, std::string_view regexp) {
	if (text.empty() && regexp.empty()) return true;
	if (regexp == "*") return true;
	if (text.empty()) return false;
	if (regexp.empty()) return false;
	if (regexp == "?" && text.length() == 1) return true;
	if (text == regexp) return true;
	if (regexp[0] == '*') {
		bool res = PSPMatch(text.substr(1), regexp.substr(1));
		if (!res) res = PSPMatch(text.substr(1), regexp);
		return res;
	}
	if (regexp[0] == '?') return PSPMatch(text.substr(1), regexp.substr(1));
	if (regexp[0] == text[0]) return PSPMatch(text.substr(1), regexp.substr(1));
	return false;
}

int GetSDKMainVersion(u32 sdkVersion) {
	if (sdkVersion > 0x0307FFFF) return 6;
	if (sdkVersion > 0x0300FFFF) return 5;
	if (sdkVersion > 0x0206FFFF) return 4;
	if (sdkVersion > 0x0205FFFF) return 3;
	if (sdkVersion >= 0x02000000) return 2;
	if (sdkVersion >= 0x01000000) return 1;
	return 0;
}

// secureVersion and the key only matter for the full 1536-byte request (the two older sizes have
// neither field); see utility/savedata/secureversion.
bool UsesSecureVersionField(const SavedataRequest *param) { return param->common.size >= 1536; }

// The encryption mode (sceChnnlsv hash mode) a PSP uses for a secure save. SDK 2.07 and later get
// the new keyed hash for versions 0 and 3 (utility/savedata/secureversion).
int DetermineCryptMode(const SavedataRequest *param) {
	if (!UsesSecureVersionField(param)) return 1;
	const bool newHash = GetSDKMainVersion(CompiledSdkVersion()) >= 4;
	switch (param->secureVersion) {
	case 0: return newHash ? 5 : 3;
	case 2: return 3;
	case 3: return newHash ? 5 : 1;
	default: return 1;
	}
}

// SAVEDATA_PARAMS byte 0 for an encryption mode: 0x01, plus the 0x1220 hash bit of modes 3 and 5.
u8 CryptModeFlag(int mode) { return (u8)(0x01 | ((mode & 6) << 4)); }

// Marks a save PSPC stored unencrypted (where a PSP keeps nothing: hashes sit at 0x10, 0x20, 0x70).
constexpr u32 kPlainMarkerOffset = 0x40;
constexpr char kPlainMarker[16] = "PSPC-PLAINTEXT";

bool HasPlainMarker(const std::vector<u8> &params) {
	return params.size() >= kPlainMarkerOffset + sizeof(kPlainMarker) &&
	       memcmp(params.data() + kPlainMarkerOffset, kPlainMarker, sizeof(kPlainMarker)) == 0;
}

bool HasAnyKey(const SavedataRequest *param) {
	for (u8 b : param->key)
		if (b) return true;
	return false;
}

// Stands in for a PSP's file hash, which depends on the game key: lets a load with the wrong key
// fail like decryption would.
void KeyHash(const SavedataRequest *param, int mode, u8 out[16]) {
	memset(out, 0, 16);
	if (mode <= 1 || !HasAnyKey(param)) return;
	Sha1 h;
	h.Update(param->key, sizeof(param->key));
	h.Update("PSPC", 4);
	auto d = h.Final();
	memcpy(out, d.data(), 16);
}
bool LoadSfoFile(const std::string &path, vfs::ParamSfo &sfo) {
	std::vector<u8> data;
	if (!savefs::ReadAll(path, data)) return false;
	return sfo.Parse(data);
}

savefs::FileInfo FileFromListing(const std::vector<savefs::FileInfo> &listing, const std::string &name) {
	for (const auto &f : listing)
		if (f.name == name) return f;
	savefs::FileInfo f;
	f.name = name;
	return f;
}

// Total size and dates of a save directory (dates of its PARAM.SFO, or else its first file).
SaveDirInfo GetSaveInfo(const std::string &saveDir) {
	SaveDirInfo info;
	savefs::FileInfo dir = savefs::Stat(saveDir);
	if (!dir.exists) return info;
	info.exists = true;
	info.ctime = dir.ctime;
	info.atime = dir.atime;
	info.mtime = dir.mtime;
	bool first = true;
	for (const auto &f : savefs::List(saveDir)) {
		if (f.isDir) continue;
		if (first || f.name == SFO_FILENAME) {
			info.ctime = f.ctime;
			info.mtime = f.mtime;
			info.atime = f.atime;
			first = false;
		}
		info.size += f.size;
	}
	return info;
}

u64 DateKey(const vfs::ScePspDateTime &t) {
	return ((u64)t.year << 40) | ((u64)t.month << 36) | ((u64)t.day << 31) | ((u64)t.hour << 26) |
	       ((u64)t.minute << 20) | ((u64)t.second << 14);
}

}  // namespace

u64 MemoryStickFreeSpace(const std::string &gameName) {
	u64 used = 0;
	for (const auto &d : savefs::List(Savedata::kSavePath)) {
		if (!d.isDir || d.name.compare(0, gameName.size(), gameName) != 0) continue;
		used += savefs::TreeSize(std::string(Savedata::kSavePath) + d.name);
	}
	return used < kMemoryStickSize ? kMemoryStickSize - used : 0;
}

void Savedata::Init() {
	savefs::Mkdir("ms0:/PSP");
	savefs::Mkdir("ms0:/PSP/SAVEDATA");
}

std::string Savedata::GetSaveDirName(const SavedataRequest *param, int saveId) const {
	if (!param) return "";
	if (saveId >= 0 && saveNameListDataCount_ > 0) return GetFilename(saveId);
	return GetSaveName(param);
}

std::string Savedata::GetSaveDir(const SavedataRequest *param, const std::string &saveDirName) const {
	if (!param) return "";
	return GetGameName(param) + saveDirName;
}

std::string Savedata::GetSaveDir(const SavedataRequest *param, int saveId) const {
	return GetSaveDir(param, GetSaveDirName(param, saveId));
}

std::string Savedata::GetSaveFilePath(const SavedataRequest *param, const std::string &saveDir) const {
	if (!param || saveDir.empty()) return "";
	return kSavePath + saveDir;
}

std::string Savedata::GetGameName(const SavedataRequest *param) const {
	return FixedString(param->gameName, sizeof(param->gameName));
}

std::string Savedata::GetSaveName(const SavedataRequest *param) const {
	std::string s = FixedString(param->saveName, sizeof(param->saveName));
	return s == "<>" ? "" : s;
}

std::string Savedata::GetFileName(const SavedataRequest *param) const {
	return FixedString(param->fileName, sizeof(param->fileName));
}

bool Savedata::HasKey(const SavedataRequest *param) const {
	for (u8 b : param->key)
		if (b) return true;
	return false;
}

bool Savedata::Delete(SavedataRequest *param, const std::string &saveDir) {
	if (!param) return false;
	// Never delete all of SAVEDATA/ because of an empty game name.
	if (!strnlen(param->gameName, sizeof(param->gameName)) && param->mode != SAVEDATA_LISTALLDELETE) {
		LOGE("UTIL", "Savedata: empty gameName, not deleting the save directory");
		return false;
	}
	std::string dirPath = GetSaveFilePath(param, saveDir);
	if (dirPath.empty() || !savefs::Exists(dirPath)) return false;
	savefs::RemoveTree(dirPath);
	return true;
}

int Savedata::DeleteData(SavedataRequest *param) {
	if (!param) return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_FILE_NOT_FOUND;
	std::string subFolder = GetGameName(param) + GetSaveName(param);
	std::string fileName = GetFileName(param);
	std::string dirPath = kSavePath + subFolder;
	std::string filePath = dirPath + "/" + fileName;
	std::string sfoPath = dirPath + "/" + SFO_FILENAME;

	if (!savefs::Exists(dirPath)) return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA;
	if (!savefs::Exists(sfoPath)) return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_DATA_BROKEN;
	if (!fileName.empty() && !savefs::Exists(filePath)) return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_FILE_NOT_FOUND;
	if (fileName.empty() || subFolder.empty()) return 0;

	savefs::Remove(filePath);

	// Take the file out of PARAM.SFO's list, if it was there (TITLE etc. stay as they are).
	vfs::ParamSfo sfo;
	if (LoadSfoFile(sfoPath, sfo)) {
		std::vector<u8> list;
		if (sfo.GetBinary("SAVEDATA_FILE_LIST", list)) {
			bool changed = false;
			size_t count = list.size() / sizeof(SfoFileListEntry);
			auto *entries = (SfoFileListEntry *)list.data();
			for (size_t i = 0; i < count; ++i) {
				if (strncmp(entries[i].filename, fileName.c_str(), sizeof(entries[i].filename)) != 0) continue;
				memset(entries[i].filename, 0, sizeof(entries[i].filename));
				memset(entries[i].hash, 0, sizeof(entries[i].hash));
				changed = true;
				break;
			}
			if (changed) {
				sfo.SetBinary("SAVEDATA_FILE_LIST", list.data(), list.size(), FILE_LIST_TOTAL_SIZE);
				std::vector<u8> out = sfo.Serialize();
				savefs::WriteAll(sfoPath, out.data(), out.size());
			}
		}
	}
	return 0;
}

int Savedata::Save(SavedataRequest *param, const std::string &saveDirName, bool secureMode) {
	if (!param) return (int)SCE_UTILITY_SAVEDATA_ERROR_SAVE_MS_NOSPACE;
	if (param->dataSize > param->dataBufSize) {
		LOGE("UTIL", "Savedata buffer overflow: %u / %u", param->dataSize, param->dataBufSize);
		return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_BAD_PARAMS;
	}
	auto validateSize = [](const SavedataFileData &d) { return !(ValidBuf(d.buf) && d.bufSize < d.size); };
	if (!validateSize(param->icon0FileData) || !validateSize(param->icon1FileData) ||
	    !validateSize(param->pic1FileData) || !validateSize(param->snd0FileData)) {
		return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_BAD_PARAMS;
	}
	if (param->secureVersion > 3) {
		LOGE("UTIL", "Savedata version requested on save: %u", param->secureVersion);
		return (int)SCE_UTILITY_SAVEDATA_ERROR_SAVE_PARAM;
	} else if (secureMode && MissingRequiredKey(param)) {
		LOGE("UTIL", "Savedata version %u with missing key on save", param->secureVersion);
		return (int)SCE_UTILITY_SAVEDATA_ERROR_SAVE_PARAM;
	}

	std::string dirPath = GetSaveFilePath(param, GetSaveDir(param, saveDirName));
	if (dirPath.empty()) return (int)SCE_UTILITY_SAVEDATA_ERROR_SAVE_MS_NOSPACE;
	// WRITEDATA only writes into a save MAKEDATA created (utility/savedata/saveemptyfilename).
	if ((param->mode == SAVEDATA_WRITEDATA || param->mode == SAVEDATA_WRITEDATASECURE) && !savefs::Exists(dirPath))
		return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA;
	if (!savefs::Exists(dirPath) && !savefs::Mkdir(dirPath)) {
		LOGE("UTIL", "Unable to create %s", dirPath.c_str());
	}

	// PARAM.SFO
	std::string sfoPath = dirPath + "/" + SFO_FILENAME;
	vfs::ParamSfo sfo;
	LoadSfoFile(sfoPath, sfo);
	// A sub-write (WRITEDATA) without a title keeps the existing values.
	if (strnlen(param->sfoParam.title, sizeof(param->sfoParam.title)) > 0) {
		sfo.SetString("TITLE", FixedString(param->sfoParam.title, sizeof(param->sfoParam.title)), 128);
		sfo.SetString("SAVEDATA_TITLE", FixedString(param->sfoParam.savedataTitle, sizeof(param->sfoParam.savedataTitle)), 128);
		sfo.SetString("SAVEDATA_DETAIL", FixedString(param->sfoParam.detail, sizeof(param->sfoParam.detail)), 1024);
		sfo.SetInt("PARENTAL_LEVEL", param->sfoParam.parentalLevel);
		sfo.SetString("CATEGORY", "MS", 4);
		sfo.SetString("SAVEDATA_DIRECTORY", GetSaveDir(param, saveDirName), 64);
	}

	// What a PSP would encrypt: the data file of a secure write. A sub-write (WRITEDATA) into an
	// encrypted save keeps its mode.
	const bool encrypt = secureMode && ValidBuf(param->dataBuf);
	const bool subWrite = param->mode == SAVEDATA_WRITEDATASECURE || param->mode == SAVEDATA_WRITEDATA;
	std::vector<u8> oldParams;
	const bool wasCrypted = sfo.GetBinary("SAVEDATA_PARAMS", oldParams) && !oldParams.empty() && oldParams[0] != 0;

	// The list of secure files: 13 bytes of name, 16 of hash, 3 of padding.
	std::vector<u8> list(FILE_LIST_TOTAL_SIZE, 0);
	{
		std::vector<u8> old;
		if (sfo.GetBinary("SAVEDATA_FILE_LIST", old)) memcpy(list.data(), old.data(), std::min<size_t>(old.size(), list.size()));
	}
	const int cryptMode = DetermineCryptMode(param);
	if (encrypt) {
		const std::string saveFilename = GetFileName(param);
		auto *entries = (SfoFileListEntry *)list.data();
		for (int i = 0; i < FILE_LIST_COUNT_MAX; ++i) {
			SfoFileListEntry &e = entries[i];
			if (e.filename[0] != '\0' && strncmp(e.filename, saveFilename.c_str(), sizeof(e.filename)) != 0) continue;
			memset(e.filename, 0, sizeof(e.filename));
			memcpy(e.filename, saveFilename.data(), std::min<size_t>(saveFilename.size(), sizeof(e.filename) - 1));
			KeyHash(param, cryptMode, e.hash);
			break;
		}
	}
	sfo.SetBinary("SAVEDATA_FILE_LIST", list.data(), list.size(), FILE_LIST_TOTAL_SIZE);
	// SAVEDATA_PARAMS: byte 0 is the encryption mode a PSP would have used; the PSPC marker says the
	// data is stored in the clear (see savedata.h).
	u8 params[128]{};
	if (encrypt) {
		params[0] = CryptModeFlag(cryptMode);
		memcpy(params + kPlainMarkerOffset, kPlainMarker, sizeof(kPlainMarker));
	} else if (subWrite && wasCrypted) {
		memcpy(params, oldParams.data(), std::min<size_t>(oldParams.size(), sizeof(params)));
	}
	sfo.SetBinary("SAVEDATA_PARAMS", params, sizeof(params), sizeof(params));
	std::vector<u8> sfoData = sfo.Serialize();
	savefs::WriteAll(sfoPath, sfoData.data(), sfoData.size());

	// The data file. MAKEDATA may come without one.
	if (ValidBuf(param->dataBuf)) {
		std::string fileName = GetFileName(param);
		u32 saveSize = param->dataSize;
		if (saveSize == 0 || saveSize > param->dataBufSize) saveSize = param->dataBufSize;
		StrNCopy(param->saveName, saveDirName, sizeof(param->saveName));
		if (!fileName.empty()) {
			const u8 *p = mem::GetPointer(param->dataBuf, saveSize);
			if (!p && saveSize) return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_BAD_PARAMS;
			LOGI("UTIL", "Saving %u bytes to %s/%s", saveSize, dirPath.c_str(), fileName.c_str());
			if (!savefs::WriteAll(dirPath + "/" + fileName, p, saveSize)) {
				LOGE("UTIL", "Error writing %s/%s", dirPath.c_str(), fileName.c_str());
				return (int)SCE_UTILITY_SAVEDATA_ERROR_SAVE_MS_NOSPACE;
			}
		}
	}

	auto writeSub = [&](const SavedataFileData &d, const std::string &name, bool skipEmpty) {
		if (!ValidBuf(d.buf)) return;
		if (skipEmpty && d.size == 0) return;
		const u8 *p = mem::GetPointer(d.buf, d.size);
		if (!p && d.size) return;
		savefs::WriteAll(dirPath + "/" + name, p, d.size);
	};
	writeSub(param->icon0FileData, ICON0_FILENAME, false);
	writeSub(param->icon1FileData, ICON1_FILENAME, true);
	writeSub(param->pic1FileData, PIC1_FILENAME, false);
	writeSub(param->snd0FileData, SND0_FILENAME, true);
	return 0;
}

int Savedata::Load(SavedataRequest *param, const std::string &saveDirName, int saveId, bool secureMode) {
	(void)saveId;
	if (!param) return (int)SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA;
	bool isRWMode = param->mode == SAVEDATA_READDATA || param->mode == SAVEDATA_READDATASECURE;
	std::string dirPath = GetSaveFilePath(param, GetSaveDir(param, saveDirName));
	std::string fileName = GetFileName(param);
	std::string filePath = dirPath + "/" + fileName;

	if (dirPath.empty() || !savefs::Exists(dirPath))
		return (int)(isRWMode ? SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA : SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA);
	// Without PARAM.SFO the save is broken, whether the data file is there or not
	// (utility/savedata/loadbroken).
	if (!savefs::Exists(dirPath + "/" + SFO_FILENAME))
		return (int)(isRWMode ? SCE_UTILITY_SAVEDATA_ERROR_RW_DATA_BROKEN : SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN);
	if (!fileName.empty() && !savefs::Exists(filePath))
		return (int)(isRWMode ? SCE_UTILITY_SAVEDATA_ERROR_RW_FILE_NOT_FOUND : SCE_UTILITY_SAVEDATA_ERROR_LOAD_FILE_NOT_FOUND);

	// Zeroed before loading, and stays so on errors (but not if the path doesn't exist).
	param->dataSize = 0;
	int result = LoadSaveData(param, saveDirName, dirPath, secureMode);
	if (result != 0) return result;

	if (!LoadSFO(param, dirPath)) {
		LOGW("UTIL", "Load: no PARAM.SFO in %s", dirPath.c_str());
		return (int)(isRWMode ? SCE_UTILITY_SAVEDATA_ERROR_RW_DATA_BROKEN : SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN);
	}
	// What the firmware answers; some games depend on it.
	param->bind = 1021;

	LoadFile(dirPath, ICON0_FILENAME, &param->icon0FileData);
	LoadFile(dirPath, ICON1_FILENAME, &param->icon1FileData);
	LoadFile(dirPath, PIC1_FILENAME, &param->pic1FileData);
	LoadFile(dirPath, SND0_FILENAME, &param->snd0FileData);
	return 0;
}

int Savedata::LoadSaveData(SavedataRequest *param, const std::string &saveDirName, const std::string &dirPath, bool secureMode) {
	if (param->secureVersion > 3) {
		LOGE("UTIL", "Savedata version requested: %u", param->secureVersion);
		return (int)SCE_UTILITY_SAVEDATA_ERROR_LOAD_PARAM;
	} else if (secureMode && MissingRequiredKey(param)) {
		LOGE("UTIL", "Savedata version %u with missing key", param->secureVersion);
		return (int)SCE_UTILITY_SAVEDATA_ERROR_LOAD_PARAM;
	}

	std::string filename = GetFileName(param);
	std::string filePath = dirPath + "/" + filename;
	// A blank file name always succeeds, if secureVersion was right.
	if (filename.empty()) return 0;

	std::vector<u8> saveData;
	if (!savefs::ReadAll(filePath, saveData) || saveData.empty()) {
		LOGE("UTIL", "Error reading %s", filePath.c_str());
		return (int)SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA;
	}
	StrNCopy(param->saveName, saveDirName, sizeof(param->saveName));

	int prevCryptMode = GetSaveCryptMode(param, saveDirName);
	if (prevCryptMode != 0 && !secureMode) {
		// A secure (encrypted) file can't be read without decrypting it (utility/savedata/loaddata).
		std::vector<std::string> secure = GetSecureFileNames(dirPath);
		if (std::find(secure.begin(), secure.end(), filename) != secure.end()) return (int)SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN;
	}
	if (prevCryptMode != 0 && secureMode) {
		const int mode = DetermineCryptMode(param);
		if (mode > 1 && !HasKey(param)) return (int)SCE_UTILITY_SAVEDATA_ERROR_LOAD_PARAM;
		vfs::ParamSfo sfo;
		std::vector<u8> params;
		LoadSfoFile(dirPath + "/" + SFO_FILENAME, sfo);
		sfo.GetBinary("SAVEDATA_PARAMS", params);
		if (!HasPlainMarker(params)) {
			// Encrypted by a PSP (or PPSSPP): there's no KIRK engine to decrypt it with.
			LOGE("UTIL", "%s is encrypted (mode %d); PSPC can't decrypt saves made elsewhere", filePath.c_str(), prevCryptMode);
			return (int)SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN;
		}
		// Decrypting with another mode or key would fail on a PSP.
		u8 expected[16], stored[16]{};
		KeyHash(param, mode, expected);
		std::vector<u8> list;
		if (sfo.GetBinary("SAVEDATA_FILE_LIST", list)) {
			const auto *entries = (const SfoFileListEntry *)list.data();
			for (size_t i = 0; i < list.size() / sizeof(SfoFileListEntry); i++) {
				if (strncmp(entries[i].filename, filename.c_str(), sizeof(entries[i].filename)) == 0) {
					memcpy(stored, entries[i].hash, sizeof(stored));
					break;
				}
			}
		}
		if (mode != prevCryptMode || memcmp(expected, stored, sizeof(stored)) != 0) {
			LOGW("UTIL", "%s: saved with mode %d, loading with mode %d or another key", filePath.c_str(), prevCryptMode, mode);
			return (int)SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN;
		}
	}

	if (ValidBuf(param->dataBuf)) {
		if (saveData.size() > param->dataBufSize || !mem::IsValidRange(param->dataBuf, (u32)saveData.size()))
			return (int)SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN;
		mem::WriteBytes(param->dataBuf, saveData.data(), (u32)saveData.size());
	}
	param->dataSize = (u32)saveData.size();
	return 0;
}

// secureVersion and the key only matter for the full 1536-byte request (the two older sizes have
// neither field); see utility/savedata/secureversion.
bool Savedata::UsesSecureVersion(const SavedataRequest *param) const { return param->common.size >= 1536; }

// secureVersion 0, 2 and 3 need a key; hardware returns SAVE_PARAM for a save without one.
bool Savedata::MissingRequiredKey(const SavedataRequest *param) const {
	return UsesSecureVersion(param) && param->secureVersion != 1 && !HasKey(param);
}

bool Savedata::LoadSFO(SavedataRequest *param, const std::string &dirPath) {
	vfs::ParamSfo sfo;
	if (!LoadSfoFile(dirPath + "/" + SFO_FILENAME, sfo)) return false;
	StrNCopy(param->sfoParam.title, sfo.GetString("TITLE"), 128);
	StrNCopy(param->sfoParam.savedataTitle, sfo.GetString("SAVEDATA_TITLE"), 128);
	StrNCopy(param->sfoParam.detail, sfo.GetString("SAVEDATA_DETAIL"), 1024);
	param->sfoParam.parentalLevel = (u8)sfo.GetInt("PARENTAL_LEVEL");
	return true;
}

std::vector<std::string> Savedata::GetSecureFileNames(const std::string &dirPath) {
	std::vector<std::string> names;
	vfs::ParamSfo sfo;
	if (!LoadSfoFile(dirPath + "/" + SFO_FILENAME, sfo)) return names;
	std::vector<u8> list;
	if (!sfo.GetBinary("SAVEDATA_FILE_LIST", list)) return names;
	u32 count = std::min<u32>(FILE_LIST_COUNT_MAX, (u32)(list.size() / sizeof(SfoFileListEntry)));
	const auto *entries = (const SfoFileListEntry *)list.data();
	for (u32 i = 0; i < count; ++i) {
		if (entries[i].filename[0] != '\0') names.push_back(FixedString(entries[i].filename, sizeof(entries[i].filename)));
	}
	return names;
}

void Savedata::LoadFile(const std::string &dirPath, const std::string &filename, SavedataFileData *fileData) {
	if (!ValidBuf(fileData->buf)) return;
	u32 size = fileData->bufSize;
	// Clamp to valid memory.
	while (size && !mem::IsValidRange(fileData->buf, size)) size >>= 1;
	u8 *dst = mem::GetPointer(fileData->buf, size ? size : 1);
	if (!dst) return;
	s64 got = savefs::ReadInto(dirPath + "/" + filename, dst, size);
	if (got > 0) {
		fileData->size = (u32)got;
		LOGD("UTIL", "Loaded %s/%s (%d bytes) into %08x", dirPath.c_str(), filename.c_str(), (int)got, fileData->buf);
	}
}

std::string Savedata::GetSpaceText(u64 size, bool roundUp) {
	static const char *const suffixes[] = {"B", "KB", "MB", "GB"};
	char text[50];
	for (const char *suffix : suffixes) {
		if (size < 1024) {
			snprintf(text, sizeof(text), "%llu %s", (unsigned long long)size, suffix);
			return text;
		}
		size = roundUp ? (size + 1023) / 1024 : size / 1024;
	}
	snprintf(text, sizeof(text), "%llu TB", (unsigned long long)size);
	return text;
}

int Savedata::GetSizes(SavedataRequest *param) {
	if (!param) return (int)SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_DATA;
	int ret = 0;
	const u32 sector = (u32)kMemoryStickSectorSize;

	if (ValidBuf(param->msFree)) {
		const u64 freeBytes = MemoryStickFreeSpace(GetGameName(param));
		SavedataMsFreeInfo info;
		mem::ReadBytes(&info, param->msFree, sizeof(info));
		info.clusterSize = (s32)sector;
		info.freeClusters = (s32)(freeBytes / sector);
		info.freeSpaceKB = (s32)(freeBytes / 0x400);
		memset(info.freeSpaceStr, 0, sizeof(info.freeSpaceStr));
		StrNCopy(info.freeSpaceStr, GetSpaceText(freeBytes, false), sizeof(info.freeSpaceStr));
		mem::WriteBytes(param->msFree, &info, sizeof(info));
	}
	if (ValidBuf(param->msData)) {
		SavedataMsDataInfo info;
		mem::ReadBytes(&info, param->msData, sizeof(info));
		const std::string gameName = FixedString(info.gameName, sizeof(info.gameName));
		const std::string saveName = FixedString(info.saveName, sizeof(info.saveName));
		std::string path = GetSaveFilePath(param, gameName + (saveName == "<>" ? "" : saveName));
		bool exists = false;
		auto listing = path.empty() ? std::vector<savefs::FileInfo>() : savefs::List(path, &exists);
		if (exists) {
			// The directory takes a cluster too (utility/savedata/sizes).
			info.info.usedClusters = 1;
			for (const auto &item : listing) info.info.usedClusters += (s32)(((u64)item.size + sector - 1) / sector);
			// Based on clusters, not bytes (Fieldrunners expects 736 KB for ~600 KB of files).
			int total = info.info.usedClusters * (int)sector;
			info.info.usedSpaceKB = total / 0x400;
			std::string text = GetSpaceText((u64)total, true);
			StrNCopy(info.info.usedSpaceStr, text, sizeof(info.info.usedSpaceStr));
			info.info.usedSpace32KB = info.info.usedSpaceKB;
			StrNCopy(info.info.usedSpace32Str, text, sizeof(info.info.usedSpace32Str));
		} else {
			info.info.usedClusters = 0;
			info.info.usedSpaceKB = 0;
			memset(info.info.usedSpaceStr, 0, sizeof(info.info.usedSpaceStr));
			info.info.usedSpace32KB = 0;
			memset(info.info.usedSpace32Str, 0, sizeof(info.info.usedSpace32Str));
			ret = (int)SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_DATA;
		}
		// Written later than msFree and utilityData, as on a PSP (utility/savedata/sizes).
		LateWrite(param->msData, &info, sizeof(info));
	}
	if (ValidBuf(param->utilityData)) {
		int total = 0;
		total += SizeNormalized(1);  // the directory record
		total += SizeNormalized(1);  // PARAM.SFO
		// The data itself, if there's a file name, with the encryption overhead a PSP would add.
		if (param->fileName[0] != 0) total += SizeNormalized((u64)param->dataSize + 16);
		total += SizeNormalized(param->icon0FileData.size);
		total += SizeNormalized(param->icon1FileData.size);
		total += SizeNormalized(param->pic1FileData.size);
		total += SizeNormalized(param->snd0FileData.size);

		SavedataUsedDataInfo info;
		mem::ReadBytes(&info, param->utilityData, sizeof(info));
		info.usedClusters = total / (int)sector;
		info.usedSpaceKB = total / 0x400;
		std::string text = GetSpaceText((u64)total, true);
		memset(info.usedSpaceStr, 0, sizeof(info.usedSpaceStr));
		StrNCopy(info.usedSpaceStr, text, sizeof(info.usedSpaceStr));
		info.usedSpace32KB = total / 0x400;
		memset(info.usedSpace32Str, 0, sizeof(info.usedSpace32Str));
		StrNCopy(info.usedSpace32Str, text, sizeof(info.usedSpace32Str));
		mem::WriteBytes(param->utilityData, &info, sizeof(info));
	}
	return ret;
}

bool Savedata::GetList(SavedataRequest *param) {
	if (!param) return false;
	if (!ValidBuf(param->idList)) return true;
	SavedataIdListInfo list;
	mem::ReadBytes(&list, param->idList, sizeof(list));
	u32 maxCount = (u32)list.maxCount;

	std::vector<savefs::FileInfo> validDir;
	std::string searchString = GetGameName(param) + GetSaveName(param);
	for (const auto &d : savefs::List(kSavePath)) {
		if (validDir.size() >= maxCount) break;
		if (PSPMatch(d.name, searchString)) validDir.push_back(d);
	}

	const std::string gameName = GetGameName(param);
	for (u32 i = 0; i < (u32)validDir.size(); i++) {
		u32 addr = list.entries + i * (u32)sizeof(SavedataIdListEntry);
		if (!mem::IsValidRange(addr, sizeof(SavedataIdListEntry))) break;
		SavedataIdListEntry e{};
		e.st_mode = 0x11FF;
		// The dates come from PARAM.SFO.
		savefs::FileInfo sfo = savefs::Stat(kSavePath + validDir[i].name + "/" + SFO_FILENAME);
		const savefs::FileInfo &src = sfo.exists ? sfo : validDir[i];
		e.st_ctime = src.ctime;
		e.st_atime = src.atime;
		e.st_mtime = src.mtime;
		// The folder name without the game name.
		std::string outName = validDir[i].name.substr(std::min(gameName.size(), validDir[i].name.size()));
		StrNCopy(e.name, outName, sizeof(e.name));
		mem::WriteBytes(addr, &e, sizeof(e));
	}
	// The count comes after the entries, as on a PSP (utility/savedata/idlist).
	u32 count = (u32)validDir.size();
	LateWrite(param->idList + offsetof(SavedataIdListInfo, resultCount), &count, 4);
	LOGI("UTIL", "Savedata LIST (%s): %d entries (max %u)", searchString.c_str(), (int)validDir.size(), maxCount);
	return true;
}

int Savedata::GetFilesList(SavedataRequest *param, u32 requestAddr, const std::string &saveDirName) {
	if (!param) return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_BAD_STATUS;
	if (!ValidBuf(param->fileList)) {
		LOGE("UTIL", "Savedata FILES: bad fileList address %08x", param->fileList);
		return -1;
	}
	SavedataFileListInfo fl;
	mem::ReadBytes(&fl, param->fileList, sizeof(fl));
	if (ValidBuf(fl.secureEntries) && fl.maxSecureEntries > 99) return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_BAD_PARAMS;
	if (ValidBuf(fl.normalEntries) && fl.maxNormalEntries > 8192) return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_BAD_PARAMS;
	if (CompiledSdkVersion() >= 0x02060000) {
		if (ValidBuf(fl.systemEntries) && fl.maxSystemEntries > 5) return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_BAD_PARAMS;
	}

	std::string dirPath = kSavePath + GetGameName(param) + GetSaveName(param);
	bool exists = false;
	auto files = savefs::List(dirPath, &exists);
	if (!exists) return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA;

	// PARAM.SFO's SAVEDATA_FILE_LIST says which entries are secure. Without it the counts are left
	// alone, like when the directory is missing (utility/savedata/loadbroken).
	if (!FileFromListing(files, SFO_FILENAME).exists) return (int)SCE_UTILITY_SAVEDATA_ERROR_RW_DATA_BROKEN;

	// Even if there are no files.
	fl.resultNumSecureEntries = 0;
	fl.resultNumNormalEntries = 0;
	fl.resultNumSystemEntries = 0;
	std::vector<std::string> secureNames = GetSecureFileNames(dirPath);

	param->bind = 1021;
	if (mem::IsValidRange(requestAddr, sizeof(SavedataRequest)))
		GWrite32(requestAddr + offsetof(SavedataRequest, bind), 1021);

	// Encrypted secure files carry 16 extra bytes; the ones PSPC stores in the clear don't.
	bool isCrypted = GetSaveCryptMode(param, saveDirName) != 0;
	if (isCrypted) {
		vfs::ParamSfo sfo;
		std::vector<u8> params;
		if (LoadSfoFile(dirPath + "/" + SFO_FILENAME, sfo) && sfo.GetBinary("SAVEDATA_PARAMS", params) && HasPlainMarker(params))
			isCrypted = false;
	}
	for (const auto &file : files) {
		if (file.isDir) continue;
		// Only ALL UPPERCASE names are listed.
		if (file.name.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos) continue;

		bool isSystemFile = file.name == ICON0_FILENAME || file.name == ICON1_FILENAME || file.name == PIC1_FILENAME ||
		                    file.name == SND0_FILENAME || file.name == SFO_FILENAME;
		u32 entryAddr = 0;
		int sizeOffset = 0;
		if (isSystemFile) {
			if (ValidBuf(fl.systemEntries) && fl.resultNumSystemEntries < fl.maxSystemEntries)
				entryAddr = fl.systemEntries + (fl.resultNumSystemEntries++) * (u32)sizeof(SavedataFileListEntry);
		} else if (std::find(secureNames.begin(), secureNames.end(), file.name) != secureNames.end()) {
			if (ValidBuf(fl.secureEntries) && fl.resultNumSecureEntries < fl.maxSecureEntries)
				entryAddr = fl.secureEntries + (fl.resultNumSecureEntries++) * (u32)sizeof(SavedataFileListEntry);
			if (isCrypted) sizeOffset = -0x10;
		} else {
			if (ValidBuf(fl.normalEntries) && fl.resultNumNormalEntries < fl.maxNormalEntries)
				entryAddr = fl.normalEntries + (fl.resultNumNormalEntries++) * (u32)sizeof(SavedataFileListEntry);
		}
		if (!entryAddr || !mem::IsValidRange(entryAddr, sizeof(SavedataFileListEntry))) continue;

		SavedataFileListEntry e;
		mem::ReadBytes(&e, entryAddr, sizeof(e));
		e.st_mode = 0x21FF;
		e.st_size = (u64)(file.size + sizeOffset);
		e.st_ctime = file.ctime;
		e.st_atime = file.atime;
		e.st_mtime = file.mtime;
		memset(e.name, 0, sizeof(e.name));
		memcpy(e.name, file.name.data(), std::min<size_t>(file.name.size(), 15));
		mem::WriteBytes(entryAddr, &e, sizeof(e));
	}
	mem::WriteBytes(param->fileList, &fl, sizeof(fl));
	LOGI("UTIL", "Savedata FILES: %u normal, %u system, %u secure", fl.resultNumNormalEntries, fl.resultNumSystemEntries,
	     fl.resultNumSecureEntries);
	return 0;
}

bool Savedata::GetSize(SavedataRequest *param) {
	if (!param) return false;
	const std::string saveDir = kSavePath + GetGameName(param) + GetSaveName(param);
	bool exists = false;
	if (ValidBuf(param->sizeInfo)) {
		auto listing = savefs::List(saveDir, &exists);
		const u64 freeBytes = MemoryStickFreeSpace(GetGameName(param));
		SavedataSizeInfo si;
		mem::ReadBytes(&si, param->sizeInfo, sizeof(si));

		s64 overwriteBytes = 0, writeBytes = 0;
		for (int i = 0; i < si.numNormalEntries; ++i) {
			u32 a = si.normalEntries + i * (u32)sizeof(SavedataSizeEntry);
			if (!mem::IsValidRange(a, sizeof(SavedataSizeEntry))) break;
			SavedataSizeEntry e;
			mem::ReadBytes(&e, a, sizeof(e));
			overwriteBytes += FileFromListing(listing, FixedString(e.name, sizeof(e.name))).size;
			writeBytes += (s64)e.size;
		}
		for (int i = 0; i < si.numSecureEntries; ++i) {
			u32 a = si.secureEntries + i * (u32)sizeof(SavedataSizeEntry);
			if (!mem::IsValidRange(a, sizeof(SavedataSizeEntry))) break;
			SavedataSizeEntry e;
			mem::ReadBytes(&e, a, sizeof(e));
			overwriteBytes += FileFromListing(listing, FixedString(e.name, sizeof(e.name))).size;
			writeBytes += (s64)e.size + 0x10;
		}

		si.sectorSize = (int)kMemoryStickSectorSize;
		si.freeSectors = (int)(freeBytes / kMemoryStickSectorSize);
		si.freeKB = (int)(freeBytes / 1024);
		TruncateCopy(si.freeString, sizeof(si.freeString), GetSpaceText(freeBytes, false));

		if (writeBytes - overwriteBytes < (s64)freeBytes) {
			si.neededKB = 0;
			si.overwriteKB = 0;
			// The strings are left alone when nothing is needed (utility/savedata/getsize).
		} else {
			s64 neededBytes = writeBytes - (s64)freeBytes;
			si.neededKB = (int)((neededBytes + 1023) / 1024);
			TruncateCopy(si.neededString, sizeof(si.neededString), GetSpaceText((u64)neededBytes, true));
			s64 neededOverwriteBytes = writeBytes - (s64)freeBytes - overwriteBytes;
			si.overwriteKB = (int)((neededOverwriteBytes + 1023) / 1024);
			TruncateCopy(si.overwriteString, sizeof(si.overwriteString), GetSpaceText((u64)neededOverwriteBytes, true));
		}
		mem::WriteBytes(param->sizeInfo, &si, sizeof(si));
	}
	return exists;
}

void Savedata::LateWrite(u32 addr, const void *data, size_t size) {
	lateWrites_.emplace_back(addr, std::vector<u8>((const u8 *)data, (const u8 *)data + size));
}

void Savedata::ApplyLateWrites() {
	for (auto &w : lateWrites_)
		if (mem::IsValidRange(w.first, (u32)w.second.size())) mem::WriteBytes(w.first, w.second.data(), (u32)w.second.size());
	lateWrites_.clear();
}

void Savedata::Clear() {
	saveDataList_.clear();
}

int Savedata::SetPspParam(SavedataRequest *param) {
	pspParam_ = param;
	if (!pspParam_) {
		Clear();
		return 0;
	}

	if (param->mode == SAVEDATA_LISTALLDELETE) {
		Clear();
		int realCount = 0;
		auto allSaves = savefs::List(kSavePath);
		saveDataList_.resize(allSaves.size());
		for (const auto &save : allSaves) {
			if (!save.isDir) continue;
			SaveDirInfo info = GetSaveInfo(kSavePath + save.name);
			SetFileInfo(realCount, info, "", save.name);
			realCount++;
		}
		saveNameListDataCount_ = realCount;
		return 0;
	}

	bool listEmptyFile = !(param->mode == SAVEDATA_LISTLOAD || param->mode == SAVEDATA_LISTDELETE);

	bool hasMultipleFileName = false;
	if (ValidBuf(param->saveNameList) && WouldHaveMultiSaveName(param)) {
		Clear();
		std::vector<std::string> names;
		for (u32 a = param->saveNameList; mem::IsValidRange(a, 20) && GRead8(a) != 0; a += 20) {
			std::string entry = FixedString((const char *)GPtr(a), 20);
			if (HasPathTraversal(entry)) {
				LOGE("UTIL", "Savedata: invalid saveName in list: %s", entry.c_str());
				return (int)SCE_ERROR_UTILITY_INVALID_PARAM_SIZE;
			}
			names.push_back(entry);
		}

		if (!names.empty()) {
			hasMultipleFileName = true;
			saveDataList_.resize(names.size());
			int realCount = 0;
			auto allSaves = savefs::List(kSavePath);
			const std::string gameName = GetGameName(param);

			for (size_t i = 0; i < names.size(); i++) {
				// "<>" means any save name.
				if (names[i] == "<>") {
					for (const auto &it : allSaves) {
						if (it.name.compare(0, gameName.length(), gameName) != 0) continue;
						std::string saveName = it.name.substr(gameName.length());
						if (IsInSaveDataList(saveName, realCount)) continue;
						SaveDirInfo info;
						info.exists = true;
						info.size = it.size;
						info.ctime = it.ctime;
						info.atime = it.atime;
						info.mtime = it.mtime;
						SetFileInfo(realCount, info, saveName);
						++realCount;
						break;
					}
					continue;
				}

				const std::string &thisSaveName = names[i];
				const std::string folderName = gameName + thisSaveName;
				bool found = false;
				for (const auto &s : allSaves)
					if (s.name == folderName) found = true;

				const std::string fileDataDir = kSavePath + folderName;
				if (found) {
					SaveDirInfo info = GetSaveInfo(fileDataDir);
					if (info.exists) {
						SetFileInfo(realCount, info, thisSaveName);
						realCount++;
					} else {
						found = false;
					}
				}
				if (!found && listEmptyFile) {
					ClearFileInfo(saveDataList_[realCount], thisSaveName);
					realCount++;
				}
			}
			saveNameListDataCount_ = realCount;
		}
	}

	// Only this one save.
	if (!hasMultipleFileName) {
		Clear();
		saveDataList_.resize(1);
		std::string fileDataDir = kSavePath + GetGameName(param) + GetSaveName(param);
		SaveDirInfo info = GetSaveInfo(fileDataDir);
		if (info.exists) {
			SetFileInfo(0, info, GetSaveName(param));
			saveNameListDataCount_ = 1;
		} else {
			if (listEmptyFile) ClearFileInfo(saveDataList_[0], GetSaveName(param));
			saveNameListDataCount_ = 0;
		}
	}
	return 0;
}

void Savedata::SetFileInfo(int idx, const SaveDirInfo &info, const std::string &saveName, const std::string &saveDir) {
	if (idx < 0 || idx >= (int)saveDataList_.size()) return;
	SaveFileInfo &s = saveDataList_[idx];
	s = SaveFileInfo();
	s.size = info.size;
	s.saveName = saveName;
	s.idx = idx;
	s.modifTime = info.mtime;
	s.saveDir = saveDir.empty() ? GetGameName(pspParam_) + saveName : saveDir;

	vfs::ParamSfo sfo;
	if (LoadSfoFile(kSavePath + s.saveDir + "/" + SFO_FILENAME, sfo)) {
		TruncateCopy(s.title, sizeof(s.title), sfo.GetString("TITLE"));
		TruncateCopy(s.saveTitle, sizeof(s.saveTitle), sfo.GetString("SAVEDATA_TITLE"));
		TruncateCopy(s.saveDetail, sizeof(s.saveDetail), sfo.GetString("SAVEDATA_DETAIL"));
	} else {
		s.broken = true;
		TruncateCopy(s.title, sizeof(s.title), s.saveDir);
	}
}

void Savedata::ClearFileInfo(SaveFileInfo &saveInfo, const std::string &saveName) {
	saveInfo = SaveFileInfo();
	saveInfo.saveName = saveName;
}

bool Savedata::IsInSaveDataList(const std::string &saveName, int count) const {
	for (int i = 0; i < count && i < (int)saveDataList_.size(); ++i)
		if (saveDataList_[i].saveName == saveName) return true;
	return false;
}

const SaveFileInfo &Savedata::GetFileInfo(int idx) const {
	static const SaveFileInfo empty;
	if (idx < 0 || idx >= (int)saveDataList_.size()) return empty;
	return saveDataList_[idx];
}

std::string Savedata::GetFilename(int idx) const { return GetFileInfo(idx).saveName; }

std::string Savedata::GetSaveDir(int idx) const {
	if (idx < 0 || idx >= (int)saveDataList_.size()) return "";
	return saveDataList_[idx].saveDir;
}

int Savedata::GetSelectedSave() const { return selectedSave_ < saveNameListDataCount_ ? selectedSave_ : 0; }

int Savedata::GetLatestSave() const {
	int idx = 0;
	u64 best = 0;
	for (int i = 0; i < saveNameListDataCount_; ++i) {
		if (saveDataList_[i].size == 0) continue;
		u64 t = DateKey(saveDataList_[i].modifTime);
		if (best < t) {
			idx = i;
			best = t;
		}
	}
	return idx;
}

int Savedata::GetOldestSave() const {
	int idx = 0;
	u64 best = UINT64_MAX;
	for (int i = 0; i < saveNameListDataCount_; ++i) {
		if (saveDataList_[i].size == 0) continue;
		u64 t = DateKey(saveDataList_[i].modifTime);
		if (best > t) {
			idx = i;
			best = t;
		}
	}
	return idx;
}

int Savedata::GetFirstDataSave() const {
	for (int i = 0; i < saveNameListDataCount_; ++i)
		if (saveDataList_[i].size != 0) return i;
	return 0;
}

int Savedata::GetLastDataSave() const {
	for (int i = saveNameListDataCount_ - 1; i >= 0; --i)
		if (saveDataList_[i].size != 0) return i;
	return 0;
}

int Savedata::GetFirstEmptySave() const {
	for (int i = 0; i < saveNameListDataCount_; ++i)
		if (saveDataList_[i].size == 0) return i;
	return 0;
}

int Savedata::GetLastEmptySave() const {
	for (int i = saveNameListDataCount_ - 1; i >= 0; --i)
		if (saveDataList_[i].size == 0) return i;
	return 0;
}

int Savedata::GetSaveNameIndex(const SavedataRequest *param) const {
	std::string saveName = GetSaveName(param);
	for (int i = 0; i < saveNameListDataCount_; i++)
		if (saveDataList_[i].saveName == saveName) return i;
	return 0;
}

bool Savedata::WouldHaveMultiSaveName(const SavedataRequest *param) const {
	switch (param->mode) {
	case SAVEDATA_LOAD:
	case SAVEDATA_AUTOLOAD:
	case SAVEDATA_SAVE:
	case SAVEDATA_AUTOSAVE:
	case SAVEDATA_MAKEDATASECURE:
	case SAVEDATA_MAKEDATA:
	case SAVEDATA_READDATASECURE:
	case SAVEDATA_READDATA:
	case SAVEDATA_WRITEDATASECURE:
	case SAVEDATA_WRITEDATA:
	case SAVEDATA_AUTODELETE:
	case SAVEDATA_DELETE:
	case SAVEDATA_ERASESECURE:
	case SAVEDATA_ERASE:
	case SAVEDATA_DELETEDATA:
		return false;
	default:
		return true;
	}
}

int Savedata::GetSaveCryptMode(const SavedataRequest *param, const std::string &saveDirName) {
	std::string dirPath = GetSaveFilePath(param, GetSaveDir(param, saveDirName));
	vfs::ParamSfo sfo;
	if (dirPath.empty() || !LoadSfoFile(dirPath + "/" + SFO_FILENAME, sfo)) return 0;
	std::vector<u8> params;
	if (!sfo.GetBinary("SAVEDATA_PARAMS", params) || params.empty()) return 0;
	switch (params[0]) {
	case 0: return 0;
	case 0x01: return 1;
	case 0x21: return 3;
	case 0x41: return 5;
	default: return 1;
	}
}

}  // namespace utility
