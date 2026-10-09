// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/FileSystems/DirectoryFileSystem.cpp and the stat code of Core/HLE/sceIo.cpp),
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later. Error codes and modes follow pspautotests io/*.
#include "fs/host_fs.h"

#include <algorithm>
#include <cstring>

#include <windows.h>

#include "common/log.h"
#include "fs/fat_names.h"
#include "fs/path_util.h"

namespace vfs {

namespace {

std::wstring Utf8ToWide(std::string_view s) {
	if (s.empty()) {
		return std::wstring();
	}
	int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
	if (n <= 0) {
		// Not UTF-8: take the bytes as Latin-1 rather than failing.
		std::wstring w;
		for (unsigned char c : s) {
			w.push_back((wchar_t)c);
		}
		return w;
	}
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string WideToUtf8(const wchar_t *w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 1) {
		return std::string();
	}
	std::string s((size_t)n - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

bool EqualsNoCaseW(const wchar_t *a, const wchar_t *b) {
	return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

s32 MapWin32Error(DWORD e) {
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:
	case ERROR_BAD_NETPATH:
	case ERROR_BAD_NET_NAME:
		return SCE_ERRNO_ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return SCE_ERRNO_EACCES;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		return SCE_ERRNO_EEXIST;
	case ERROR_DIR_NOT_EMPTY:
		return SCE_ERRNO_ENOTEMPTY;
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:
		return SCE_ERRNO_ENOSPC;
	case ERROR_WRITE_PROTECT:
		return SCE_ERRNO_EROFS;
	case ERROR_INVALID_NAME:
	case ERROR_BAD_PATHNAME:
	case ERROR_DIRECTORY:
		return SCE_ERRNO_EINVAL;
	case ERROR_FILENAME_EXCED_RANGE:
		return SCE_ERRNO_ENAMETOOLONG;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
		return SCE_ERRNO_ENOMEM;
	case ERROR_TOO_MANY_OPEN_FILES:
		return SCE_ERRNO_EMFILE;
	default:
		return SCE_ERRNO_EIO;
	}
}

u64 FileTimeTicks(const FILETIME &ft) {
	return ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// Host times are UTC; the PSP keeps local time, as its FAT driver stores it.
void FileTimeToPsp(const FILETIME &ft, ScePspDateTime *out) {
	memset(out, 0, sizeof(*out));
	const u64 ticks = FileTimeTicks(ft);
	SYSTEMTIME utc, local;
	if (ticks == 0 || !FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
		return;
	}
	out->year = local.wYear;
	out->month = local.wMonth;
	out->day = local.wDay;
	out->hour = local.wHour;
	out->minute = local.wMinute;
	out->second = local.wSecond;
	out->microsecond = (u32)((ticks / 10) % 1000000);
}

bool PspToFileTime(const ScePspDateTime &t, FILETIME *out) {
	SYSTEMTIME local{}, utc;
	local.wYear = t.year;
	local.wMonth = t.month;
	local.wDay = t.day;
	local.wHour = t.hour;
	local.wMinute = t.minute;
	local.wSecond = t.second;
	if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, out)) {
		return false;
	}
	u64 ticks = FileTimeTicks(*out) + (u64)(t.microsecond % 1000000) * 10;
	out->dwLowDateTime = (DWORD)ticks;
	out->dwHighDateTime = (DWORD)(ticks >> 32);
	return true;
}

// Checks one PSP name component and returns its host form. FAT drops trailing dots and spaces.
s32 HostComponent(const std::string &comp, std::wstring *out) {
	std::string name = comp;
	for (unsigned char c : name) {
		if (c < 0x20 || strchr("<>:\"|?*\\", c)) {
			return SCE_ERRNO_EINVAL;
		}
	}
	while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
		name.pop_back();
	}
	if (name.empty()) {
		return SCE_ERRNO_EINVAL;
	}
	*out = Utf8ToWide(name);
	return out->size() > 255 ? SCE_ERRNO_ENAMETOOLONG : 0;
}

bool FindNoCase(const std::wstring &dir, const std::wstring &name, std::wstring *found) {
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	bool ok = false;
	do {
		if (EqualsNoCaseW(fd.cFileName, name.c_str())) {
			*found = fd.cFileName;
			ok = true;
			break;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return ok;
}

// Host names of `dir` in listing order (the order DOpen reports them in), without "." and "..".
std::vector<std::wstring> ListNames(const std::wstring &dir) {
	std::vector<std::wstring> names;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
	if (h == INVALID_HANDLE_VALUE) {
		return names;
	}
	do {
		if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
			names.push_back(fd.cFileName);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return names;
}

// Finds the entry of `dir` whose generated FAT short name is `shortName`.
bool FindShortName(const std::wstring &dir, const std::string &shortName, std::wstring *found) {
	std::vector<std::wstring> wide = ListNames(dir);
	std::vector<std::string> names;
	for (const std::wstring &w : wide) {
		names.push_back(WideToUtf8(w.c_str()));
	}
	std::vector<std::string> shorts = GenerateFatShortNames(names);
	for (size_t i = 0; i < shorts.size(); i++) {
		if (EqualsNoCase(shorts[i], shortName)) {
			*found = wide[i];
			return true;
		}
	}
	return false;
}

// Windows writes lowercase 8.3 names as upper case plus a flag the PSP doesn't read. Applies when
// base and extension fit 8.3 and neither mixes cases.
std::string UpperCase83(const std::string &name) {
	static const char *const kFatChars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&'(){}-_`~";
	auto partOk = [](std::string_view s, size_t maxLen) {
		if (s.size() > maxLen) {
			return false;
		}
		bool lower = false, upper = false;
		for (char c : s) {
			if (!strchr(kFatChars, c) || c == '\0') {
				return false;
			}
			lower = lower || (c >= 'a' && c <= 'z');
			upper = upper || (c >= 'A' && c <= 'Z');
		}
		return !(lower && upper);
	};
	if (name.find_first_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos) {
		return name;
	}
	size_t dot = name.find('.');
	std::string_view base = std::string_view(name).substr(0, dot);
	std::string_view ext = dot == std::string::npos ? std::string_view() : std::string_view(name).substr(dot + 1);
	if (base.empty() || !partOk(base, 8) || !partOk(ext, 3)) {
		return name;
	}
	std::string up = name;
	for (char &c : up) {
		if (c >= 'a' && c <= 'z') c -= 32;
	}
	return up;
}

void CopyName(char (&dst)[256], const std::string &src) {
	size_t len = std::min(src.size(), sizeof(dst) - 1);
	while (len > 0 && len < src.size() && ((unsigned char)src[len] & 0xC0) == 0x80) {
		len--;  // don't cut a UTF-8 sequence in half
	}
	memcpy(dst, src.data(), len);
	dst[len] = '\0';
}

bool HasWildcard(std::string_view s) {
	return s.find_first_of("*?") != std::string_view::npos;
}

}  // namespace

struct HostFileSystem::EntryInfo {
	std::string name;
	DWORD attrs = 0;
	u64 size = 0;
	FILETIME ctime{}, atime{}, mtime{};
};

struct HostFileSystem::OpenFile {
	std::mutex m;
	HANDLE h = INVALID_HANDLE_VALUE;
	u32 flags = 0;
	~OpenFile() {
		if (h != INVALID_HANDLE_VALUE) {
			CloseHandle(h);
		}
	}
};

struct HostFileSystem::OpenDir {
	std::vector<EntryInfo> entries;
	size_t index = 0;
};

HostFileSystem::HostFileSystem(std::filesystem::path root, HostFsOptions options) : opt_(options) {
	std::error_code ec;
	std::filesystem::create_directories(root, ec);
	std::filesystem::path abs = std::filesystem::absolute(root, ec);
	std::filesystem::path canon = std::filesystem::weakly_canonical(abs, ec);
	root_ = ec ? abs : canon;
	std::wstring w = root_.wstring();
	std::replace(w.begin(), w.end(), L'/', L'\\');
	while (w.size() > 3 && w.back() == L'\\') {
		w.pop_back();
	}
	if (w.rfind(L"\\\\?\\", 0) == 0) {
		rootW_ = w;
	} else if (w.rfind(L"\\\\", 0) == 0) {
		rootW_ = L"\\\\?\\UNC\\" + w.substr(2);
	} else {
		rootW_ = L"\\\\?\\" + w;
	}
	if (!rootW_.empty() && rootW_.back() == L'\\') {
		rootW_.pop_back();  // drive root "C:\"
	}
}

HostFileSystem::~HostFileSystem() = default;

u32 HostFileSystem::Caps() const {
	return (opt_.readOnly ? FS_CAP_READ_ONLY : 0) | (opt_.fat ? FS_CAP_FAT : 0) | FS_CAP_CASE_INSENSITIVE;
}

s32 HostFileSystem::Resolve(std::string_view path, bool allowMissingLast, std::wstring *hostPath, std::vector<std::string> *comps) {
	std::vector<std::string> parts = SplitPspPathComponents(path);
	std::wstring cur = rootW_;
	s32 exists = 1;
	for (size_t i = 0; i < parts.size(); i++) {
		std::wstring w;
		s32 err = HostComponent(parts[i], &w);
		if (err < 0) {
			return err;
		}
		const bool last = i + 1 == parts.size();
		std::wstring cand = cur + L"\\" + w;
		DWORD attrs = INVALID_FILE_ATTRIBUTES;
		if (opt_.fat && parts[i].find('~') != std::string::npos) {
			// Our own 8.3 names first: Windows' aliases follow other rules (or are disabled).
			std::wstring found;
			if (FindShortName(cur, parts[i], &found)) {
				cand = cur + L"\\" + found;
				attrs = GetFileAttributesW(cand.c_str());
			}
		}
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			cand = cur + L"\\" + w;
			attrs = GetFileAttributesW(cand.c_str());
		}
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			std::wstring found;
			if (FindNoCase(cur, w, &found)) {
				cand = cur + L"\\" + found;
				attrs = GetFileAttributesW(cand.c_str());
			}
		}
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			if (!last || !allowMissingLast) {
				return SCE_ERRNO_ENOENT;
			}
			exists = 0;
		} else if (!last && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			return SCE_ERRNO_ENOENT;
		}
		cur = std::move(cand);
	}
	*hostPath = std::move(cur);
	if (comps) {
		*comps = std::move(parts);
	}
	return exists;
}

s32 HostFileSystem::HostPath(std::string_view path, std::filesystem::path *out) {
	std::wstring w;
	s32 r = Resolve(path, true, &w, nullptr);
	if (r < 0) {
		return r;
	}
	*out = std::filesystem::path(w);
	return 0;
}

s32 HostFileSystem::AllocHandle() {
	for (;;) {
		s32 h = nextHandle_++;
		if (nextHandle_ <= 0) {
			nextHandle_ = 1;
		}
		if (!files_.count(h) && !dirs_.count(h)) {
			return h;
		}
	}
}

std::shared_ptr<HostFileSystem::OpenFile> HostFileSystem::GetFile(s32 fd) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = files_.find(fd);
	return it == files_.end() ? nullptr : it->second;
}

s32 HostFileSystem::Open(std::string_view path, u32 flags, s32 mode) {
	const bool read = (flags & PSP_O_RDONLY) != 0;
	const bool write = (flags & PSP_O_WRONLY) != 0;
	if (!read && !write) {
		return SCE_ERRNO_EINVAL;
	}
	if (opt_.readOnly && (write || (flags & (PSP_O_CREAT | PSP_O_TRUNC | PSP_O_APPEND)))) {
		return SCE_ERRNO_EROFS;
	}
	std::wstring hp;
	std::vector<std::string> comps;
	s32 exists = Resolve(path, (flags & PSP_O_CREAT) != 0, &hp, &comps);
	if (exists < 0) {
		return exists;
	}
	if (comps.empty()) {
		return SCE_ERRNO_ENOENT;  // the root directory
	}
	if (exists) {
		DWORD attrs = GetFileAttributesW(hp.c_str());
		if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			return SCE_ERRNO_ENOENT;  // io/file: opening a directory fails like a missing file
		}
		if ((flags & PSP_O_CREAT) && (flags & PSP_O_EXCL)) {
			return SCE_ERRNO_EEXIST;
		}
	}
	DWORD desired = (read ? GENERIC_READ : 0) | (write ? GENERIC_WRITE : 0);
	DWORD disposition = OPEN_EXISTING;
	if (flags & PSP_O_CREAT) {
		disposition = (flags & PSP_O_EXCL) ? CREATE_NEW : OPEN_ALWAYS;
	}
	HANDLE h = CreateFileW(hp.c_str(), desired, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		return MapWin32Error(GetLastError());
	}
	auto f = std::make_shared<OpenFile>();
	f->h = h;
	f->flags = flags;
	LARGE_INTEGER zero{};
	if ((flags & PSP_O_TRUNC) && write) {
		SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
		SetEndOfFile(h);
	}
	if (flags & PSP_O_APPEND) {
		SetFilePointerEx(h, zero, nullptr, FILE_END);
	}
	std::lock_guard<std::mutex> lock(mutex_);
	s32 fd = AllocHandle();
	files_[fd] = std::move(f);
	return fd;
}

s32 HostFileSystem::Close(s32 fd) {
	std::lock_guard<std::mutex> lock(mutex_);
	return files_.erase(fd) ? 0 : SCE_KERNEL_ERROR_BADF;
}

s64 HostFileSystem::Read(s32 fd, void *buf, u64 size) {
	auto f = GetFile(fd);
	if (!f || !(f->flags & PSP_O_RDONLY)) {
		return SCE_KERNEL_ERROR_BADF;
	}
	std::lock_guard<std::mutex> lock(f->m);
	u64 total = 0;
	while (total < size) {
		DWORD chunk = (DWORD)std::min<u64>(size - total, 1u << 30);
		DWORD got = 0;
		if (!ReadFile(f->h, (u8 *)buf + total, chunk, &got, nullptr)) {
			return total ? (s64)total : MapWin32Error(GetLastError());
		}
		total += got;
		if (got < chunk) {
			break;
		}
	}
	return (s64)total;
}

s64 HostFileSystem::Write(s32 fd, const void *buf, u64 size) {
	auto f = GetFile(fd);
	if (!f || !(f->flags & PSP_O_WRONLY)) {
		return SCE_KERNEL_ERROR_BADF;
	}
	std::lock_guard<std::mutex> lock(f->m);
	if (f->flags & PSP_O_APPEND) {
		LARGE_INTEGER zero{};
		SetFilePointerEx(f->h, zero, nullptr, FILE_END);
	}
	u64 total = 0;
	while (total < size) {
		DWORD chunk = (DWORD)std::min<u64>(size - total, 1u << 30);
		DWORD wrote = 0;
		if (!WriteFile(f->h, (const u8 *)buf + total, chunk, &wrote, nullptr)) {
			return total ? (s64)total : MapWin32Error(GetLastError());
		}
		total += wrote;
		if (wrote < chunk) {
			break;
		}
	}
	return (s64)total;
}

s64 HostFileSystem::Seek(s32 fd, s64 offset, s32 whence) {
	auto f = GetFile(fd);
	if (!f) {
		return SCE_KERNEL_ERROR_BADF;
	}
	std::lock_guard<std::mutex> lock(f->m);
	LARGE_INTEGER zero{}, pos{}, size{};
	s64 base;
	switch (whence) {
	case PSP_SEEK_SET:
		base = 0;
		break;
	case PSP_SEEK_CUR:
		SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
		base = pos.QuadPart;
		break;
	case PSP_SEEK_END:
		GetFileSizeEx(f->h, &size);
		base = size.QuadPart;
		break;
	default:
		return SCE_KERNEL_ERROR_INVAL;
	}
	if ((offset < 0 && base + offset < 0) || (offset > 0 && base > INT64_MAX - offset)) {
		return -1;
	}
	LARGE_INTEGER target;
	target.QuadPart = base + offset;
	if (!SetFilePointerEx(f->h, target, nullptr, FILE_BEGIN)) {
		return -1;
	}
	return target.QuadPart;
}

s32 HostFileSystem::GetHandleInfo(s32 fd, HandleInfo *out) {
	auto f = GetFile(fd);
	if (!f) {
		return SCE_KERNEL_ERROR_BADF;
	}
	std::lock_guard<std::mutex> lock(f->m);
	LARGE_INTEGER zero{}, pos{}, size{};
	SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
	GetFileSizeEx(f->h, &size);
	*out = HandleInfo();
	out->flags = f->flags;
	out->size = (u64)size.QuadPart;
	out->position = (u64)pos.QuadPart;
	return 0;
}

s32 HostFileSystem::Ioctl(s32 fd, u32 cmd, const void *in, u32 inLen, void *out, u32 outLen) {
	if (!GetFile(fd)) {
		return SCE_KERNEL_ERROR_BADF;
	}
	return SCE_ERRNO_FUNCTION_NOT_SUPPORTED;
}

void HostFileSystem::FillStat(const EntryInfo &e, SceIoStat *out) const {
	const bool dir = (e.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
	const bool ro = (e.attrs & FILE_ATTRIBUTE_READONLY) != 0 && !dir;
	const u32 type = dir ? FIO_S_IFDIR : FIO_S_IFREG;
	if (opt_.fat) {
		// FAT has no permissions: everything is 0777, and clearing the write bits is the
		// read-only attribute (io/stat/readonly).
		out->st_mode = (s32)(type | (ro ? 0555 : 0777));
		out->st_attr = (dir ? FIO_SO_IFDIR : FIO_SO_IFREG) | (ro ? FAT_ATTR_READONLY : 0) |
			((e.attrs & FILE_ATTRIBUTE_HIDDEN) ? FAT_ATTR_HIDDEN : 0) | ((e.attrs & FILE_ATTRIBUTE_SYSTEM) ? FAT_ATTR_SYSTEM : 0);
	} else {
		out->st_mode = (s32)(type | (dir ? 0777 : (ro ? 0444 : 0664)));
		out->st_attr = dir ? FIO_SO_IFDIR : FIO_SO_IFREG;
	}
	out->st_size = dir ? 0 : (s64)e.size;
	FileTimeToPsp(e.ctime, &out->st_ctime);
	FileTimeToPsp(e.atime, &out->st_atime);
	FileTimeToPsp(e.mtime, &out->st_mtime);
}

s32 HostFileSystem::GetStat(std::string_view path, SceIoStat *out) {
	std::wstring hp;
	s32 r = Resolve(path, false, &hp, nullptr);
	if (r < 0) {
		return r;
	}
	WIN32_FILE_ATTRIBUTE_DATA d;
	if (!GetFileAttributesExW(hp.c_str(), GetFileExInfoStandard, &d)) {
		return MapWin32Error(GetLastError());
	}
	EntryInfo e;
	e.attrs = d.dwFileAttributes;
	e.size = ((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow;
	e.ctime = d.ftCreationTime;
	e.atime = d.ftLastAccessTime;
	e.mtime = d.ftLastWriteTime;
	FillStat(e, out);
	return 0;
}

s32 HostFileSystem::ChStat(std::string_view path, const SceIoStat &in, u32 bits) {
	if (opt_.readOnly) {
		return SCE_ERRNO_EROFS;
	}
	std::wstring hp;
	s32 r = Resolve(path, false, &hp, nullptr);
	if (r < 0) {
		return r;
	}
	DWORD attrs = GetFileAttributesW(hp.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES) {
		return MapWin32Error(GetLastError());
	}
	const bool dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
	if (bits & (SCE_CST_CT | SCE_CST_AT | SCE_CST_MT)) {
		FILETIME c, a, m;
		const bool hc = (bits & SCE_CST_CT) && PspToFileTime(in.st_ctime, &c);
		const bool ha = (bits & SCE_CST_AT) && PspToFileTime(in.st_atime, &a);
		const bool hm = (bits & SCE_CST_MT) && PspToFileTime(in.st_mtime, &m);
		HANDLE h = CreateFileW(hp.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			return MapWin32Error(GetLastError());
		}
		BOOL ok = SetFileTime(h, hc ? &c : nullptr, ha ? &a : nullptr, hm ? &m : nullptr);
		CloseHandle(h);
		if (!ok) {
			return SCE_ERRNO_EIO;
		}
	}
	if ((bits & (SCE_CST_MODE | SCE_CST_ATTR)) && !dir) {
		bool writable = true;
		if (bits & SCE_CST_MODE) {
			writable = (in.st_mode & 0222) != 0;
		}
		if (bits & SCE_CST_ATTR) {
			writable = (in.st_attr & FAT_ATTR_READONLY) == 0;  // names the flag directly, so it wins
		}
		DWORD want = writable ? (attrs & ~FILE_ATTRIBUTE_READONLY) : (attrs | FILE_ATTRIBUTE_READONLY);
		if (want != attrs && !SetFileAttributesW(hp.c_str(), want ? want : FILE_ATTRIBUTE_NORMAL)) {
			return MapWin32Error(GetLastError());
		}
	}
	return 0;
}

s32 HostFileSystem::Remove(std::string_view path) {
	if (opt_.readOnly) {
		return SCE_ERRNO_EROFS;
	}
	std::wstring hp;
	std::vector<std::string> comps;
	s32 r = Resolve(path, false, &hp, &comps);
	if (r < 0) {
		return r;
	}
	DWORD attrs = GetFileAttributesW(hp.c_str());
	if (comps.empty() || (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))) {
		return SCE_ERRNO_EISDIR;
	}
	if (!DeleteFileW(hp.c_str())) {
		return MapWin32Error(GetLastError());
	}
	return 0;
}

s32 HostFileSystem::Mkdir(std::string_view path, s32 mode) {
	if (opt_.readOnly) {
		return SCE_ERRNO_EROFS;
	}
	std::wstring hp;
	std::vector<std::string> comps;
	s32 r = Resolve(path, true, &hp, &comps);
	if (r < 0) {
		return r;
	}
	if (r == 1 || comps.empty()) {
		return SCE_ERRNO_EEXIST;
	}
	if (!CreateDirectoryW(hp.c_str(), nullptr)) {
		return MapWin32Error(GetLastError());
	}
	return 0;
}

s32 HostFileSystem::Rmdir(std::string_view path) {
	if (opt_.readOnly) {
		return SCE_ERRNO_EROFS;
	}
	std::wstring hp;
	std::vector<std::string> comps;
	s32 r = Resolve(path, false, &hp, &comps);
	if (r < 0) {
		return r;
	}
	if (comps.empty()) {
		return SCE_ERRNO_EBUSY;  // the volume root
	}
	DWORD attrs = GetFileAttributesW(hp.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		return SCE_ERRNO_ENOTDIR;
	}
	if (!RemoveDirectoryW(hp.c_str())) {
		return MapWin32Error(GetLastError());
	}
	return 0;
}

s32 HostFileSystem::Rename(std::string_view from, std::string_view to) {
	if (opt_.readOnly) {
		return SCE_ERRNO_EROFS;
	}
	// io/file/rename: wildcards are invalid, the source must exist, an existing target (including
	// the source itself) is EEXIST, and on the memory stick the file stays in its directory.
	if (HasWildcard(from) || HasWildcard(to)) {
		return SCE_ERRNO_EINVAL;
	}
	std::vector<std::string> fromComps = SplitPspPathComponents(from);
	std::vector<std::string> toComps = SplitPspPathComponents(to);
	if (fromComps.empty() || toComps.empty()) {
		return SCE_ERRNO_EINVAL;
	}
	if (opt_.fat) {
		std::string name = toComps.back();
		toComps = fromComps;
		toComps.back() = name;
	}
	std::string toPath;
	for (const std::string &c : toComps) {
		toPath += "/" + c;
	}
	std::wstring src, dst;
	s32 r = Resolve(from, false, &src, nullptr);
	if (r < 0) {
		return r;
	}
	r = Resolve(toPath, true, &dst, nullptr);
	if (r < 0) {
		return r;
	}
	if (r == 1) {
		// Names compare case-insensitively, so this includes renaming a file to itself.
		return SCE_ERRNO_EEXIST;
	}
	if (!MoveFileExW(src.c_str(), dst.c_str(), 0)) {
		return MapWin32Error(GetLastError());
	}
	return 0;
}

s32 HostFileSystem::DOpen(std::string_view path) {
	std::wstring hp;
	std::vector<std::string> comps;
	s32 r = Resolve(path, false, &hp, &comps);
	if (r < 0) {
		return r;
	}
	WIN32_FILE_ATTRIBUTE_DATA self;
	if (!GetFileAttributesExW(hp.c_str(), GetFileExInfoStandard, &self)) {
		return MapWin32Error(GetLastError());
	}
	if (!(self.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		return SCE_ERRNO_ENOTDIR;
	}
	auto d = std::make_shared<OpenDir>();
	{
		// Listings start with "." and "..", the root's too (io/io sees "." in flash0:/).
		// io/directory shows ".." with a size of 4096.
		EntryInfo dot;
		dot.name = ".";
		dot.attrs = FILE_ATTRIBUTE_DIRECTORY;
		dot.ctime = self.ftCreationTime;
		dot.atime = self.ftLastAccessTime;
		dot.mtime = self.ftLastWriteTime;
		d->entries.push_back(dot);
		dot.name = "..";
		d->entries.push_back(dot);
	}
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileExW((hp + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) {
				continue;
			}
			EntryInfo e;
			e.name = WideToUtf8(fd.cFileName);
			if (opt_.upperCase83) {
				e.name = UpperCase83(e.name);
			}
			e.attrs = fd.dwFileAttributes;
			e.size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
			e.ctime = fd.ftCreationTime;
			e.atime = fd.ftLastAccessTime;
			e.mtime = fd.ftLastWriteTime;
			d->entries.push_back(std::move(e));
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	std::lock_guard<std::mutex> lock(mutex_);
	s32 dd = AllocHandle();
	dirs_[dd] = std::move(d);
	return dd;
}

s32 HostFileSystem::DRead(s32 dd, SceIoDirent *out) {
	std::shared_ptr<OpenDir> d;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = dirs_.find(dd);
		if (it == dirs_.end()) {
			return SCE_KERNEL_ERROR_BADF;
		}
		d = it->second;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (d->index >= d->entries.size()) {
		out->d_name[0] = '\0';
		return 0;
	}
	const EntryInfo &e = d->entries[d->index++];
	FillStat(e, &out->d_stat);
	if (e.name == "..") {
		out->d_stat.st_size = 4096;
	}
	CopyName(out->d_name, e.name);
	return 1;
}

s32 HostFileSystem::DClose(s32 dd) {
	std::lock_guard<std::mutex> lock(mutex_);
	return dirs_.erase(dd) ? 0 : SCE_KERNEL_ERROR_BADF;
}

bool HostFileSystem::QueryFreeSpace(u64 *freeBytes, u64 *totalBytes) {
	ULARGE_INTEGER avail, total;
	if (!GetDiskFreeSpaceExW((rootW_ + L"\\").c_str(), &avail, &total, nullptr)) {
		return false;
	}
	*freeBytes = avail.QuadPart;
	*totalBytes = total.QuadPart;
	return true;
}

}  // namespace vfs
