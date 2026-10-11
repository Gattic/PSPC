// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/FileSystems/DirectoryFileSystem.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
// A PSP volume backed by a host directory: ms0: (memory stick), flash0:, host0:. Win32 only.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fs/filesystem.h"

namespace vfs {

struct HostFsOptions {
	bool readOnly = false;
	// FAT volume semantics (see FS_CAP_FAT); rename keeps the file in its own directory and ignores
	// the directory part of the new name, as the memory stick does. Off gives host0:-style results:
	// 0777 directories, 0664 (0444 read-only) files.
	bool fat = true;
	// Show all-lowercase 8.3 names upper case. The PSP's FAT driver ignores the NT "lowercase" flags,
	// so a "save.dat" written by Windows appears as "SAVE.DAT" on hardware (host0: shows the same
	// in io/directory).
	bool upperCase83 = false;
	// Names in the volume's root that are other host files (lower case, no '/'): pspautotests'
	// host0:/__testoutput.txt goes to a file of each test's own when tests run in parallel.
	std::vector<std::pair<std::string, std::filesystem::path>> redirects;
};

class HostFileSystem : public FileSystem {
public:
	// Creates `root` if needed. Paths never resolve outside it: ".." stops at the root, and names
	// with ':' '\\' '<' '>' '"' '|' '?' '*' or control characters are rejected (EINVAL).
	// Symbolic links or junctions placed inside the root by the user are followed.
	explicit HostFileSystem(std::filesystem::path root, HostFsOptions options = {});
	~HostFileSystem() override;

	const std::filesystem::path &Root() const { return root_; }
	// Overlays another host directory onto the volume: a path missing from the root is looked up
	// there, listings merge (the root's entries first), and a new file goes wherever its folder
	// exists, the root preferred. Not thread-safe against concurrent I/O; add roots at setup.
	void AddFallbackRoot(const std::filesystem::path &dir);
	// Host path of a volume path, existing components matched case-insensitively (the last one
	// may be missing). Returns 0, or an error for invalid names or a missing parent.
	s32 HostPath(std::string_view path, std::filesystem::path *out);

	u32 Caps() const override;
	s32 Open(std::string_view path, u32 flags, s32 mode) override;
	s32 Close(s32 fd) override;
	s64 Read(s32 fd, void *buf, u64 size) override;
	s64 Write(s32 fd, const void *buf, u64 size) override;
	s64 Seek(s32 fd, s64 offset, s32 whence) override;
	s32 GetHandleInfo(s32 fd, HandleInfo *out) override;
	s32 Ioctl(s32 fd, u32 cmd, const void *in, u32 inLen, void *out, u32 outLen) override;
	s32 GetStat(std::string_view path, SceIoStat *out) override;
	s32 ChStat(std::string_view path, const SceIoStat &in, u32 bits) override;
	s32 Remove(std::string_view path) override;
	s32 Mkdir(std::string_view path, s32 mode) override;
	s32 Rmdir(std::string_view path) override;
	s32 Rename(std::string_view from, std::string_view to) override;
	s32 DOpen(std::string_view path) override;
	s32 DRead(s32 dd, SceIoDirent *out) override;
	s32 DClose(s32 dd) override;
	bool QueryFreeSpace(u64 *freeBytes, u64 *totalBytes) override;

private:
	struct OpenFile;
	struct OpenDir;
	struct EntryInfo;

	// Result >= 0: 1 if the path exists, 0 if only its parent does (allowMissingLast).
	s32 Resolve(std::string_view path, bool allowMissingLast, std::wstring *hostPath, std::vector<std::string> *comps);
	s32 ResolveIn(const std::wstring &rootW, std::string_view path, bool allowMissingLast, std::wstring *hostPath,
		std::vector<std::string> *comps);
	void ListInto(const std::wstring &dir, std::vector<EntryInfo> *entries) const;
	void FillStat(const EntryInfo &e, SceIoStat *out) const;
	s32 AllocHandle();
	std::shared_ptr<OpenFile> GetFile(s32 fd);

	std::filesystem::path root_;
	std::wstring rootW_;  // "\\?\" form, no trailing separator
	std::vector<std::wstring> fallbackW_;  // AddFallbackRoot, same form
	HostFsOptions opt_;

	std::mutex mutex_;
	std::map<s32, std::shared_ptr<OpenFile>> files_;
	std::map<s32, std::shared_ptr<OpenDir>> dirs_;
	s32 nextHandle_ = 1;
};

}  // namespace vfs
