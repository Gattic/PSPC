// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/host_file.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
#include <sys/xattr.h>
#endif

namespace hostfile {
#ifdef _WIN32
static s64 UnixTime(FILETIME t) {
	return (s64)(((u64)t.dwHighDateTime << 32) | t.dwLowDateTime) / 10 - 11644473600000000ll;
}
static FILETIME FileTime(s64 t) {
	u64 ticks = (u64)(t + 11644473600000000ll) * 10;
	return {(DWORD)ticks, (DWORD)(ticks >> 32)};
}
#endif
bool Stat(const std::filesystem::path &path, Info &out, std::error_code &ec) {
	out = {};
#ifdef _WIN32
	WIN32_FILE_ATTRIBUTE_DATA d;
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) {
		ec = std::error_code(GetLastError(), std::system_category());
		return false;
	}
	out.directory = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	out.readOnly = (d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
	out.hidden = (d.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
	out.system = (d.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;
	out.size = ((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow;
	out.created = UnixTime(d.ftCreationTime);
	out.accessed = UnixTime(d.ftLastAccessTime);
	out.modified = UnixTime(d.ftLastWriteTime);
#else
	struct stat st;
	if (stat(path.c_str(), &st) != 0) {
		ec = std::error_code(errno, std::generic_category());
		return false;
	}
	out.directory = S_ISDIR(st.st_mode);
	out.readOnly = !(st.st_mode & 0222);
	out.size = st.st_size;
	out.created = (s64)st.st_ctim.tv_sec * 1000000 + st.st_ctim.tv_nsec / 1000;
	out.accessed = (s64)st.st_atim.tv_sec * 1000000 + st.st_atim.tv_nsec / 1000;
	out.modified = (s64)st.st_mtim.tv_sec * 1000000 + st.st_mtim.tv_nsec / 1000;
	// Linux cannot set a birth time. Preserve guest-set FAT creation times as file metadata,
	// so they survive restart, rename and copies performed by the PSP's savedata utility.
	s64 created;
	if (getxattr(path.c_str(), "user.pspc.created", &created, sizeof(created)) == sizeof(created))
		out.created = created;
#endif
	ec.clear();
	return true;
}

bool SetTimes(const std::filesystem::path &path, const s64 *created, const s64 *accessed,
              const s64 *modified, std::error_code &ec) {
#ifdef _WIN32
	HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		ec = std::error_code(GetLastError(), std::system_category());
		return false;
	}
	FILETIME c{}, a{}, m{};
	if (created) c = FileTime(*created);
	if (accessed) a = FileTime(*accessed);
	if (modified) m = FileTime(*modified);
	bool ok = SetFileTime(h, created ? &c : nullptr, accessed ? &a : nullptr, modified ? &m : nullptr);
	if (!ok) ec = std::error_code(GetLastError(), std::system_category());
	CloseHandle(h);
	return ok;
#else
	timespec ts[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
	auto set = [](timespec &t, s64 us) {
		t.tv_sec = us / 1000000;
		t.tv_nsec = (us % 1000000) * 1000;
		if (t.tv_nsec < 0) { --t.tv_sec; t.tv_nsec += 1000000000; }
	};
	if (accessed) set(ts[0], *accessed);
	if (modified) set(ts[1], *modified);
	if ((created && setxattr(path.c_str(), "user.pspc.created", created, sizeof(*created), 0) != 0) ||
	    utimensat(AT_FDCWD, path.c_str(), ts, 0) != 0) {
		ec = std::error_code(errno, std::generic_category());
		return false;
	}
	ec.clear();
	return true;
#endif
}

FILE *Open(const std::filesystem::path &path, bool read, bool write, bool create, bool exclusive) {
	int flags = read && write ? O_RDWR : write ? O_WRONLY : O_RDONLY;
	if (create) flags |= O_CREAT;
	if (exclusive) flags |= O_EXCL;
#ifdef _WIN32
	int fd = -1;
	if (_wsopen_s(&fd, path.c_str(), flags | O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE)) return nullptr;
	FILE *f = _fdopen(fd, read && write ? "r+b" : write ? "wb" : "rb");
	if (!f) _close(fd);
#else
	int fd = open(path.c_str(), flags | O_CLOEXEC, 0666);
	if (fd < 0) return nullptr;
	FILE *f = fdopen(fd, read && write ? "r+b" : write ? "wb" : "rb");
	if (!f) close(fd);
#endif
	// Guest-visible file size and timestamps must agree with concurrent directory/stat calls.
	if (f) setvbuf(f, nullptr, _IONBF, 0);
	return f;
}
s64 Tell(FILE *f) {
#ifdef _WIN32
	return _ftelli64(f);
#else
	return ftello(f);
#endif
}
int Seek(FILE *f, s64 off, int origin) {
#ifdef _WIN32
	return _fseeki64(f, off, origin);
#else
	return fseeko(f, off, origin);
#endif
}
bool Truncate(FILE *f, s64 size) {
#ifdef _WIN32
	return _chsize_s(_fileno(f), size) == 0;
#else
	return ftruncate(fileno(f), size) == 0;
#endif
}
}
