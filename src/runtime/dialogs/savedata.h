// SPDX-License-Identifier: GPL-2.0-or-later
//
// Savedata storage: ms0:/PSP/SAVEDATA/<gameName><saveName>/ with PARAM.SFO, the data file and the
// ICON0/ICON1/PIC1/SND0 files.
// Ported from PPSSPP (Core/Dialog/SavedataParam.cpp/.h, Core/HW/MemoryStick.cpp), Copyright (c)
// 2012- PPSSPP Project, GPL-2.0-or-later.
//
// Secure saves are stored unencrypted: PSPC has no KIRK engine in the runtime. The data file of a
// secure save is still listed in PARAM.SFO's SAVEDATA_FILE_LIST (with a zero hash), and
// SAVEDATA_PARAMS stays zero, which marks the save as not encrypted. Saves encrypted by a real PSP
// can't be decrypted and load as their raw bytes (with a warning).
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "common/common.h"
#include "runtime/dialogs/dialog.h"
#include "fs/psp_io_types.h"

namespace utility {

enum SavedataMode : s32 {
	SAVEDATA_AUTOLOAD = 0,
	SAVEDATA_AUTOSAVE = 1,
	SAVEDATA_LOAD = 2,
	SAVEDATA_SAVE = 3,
	SAVEDATA_LISTLOAD = 4,
	SAVEDATA_LISTSAVE = 5,
	SAVEDATA_LISTDELETE = 6,
	SAVEDATA_LISTALLDELETE = 7,
	SAVEDATA_SIZES = 8,
	SAVEDATA_AUTODELETE = 9,
	SAVEDATA_DELETE = 10,
	SAVEDATA_LIST = 11,
	SAVEDATA_FILES = 12,
	SAVEDATA_MAKEDATASECURE = 13,
	SAVEDATA_MAKEDATA = 14,
	SAVEDATA_READDATASECURE = 15,
	SAVEDATA_READDATA = 16,
	SAVEDATA_WRITEDATASECURE = 17,
	SAVEDATA_WRITEDATA = 18,
	SAVEDATA_ERASESECURE = 19,
	SAVEDATA_ERASE = 20,
	SAVEDATA_DELETEDATA = 21,
	SAVEDATA_GETSIZE = 22,
};

enum SavedataFocus : s32 {
	FOCUS_NAME = 0,
	FOCUS_FIRSTLIST = 1,
	FOCUS_LASTLIST = 2,
	FOCUS_LATEST = 3,
	FOCUS_OLDEST = 4,
	FOCUS_FIRSTDATA = 5,
	FOCUS_LASTDATA = 6,
	FOCUS_FIRSTEMPTY = 7,
	FOCUS_LASTEMPTY = 8,
};

#pragma pack(push, 4)
struct SavedataSfoParam {
	char title[0x80];
	char savedataTitle[0x80];
	char detail[0x400];
	u8 parentalLevel;
	u8 unknown[3];
};
struct SavedataFileData {
	u32 buf;
	u32 bufSize;
	u32 size;
	s32 unknown;
};
struct SavedataSizeEntry {
	u64 size;
	char name[16];
};
struct SavedataSizeInfo {
	s32 numSecureEntries;
	s32 numNormalEntries;
	u32 secureEntries;  // SavedataSizeEntry *
	u32 normalEntries;
	s32 sectorSize;
	s32 freeSectors;
	s32 freeKB;
	char freeString[8];
	s32 neededKB;
	char neededString[8];
	s32 overwriteKB;
	char overwriteString[8];
};
struct SavedataIdListEntry {
	s32 st_mode;
	vfs::ScePspDateTime st_ctime, st_atime, st_mtime;
	char name[20];
};
struct SavedataIdListInfo {
	s32 maxCount;
	s32 resultCount;
	u32 entries;  // SavedataIdListEntry *
};
struct SavedataFileListEntry {
	s32 st_mode;
	u32 st_unk0;
	u64 st_size;
	vfs::ScePspDateTime st_ctime, st_atime, st_mtime;
	char name[16];
};
struct SavedataFileListInfo {
	u32 maxSecureEntries;
	u32 maxNormalEntries;
	u32 maxSystemEntries;
	u32 resultNumSecureEntries;
	u32 resultNumNormalEntries;
	u32 resultNumSystemEntries;
	u32 secureEntries;  // SavedataFileListEntry *
	u32 normalEntries;
	u32 systemEntries;
};
struct SavedataMsFreeInfo {
	s32 clusterSize;
	s32 freeClusters;
	s32 freeSpaceKB;
	char freeSpaceStr[8];
};
struct SavedataUsedDataInfo {
	s32 usedClusters;
	s32 usedSpaceKB;
	char usedSpaceStr[8];
	s32 usedSpace32KB;
	char usedSpace32Str[8];
};
struct SavedataMsDataInfo {
	char gameName[13];
	char pad[3];
	char saveName[20];
	SavedataUsedDataInfo info;
};

// SceUtilitySavedataParam (1480, 1500 or 1536 bytes).
struct SavedataRequest {
	DialogCommon common;
	s32 mode;
	s32 bind;
	s32 overwriteMode;
	char gameName[13];
	char unused[3];
	char saveName[20];
	u32 saveNameList;  // char (*)[20]
	char fileName[13];
	char unused2[3];
	u32 dataBuf;
	u32 dataBufSize;
	u32 dataSize;
	SavedataSfoParam sfoParam;
	SavedataFileData icon0FileData;
	SavedataFileData icon1FileData;
	SavedataFileData pic1FileData;
	SavedataFileData snd0FileData;
	u32 newData;  // SavedataFileData *
	s32 focus;
	s32 abortStatus;
	u32 msFree;       // SavedataMsFreeInfo *
	u32 msData;       // SavedataMsDataInfo *
	u32 utilityData;  // SavedataUsedDataInfo *
	u8 key[16];
	u32 secureVersion;
	s32 multiStatus;
	u32 idList;    // SavedataIdListInfo *
	u32 fileList;  // SavedataFileListInfo *
	u32 sizeInfo;  // SavedataSizeInfo *
};
#pragma pack(pop)
static_assert(sizeof(SavedataSfoParam) == 0x504, "sfo param");
static_assert(sizeof(SavedataIdListEntry) == 72, "id list entry");
static_assert(sizeof(SavedataFileListEntry) == 80, "file list entry");
static_assert(sizeof(SavedataSizeEntry) == 24, "size entry");
static_assert(offsetof(SavedataRequest, newData) == 1476, "savedata V1 layout");
static_assert(offsetof(SavedataRequest, key) == 1500, "savedata V2 layout");
static_assert(sizeof(SavedataRequest) == 1536, "savedata V3 layout");

struct SaveFileInfo {
	s64 size = 0;
	std::string saveName;
	std::string saveDir;
	int idx = 0;
	char title[128]{};
	char saveTitle[128]{};
	char saveDetail[1024]{};
	bool broken = false;
	vfs::ScePspDateTime modifTime{};
};

// A save directory: whether it exists, its total size and the PARAM.SFO (or first file) times.
struct SaveDirInfo {
	bool exists = false;
	s64 size = 0;
	vfs::ScePspDateTime ctime{}, atime{}, mtime{};
};

// Free space reported to games: a 16 GB memory stick holding only this game's saves.
u64 MemoryStickFreeSpace(const std::string &gameName);
constexpr u64 kMemoryStickSectorSize = 32 * 1024;

class Savedata {
public:
	static const char *const kSavePath;  // "ms0:/PSP/SAVEDATA/"

	static void Init();

	std::string GetSaveFilePath(const SavedataRequest *param, const std::string &saveDir) const;
	std::string GetSaveDirName(const SavedataRequest *param, int saveId = -1) const;
	std::string GetSaveDir(const SavedataRequest *param, const std::string &saveDirName) const;
	std::string GetSaveDir(const SavedataRequest *param, int saveId = -1) const;
	bool Delete(SavedataRequest *param, const std::string &saveDir);
	int DeleteData(SavedataRequest *param);
	int Save(SavedataRequest *param, const std::string &saveDirName, bool secureMode = true);
	int Load(SavedataRequest *param, const std::string &saveDirName, int saveId = -1, bool secureMode = true);
	int GetSizes(SavedataRequest *param);
	bool GetList(SavedataRequest *param);
	int GetFilesList(SavedataRequest *param, u32 requestAddr, const std::string &saveDirName);
	bool GetSize(SavedataRequest *param);
	int GetSaveCryptMode(const SavedataRequest *param, const std::string &saveDirName);

	std::string GetGameName(const SavedataRequest *param) const;
	std::string GetSaveName(const SavedataRequest *param) const;
	std::string GetFileName(const SavedataRequest *param) const;
	bool HasKey(const SavedataRequest *param) const;

	static std::string GetSpaceText(u64 size, bool roundUp);

	int SetPspParam(SavedataRequest *param);
	SavedataRequest *GetPspParam() { return pspParam_; }
	const SavedataRequest *GetPspParam() const { return pspParam_; }

	int GetFilenameCount() const { return saveNameListDataCount_; }
	const SaveFileInfo &GetFileInfo(int idx) const;
	std::string GetFilename(int idx) const;
	std::string GetSaveDir(int idx) const;

	int GetSelectedSave() const;
	void SetSelectedSave(int idx) { selectedSave_ = idx; }

	int GetFirstListSave() const { return 0; }
	int GetLastListSave() const { return saveNameListDataCount_ - 1; }
	int GetLatestSave() const;
	int GetOldestSave() const;
	int GetFirstDataSave() const;
	int GetLastDataSave() const;
	int GetFirstEmptySave() const;
	int GetLastEmptySave() const;
	int GetSaveNameIndex(const SavedataRequest *param) const;

	bool WouldHaveMultiSaveName(const SavedataRequest *param) const;

	// Some results reach guest memory one Update later than the rest, as on a PSP.
	void ApplyLateWrites();
	void MoveLateWritesTo(Savedata &other) {
		for (auto &w : lateWrites_) other.lateWrites_.push_back(std::move(w));
		lateWrites_.clear();
	}

private:
	void LateWrite(u32 addr, const void *data, size_t size);
	void Clear();
	void SetFileInfo(int idx, const SaveDirInfo &info, const std::string &saveName, const std::string &saveDir = "");
	void ClearFileInfo(SaveFileInfo &saveInfo, const std::string &saveName);
	bool IsInSaveDataList(const std::string &saveName, int count) const;

	int LoadSaveData(SavedataRequest *param, const std::string &saveDirName, const std::string &dirPath, bool secureMode);
	bool LoadSFO(SavedataRequest *param, const std::string &dirPath);
	void LoadFile(const std::string &dirPath, const std::string &filename, SavedataFileData *fileData);
	bool UsesSecureVersion(const SavedataRequest *param) const;
	bool MissingRequiredKey(const SavedataRequest *param) const;
	std::vector<std::string> GetSecureFileNames(const std::string &dirPath);

	SavedataRequest *pspParam_ = nullptr;
	int selectedSave_ = 0;
	std::vector<SaveFileInfo> saveDataList_;
	int saveNameListDataCount_ = 0;
	std::vector<std::pair<u32, std::vector<u8>>> lateWrites_;
};

}  // namespace utility
