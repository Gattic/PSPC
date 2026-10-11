// SPDX-License-Identifier: GPL-2.0-or-later
//
// M33 vshctrl's ISO browsing (docs/m33-behaviors.md section 3), as a file system overlay on ms0:
// while the XMB runs. vshctrl does it by hooking the XMB's sceIo* imports; the effect the XMB sees
// is the same: ms0:/PSP/GAME lists a folder "MMMMMISO<n>" for the n-th image in ms0:/ISO, and that
// folder holds a virtual EBOOT.PBP. Our own code, written from the behaviour description.
#include "runtime/m33/vsh_iso.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "common/fileutil.h"
#include "common/log.h"
#include "fs/vfs.h"
#include "runtime/io/io.h"

namespace m33 {

namespace {

constexpr const char *kGameDir = "/psp/game";
constexpr const char *kVirtualPrefix = "MMMMMISO";
constexpr s32 kFirstHandle = 0x40000000;  // above the host file system's handles

std::string Lower(std::string_view s) {
	std::string r(s);
	for (char &c : r) c = (char)tolower((unsigned char)c);
	return r;
}

std::string Normalize(std::string_view path) {
	std::string p(path);
	std::replace(p.begin(), p.end(), '\\', '/');
	while (p.size() > 1 && p.back() == '/') p.pop_back();
	return p;
}

bool IsImageName(const std::string &name) {
	std::string ext = Lower(std::filesystem::u8path(name).extension().string());
	return ext == ".iso" || ext == ".cso" || ext == ".zso" || ext == ".dax";
}

// "/PSP/GAME/MMMMMISO<n>[/rest]" -> n and the rest ("" or "/EBOOT.PBP"...). -1 if not virtual.
int ParseVirtual(std::string_view path, std::string *rest) {
	std::string p = Normalize(path);
	std::string low = Lower(p);
	std::string prefix = std::string(kGameDir) + "/" + Lower(kVirtualPrefix);
	if (low.rfind(prefix, 0) != 0) return -1;
	size_t i = prefix.size(), start = i;
	while (i < p.size() && isdigit((unsigned char)p[i])) i++;
	if (i == start || (i < p.size() && p[i] != '/')) return -1;
	if (rest) *rest = p.substr(i);
	return atoi(p.substr(start, i - start).c_str());
}

void PutU32(std::vector<u8> &v, size_t at, u32 x) { memcpy(&v[at], &x, 4); }

// "/PSP/GAME/<name>[/rest]" -> name and rest. False for other paths (and /PSP/GAME itself).
bool SplitGamePath(std::string_view path, std::string *name, std::string *rest) {
	std::string p = Normalize(path);
	const std::string dir = std::string(kGameDir) + "/";
	if (Lower(p).rfind(dir, 0) != 0 || p.size() == dir.size()) return false;
	size_t slash = p.find('/', dir.size());
	*name = p.substr(dir.size(), slash == std::string::npos ? std::string::npos : slash - dir.size());
	*rest = slash == std::string::npos ? "" : p.substr(slash);
	return !name->empty();
}

// vshctrl's GAME150 / GAME4XX folders: listing ms0:/PSP/GAME adds their entries with "__150" /
// "__4XX" appended, and "/PSP/GAME/<name>__150/..." is "/PSP/GAME150/<name>/..." (likewise 4XX).
// The 1.50-kernel homebrew of GAME150 then runs in PSPC's one kernel, like any other.
const struct {
	const char *suffix;  // lower case
	const char *folder;
} kSubfolders[] = {{"__150", "/PSP/GAME150"}, {"__4xx", "/PSP/GAME4XX"}};

bool MapSubfolderGame(std::string_view path, std::string *out) {
	std::string name, rest;
	if (!SplitGamePath(path, &name, &rest)) return false;
	const std::string low = Lower(name);
	for (const auto &s : kSubfolders) {
		const size_t n = strlen(s.suffix);
		if (low.size() > n && low.compare(low.size() - n, n, s.suffix) == 0) {
			*out = std::string(s.folder) + "/" + name.substr(0, name.size() - n) + rest;
			return true;
		}
	}
	return false;
}

// An EBOOT.PBP game folder in pspc's Games folder (the PSN / homebrew layout), listed in
// ms0:/PSP/GAME under its folder's name without being on the memory stick.
struct PbpGame {
	std::string name;
	std::filesystem::path dir;
};

// The images the Game menu shows, by index (n of MMMMMISO<n>).
struct Image {
	std::string name;     // file name (in /ISO, or in a games folder)
	std::filesystem::path host;  // the image on the host
	vfs::SceIoStat stat;  // of the image file
	bool built = false;
	bool ok = false;
	std::vector<u8> pbp;  // the virtual EBOOT.PBP
};

class IsoBrowserFs;
IsoBrowserFs *g_browser = nullptr;
// Host folders listed besides ms0:/ISO (pspc's Games folder), searched a few levels deep.
std::vector<std::filesystem::path> g_extraDirs;

vfs::SceIoStat HostFileStat(const std::filesystem::path &p) {
	vfs::SceIoStat st{};
	WIN32_FILE_ATTRIBUTE_DATA d;
	if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &d)) return st;
	st.st_mode = (s32)(vfs::FIO_S_IFREG | 0777);
	st.st_attr = vfs::FIO_SO_IFREG;
	st.st_size = (s64)(((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow);
	auto conv = [](const FILETIME &ft, vfs::ScePspDateTime *out) {
		SYSTEMTIME t;
		FILETIME local;
		if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &t)) return;
		out->year = t.wYear;
		out->month = t.wMonth;
		out->day = t.wDay;
		out->hour = t.wHour;
		out->minute = t.wMinute;
		out->second = t.wSecond;
		out->microsecond = (u32)t.wMilliseconds * 1000;
	};
	conv(d.ftCreationTime, &st.st_ctime);
	conv(d.ftLastAccessTime, &st.st_atime);
	conv(d.ftLastWriteTime, &st.st_mtime);
	return st;
}

class IsoBrowserFs : public vfs::FileSystem {
public:
	explicit IsoBrowserFs(vfs::HostFileSystem *inner) : inner_(inner) {}
	~IsoBrowserFs() override {
		if (g_browser == this) g_browser = nullptr;
	}

	// The image behind index n, as a host path ("" if none).
	std::string HostPathOf(int n) {
		std::lock_guard<std::mutex> lock(mu_);
		if (n < 0 || n >= (int)images_.size()) return "";
		return PathToUtf8(images_[n].host);
	}

	u32 Caps() const override { return inner_->Caps(); }

	s32 Open(std::string_view path, u32 flags, s32 mode) override {
		std::string rest, mapped;
		int n = ParseVirtual(path, &rest);
		if (n < 0) {
			if (MapSubfolderGame(path, &mapped)) return inner_->Open(mapped, flags, mode);
			std::filesystem::path host;
			int r = PbpHostPath(path, &host);
			if (r == 0) return inner_->Open(path, flags, mode);
			if (r < 0) return r;
			if ((flags & vfs::PSP_O_WRONLY) || (flags & (vfs::PSP_O_CREAT | vfs::PSP_O_TRUNC))) return vfs::SCE_ERRNO_EACCES;
			HANDLE h = CreateFileW(host.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) return vfs::SCE_ERRNO_ENOENT;
			std::lock_guard<std::mutex> lock(mu_);
			s32 fd = next_++;
			files_[fd] = VFile{-1, 0, flags, h};
			return fd;
		}
		std::lock_guard<std::mutex> lock(mu_);
		Image *img = Built(n);
		if (!img) return vfs::SCE_ERRNO_ENOENT;
		if (Lower(rest) != "/eboot.pbp") return rest.empty() ? vfs::SCE_ERRNO_EISDIR : vfs::SCE_ERRNO_ENOENT;
		if ((flags & vfs::PSP_O_WRONLY) || (flags & (vfs::PSP_O_CREAT | vfs::PSP_O_TRUNC))) return vfs::SCE_ERRNO_EACCES;
		s32 h = next_++;
		files_[h] = VFile{n, 0, flags};
		return h;
	}

	s32 Close(s32 fd) override {
		if (fd < kFirstHandle) return inner_->Close(fd);
		std::lock_guard<std::mutex> lock(mu_);
		auto it = files_.find(fd);
		if (it == files_.end()) return vfs::SCE_KERNEL_ERROR_BADF;
		if (it->second.host) CloseHandle(it->second.host);
		files_.erase(it);
		return 0;
	}

	s64 Read(s32 fd, void *buf, u64 size) override {
		if (fd < kFirstHandle) return inner_->Read(fd, buf, size);
		std::lock_guard<std::mutex> lock(mu_);
		auto it = files_.find(fd);
		if (it == files_.end()) return vfs::SCE_KERNEL_ERROR_BADF;
		if (it->second.host) {
			DWORD got = 0;
			if (!ReadFile(it->second.host, buf, (DWORD)std::min<u64>(size, 0x7FFFFFFF), &got, nullptr)) return vfs::SCE_ERRNO_EIO;
			return (s64)got;
		}
		const std::vector<u8> &pbp = images_[it->second.image].pbp;
		u64 pos = it->second.pos;
		u64 n = pos >= pbp.size() ? 0 : std::min<u64>(size, pbp.size() - pos);
		if (n) memcpy(buf, pbp.data() + pos, (size_t)n);
		it->second.pos += n;
		return (s64)n;
	}

	s64 Write(s32 fd, const void *buf, u64 size) override {
		if (fd < kFirstHandle) return inner_->Write(fd, buf, size);
		return vfs::SCE_KERNEL_ERROR_BADF;
	}

	s64 Seek(s32 fd, s64 offset, s32 whence) override {
		if (fd < kFirstHandle) return inner_->Seek(fd, offset, whence);
		std::lock_guard<std::mutex> lock(mu_);
		auto it = files_.find(fd);
		if (it == files_.end()) return vfs::SCE_KERNEL_ERROR_BADF;
		if (it->second.host) {
			LARGE_INTEGER to, now;
			to.QuadPart = offset;
			if (!SetFilePointerEx(it->second.host, to, &now, whence == 1 ? FILE_CURRENT : whence == 2 ? FILE_END : FILE_BEGIN))
				return -1;
			return now.QuadPart;
		}
		s64 base = whence == 1 ? (s64)it->second.pos : whence == 2 ? (s64)images_[it->second.image].pbp.size() : 0;
		if (base + offset < 0) return -1;
		it->second.pos = (u64)(base + offset);
		return (s64)it->second.pos;
	}

	s32 GetHandleInfo(s32 fd, vfs::HandleInfo *out) override {
		if (fd < kFirstHandle) return inner_->GetHandleInfo(fd, out);
		std::lock_guard<std::mutex> lock(mu_);
		auto it = files_.find(fd);
		if (it == files_.end()) return vfs::SCE_KERNEL_ERROR_BADF;
		*out = vfs::HandleInfo();
		out->flags = it->second.flags;
		if (it->second.host) {
			LARGE_INTEGER size{}, zero{}, pos{};
			GetFileSizeEx(it->second.host, &size);
			SetFilePointerEx(it->second.host, zero, &pos, FILE_CURRENT);
			out->size = (u64)size.QuadPart;
			out->position = (u64)pos.QuadPart;
			return 0;
		}
		out->size = images_[it->second.image].pbp.size();
		out->position = it->second.pos;
		return 0;
	}

	s32 Ioctl(s32 fd, u32 cmd, const void *in, u32 inLen, void *out, u32 outLen) override {
		if (fd < kFirstHandle) return inner_->Ioctl(fd, cmd, in, inLen, out, outLen);
		return vfs::SCE_ERRNO_FUNCTION_NOT_SUPPORTED;
	}

	s32 GetStat(std::string_view path, vfs::SceIoStat *out) override {
		std::string rest, mapped;
		int n = ParseVirtual(path, &rest);
		if (n < 0) {
			if (MapSubfolderGame(path, &mapped)) return inner_->GetStat(mapped, out);
			std::filesystem::path host;
			int r = PbpHostPath(path, &host);
			if (r == 0) return inner_->GetStat(path, out);
			if (r < 0) return r;
			std::error_code ec;
			if (!std::filesystem::exists(host, ec)) return vfs::SCE_ERRNO_ENOENT;
			*out = HostFileStat(host);
			if (std::filesystem::is_directory(host, ec)) {
				out->st_mode = (s32)(vfs::FIO_S_IFDIR | 0777);
				out->st_attr = vfs::FIO_SO_IFDIR;
				out->st_size = 0;
			}
			return 0;
		}
		std::lock_guard<std::mutex> lock(mu_);
		if (n >= (int)images_.size()) RefreshImages();
		if (n >= (int)images_.size()) return vfs::SCE_ERRNO_ENOENT;
		if (rest.empty()) {
			*out = DirStat(images_[n]);
			return 0;
		}
		Image *img = Built(n);
		if (!img || Lower(rest) != "/eboot.pbp") return vfs::SCE_ERRNO_ENOENT;
		*out = FileStat(*img);
		return 0;
	}

	s32 ChStat(std::string_view path, const vfs::SceIoStat &in, u32 bits) override {
		std::string mapped;
		if (ParseVirtual(path, nullptr) >= 0) return 0;
		if (MapSubfolderGame(path, &mapped)) return inner_->ChStat(mapped, in, bits);
		std::filesystem::path host;
		if (PbpHostPath(path, &host) > 0) return 0;
		return inner_->ChStat(path, in, bits);
	}
	// Deleting an image's entry (or a game folder from pspc's Games folder) from the XMB would
	// delete the user's files there: refused.
	s32 Remove(std::string_view path) override {
		std::string mapped;
		if (ParseVirtual(path, nullptr) >= 0) return vfs::SCE_ERRNO_EACCES;
		if (MapSubfolderGame(path, &mapped)) return inner_->Remove(mapped);
		std::filesystem::path host;
		if (PbpHostPath(path, &host) > 0) return vfs::SCE_ERRNO_EACCES;
		return inner_->Remove(path);
	}
	s32 Mkdir(std::string_view path, s32 mode) override {
		std::string mapped;
		if (ParseVirtual(path, nullptr) >= 0) return vfs::SCE_ERRNO_EEXIST;
		if (MapSubfolderGame(path, &mapped)) return inner_->Mkdir(mapped, mode);
		std::filesystem::path host;
		if (PbpHostPath(path, &host) > 0) return vfs::SCE_ERRNO_EACCES;
		s32 r = inner_->Mkdir(path, mode);
		// vshctrl creates GAME150 and GAME4XX along with GAME.
		if (r >= 0 && Lower(Normalize(path)) == kGameDir)
			for (const auto &s : kSubfolders) inner_->Mkdir(s.folder, mode);
		return r;
	}
	s32 Rmdir(std::string_view path) override {
		std::string mapped;
		if (ParseVirtual(path, nullptr) >= 0) return vfs::SCE_ERRNO_EACCES;
		if (MapSubfolderGame(path, &mapped)) return inner_->Rmdir(mapped);
		std::filesystem::path host;
		if (PbpHostPath(path, &host) > 0) return vfs::SCE_ERRNO_EACCES;
		return inner_->Rmdir(path);
	}
	s32 Rename(std::string_view from, std::string_view to) override {
		if (ParseVirtual(from, nullptr) >= 0 || ParseVirtual(to, nullptr) >= 0) return vfs::SCE_ERRNO_EACCES;
		std::string mf(from), mt(to);
		MapSubfolderGame(from, &mf);
		MapSubfolderGame(to, &mt);
		std::filesystem::path host;
		if (PbpHostPath(mf, &host) > 0 || PbpHostPath(mt, &host) > 0) return vfs::SCE_ERRNO_EACCES;
		return inner_->Rename(mf, mt);
	}

	s32 DOpen(std::string_view path) override {
		std::string rest, mapped;
		int n = ParseVirtual(path, &rest);
		if (n < 0 && MapSubfolderGame(path, &mapped)) return inner_->DOpen(mapped);
		if (n < 0) {
			std::filesystem::path host;
			int r = PbpHostPath(path, &host);
			if (r < 0) return r;
			if (r > 0) {
				// A game folder of pspc's Games folder: its files, read-only.
				std::error_code ec;
				if (!std::filesystem::is_directory(host, ec)) return vfs::SCE_ERRNO_ENOTDIR;
				VDir d;
				vfs::SceIoStat self = HostFileStat(host);
				self.st_mode = (s32)(vfs::FIO_S_IFDIR | 0777);
				self.st_attr = vfs::FIO_SO_IFDIR;
				self.st_size = 0;
				d.entries.push_back(Entry(".", self));
				d.entries.push_back(Entry("..", self));
				for (auto it = std::filesystem::directory_iterator(host, ec); !ec && it != std::filesystem::directory_iterator();
				     it.increment(ec)) {
					vfs::SceIoStat st = HostFileStat(it->path());
					if (it->is_directory(ec)) {
						st.st_mode = (s32)(vfs::FIO_S_IFDIR | 0777);
						st.st_attr = vfs::FIO_SO_IFDIR;
						st.st_size = 0;
					}
					d.entries.push_back(Entry(PathToUtf8(it->path().filename()), st));
				}
				std::lock_guard<std::mutex> lock(mu_);
				s32 h = next_++;
				dirs_[h] = std::move(d);
				return h;
			}
		}
		std::lock_guard<std::mutex> lock(mu_);
		if (n >= 0) {
			if (!rest.empty()) return vfs::SCE_ERRNO_ENOENT;
			Image *img = Built(n);
			if (!img) return vfs::SCE_ERRNO_ENOENT;
			VDir d;
			d.entries.push_back(Entry(".", DirStat(*img)));
			d.entries.push_back(Entry("..", DirStat(*img)));
			d.entries.push_back(Entry("EBOOT.PBP", FileStat(*img)));
			s32 h = next_++;
			dirs_[h] = std::move(d);
			return h;
		}
		if (Lower(Normalize(path)) != kGameDir) return inner_->DOpen(path);
		// The game folder: the real entries, those of GAME150 and GAME4XX, the game folders of
		// pspc's Games folder, then one folder per image (vshctrl's order).
		s32 inner = inner_->DOpen(path);
		if (inner < 0) return inner;
		VDir d;
		vfs::SceIoDirent e;
		std::vector<std::string> names;
		for (;;) {
			memset(&e, 0, sizeof(e));
			if (inner_->DRead(inner, &e) <= 0) break;
			d.entries.push_back(e);
			names.push_back(Lower(e.d_name));
		}
		inner_->DClose(inner);
		for (const auto &s : kSubfolders) {
			s32 sub = inner_->DOpen(s.folder);
			if (sub < 0) continue;
			for (;;) {
				memset(&e, 0, sizeof(e));
				if (inner_->DRead(sub, &e) <= 0) break;
				if (!strcmp(e.d_name, ".") || !strcmp(e.d_name, "..")) continue;
				std::string name = std::string(e.d_name) + (s.suffix[2] == '4' ? "__4XX" : "__150");
				strncpy(e.d_name, name.c_str(), sizeof(e.d_name) - 1);
				d.entries.push_back(e);
			}
			inner_->DClose(sub);
		}
		RefreshImages();
		for (const PbpGame &g : pbpGames_) {
			if (std::find(names.begin(), names.end(), Lower(g.name)) != names.end()) continue;  // the stick's own
			vfs::SceIoStat st = HostFileStat(g.dir);
			st.st_mode = (s32)(vfs::FIO_S_IFDIR | 0777);
			st.st_attr = vfs::FIO_SO_IFDIR;
			st.st_size = 0;
			d.entries.push_back(Entry(g.name, st));
		}
		for (size_t i = 0; i < images_.size(); i++) {
			d.entries.push_back(Entry(std::string(kVirtualPrefix) + std::to_string(i), DirStat(images_[i])));
		}
		s32 h = next_++;
		dirs_[h] = std::move(d);
		return h;
	}

	s32 DRead(s32 dd, vfs::SceIoDirent *out) override {
		if (dd < kFirstHandle) return inner_->DRead(dd, out);
		std::lock_guard<std::mutex> lock(mu_);
		auto it = dirs_.find(dd);
		if (it == dirs_.end()) return vfs::SCE_KERNEL_ERROR_BADF;
		if (it->second.next >= it->second.entries.size()) {
			out->d_name[0] = 0;
			return 0;
		}
		const vfs::SceIoDirent &e = it->second.entries[it->second.next++];
		out->d_stat = e.d_stat;
		memcpy(out->d_name, e.d_name, sizeof(out->d_name));
		return 1;
	}

	s32 DClose(s32 dd) override {
		if (dd < kFirstHandle) return inner_->DClose(dd);
		std::lock_guard<std::mutex> lock(mu_);
		return dirs_.erase(dd) ? 0 : vfs::SCE_KERNEL_ERROR_BADF;
	}

	// The host file a GAME150/GAME4XX or Games folder game's path stands for ("" if none).
	std::string GameHostPath(std::string_view path) {
		std::string mapped;
		std::filesystem::path host;
		if (MapSubfolderGame(path, &mapped)) return inner_->HostPath(mapped, &host) >= 0 ? PathToUtf8(host) : "";
		return PbpHostPath(path, &host) > 0 ? PathToUtf8(host) : "";
	}

	// "/PSP/GAME/<name>[/rest]" of a game folder in pspc's Games folder: 1 and its host path,
	// 0 if the path isn't one (the memory stick's own folder of that name wins), or an error.
	int PbpHostPath(std::string_view path, std::filesystem::path *host) {
		std::string name, rest;
		if (!SplitGamePath(path, &name, &rest) || ParseVirtual(path, nullptr) >= 0) return 0;
		vfs::SceIoStat st;
		if (inner_->GetStat(std::string("/PSP/GAME/") + name, &st) >= 0) return 0;
		std::lock_guard<std::mutex> lock(mu_);
		const PbpGame *g = FindPbp(name);
		if (!g) {
			RefreshImages();
			g = FindPbp(name);
		}
		if (!g) return 0;
		std::string rel = rest.empty() ? "" : rest.substr(1);
		if (rel.find("..") != std::string::npos || rel.find(':') != std::string::npos) return vfs::SCE_ERRNO_ENOENT;
		*host = rel.empty() ? g->dir : g->dir / std::filesystem::u8path(rel);
		return 1;
	}


	bool QueryFreeSpace(u64 *freeBytes, u64 *totalBytes) override { return inner_->QueryFreeSpace(freeBytes, totalBytes); }

private:
	struct VFile {
		int image;  // -1: a file of a Games folder game (host)
		u64 pos;
		u32 flags;
		HANDLE host = nullptr;
	};

	const PbpGame *FindPbp(const std::string &name) const {
		const std::string low = Lower(name);
		for (const PbpGame &g : pbpGames_)
			if (Lower(g.name) == low) return &g;
		return nullptr;
	}
	struct VDir {
		std::vector<vfs::SceIoDirent> entries;
		size_t next = 0;
	};

	static vfs::SceIoDirent Entry(const std::string &name, const vfs::SceIoStat &st) {
		vfs::SceIoDirent e;
		memset(&e, 0, sizeof(e));
		e.d_stat = st;
		strncpy(e.d_name, name.c_str(), sizeof(e.d_name) - 1);
		return e;
	}

	static vfs::SceIoStat DirStat(const Image &img) {
		vfs::SceIoStat st = img.stat;
		st.st_mode = (s32)(vfs::FIO_S_IFDIR | 0777);
		st.st_attr = vfs::FIO_SO_IFDIR;
		st.st_size = 0;
		return st;
	}

	static vfs::SceIoStat FileStat(const Image &img) {
		vfs::SceIoStat st = img.stat;
		st.st_mode = (s32)(vfs::FIO_S_IFREG | 0777);
		st.st_attr = vfs::FIO_SO_IFREG;
		st.st_size = (s64)img.pbp.size();
		return st;
	}

	// Re-reads /ISO (images keep their index while the folder doesn't change).
	void RefreshImages() {
		std::vector<Image> found;
		s32 d = inner_->DOpen("/ISO");
		if (d >= 0) {
			vfs::SceIoDirent e;
			for (;;) {
				memset(&e, 0, sizeof(e));
				if (inner_->DRead(d, &e) <= 0) break;
				if ((e.d_stat.st_mode & vfs::FIO_S_IFDIR) || !IsImageName(e.d_name)) continue;
				Image img;
				img.name = e.d_name;
				img.stat = e.d_stat;
				if (inner_->HostPath("/ISO/" + img.name, &img.host) < 0) continue;
				found.push_back(std::move(img));
			}
			inner_->DClose(d);
		}
		// pspc's Games folder too, so the XMB shows what the launcher shows without copying: its
		// UMD images, and its EBOOT.PBP game folders (Games\<name>\EBOOT.PBP).
		std::vector<PbpGame> pbps;
		for (const std::filesystem::path &dir : g_extraDirs) {
			std::error_code ec;
			auto opts = std::filesystem::directory_options::skip_permission_denied;
			for (auto it = std::filesystem::recursive_directory_iterator(dir, opts, ec);
			     !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
				if (it.depth() > 3 || !it->is_regular_file(ec)) continue;
				std::string name = PathToUtf8(it->path().filename());
				if (Lower(name) == "eboot.pbp" && it.depth() > 0) {
					PbpGame g;
					g.dir = it->path().parent_path();
					g.name = PathToUtf8(g.dir.filename());
					bool dup = g.name.empty() || ParseVirtual("/PSP/GAME/" + g.name, nullptr) >= 0;
					std::string mapped;
					dup |= MapSubfolderGame("/PSP/GAME/" + g.name, &mapped);
					for (const PbpGame &o : pbps) dup |= Lower(o.name) == Lower(g.name);
					if (!dup) pbps.push_back(std::move(g));
					continue;
				}
				if (!IsImageName(name)) continue;
				bool dup = false;
				for (const Image &f : found)
					dup |= std::filesystem::equivalent(f.host, it->path(), ec) && !ec;
				if (dup) continue;
				Image img;
				img.name = name;
				img.host = it->path();
				img.stat = HostFileStat(img.host);
				found.push_back(std::move(img));
			}
		}
		for (Image &f : found) {
			for (Image &old : images_) {
				if (old.host == f.host && old.stat.st_size == f.stat.st_size && old.built) {
					f.built = true;
					f.ok = old.ok;
					f.pbp = std::move(old.pbp);
				}
			}
		}
		images_ = std::move(found);
		pbpGames_ = std::move(pbps);
	}

	Image *Built(int n) {
		if (n >= (int)images_.size()) RefreshImages();
		if (n < 0 || n >= (int)images_.size()) return nullptr;
		Image &img = images_[n];
		if (!img.built) {
			img.built = true;
			img.ok = BuildPbp(img);
		}
		return img.ok ? &img : nullptr;
	}

	// The virtual EBOOT.PBP: the image's PARAM.SFO as a memory stick game (CATEGORY "MG", as
	// vshctrl's template), its icons, pictures and sound; no DATA.PSP (launching it boots the image).
	bool BuildPbp(Image &img) {
		const std::filesystem::path &host = img.host;
		std::string err;
		std::unique_ptr<vfs::BlockDevice> bd = vfs::OpenBlockDevice(host, &err);
		if (!bd) {
			LOGW("M33", "ISO %s: %s", img.name.c_str(), err.c_str());
			return false;
		}
		vfs::IsoFileSystem iso(std::shared_ptr<vfs::BlockDevice>(std::move(bd)));
		std::vector<u8> sfoData;
		vfs::ParamSfo sfo;
		if (!iso.IsValid() || iso.ReadWholeFile("/PSP_GAME/PARAM.SFO", sfoData, vfs::kMaxMetadataBytes) != 0 || !sfo.Parse(sfoData)) {
			LOGW("M33", "ISO %s: no /PSP_GAME/PARAM.SFO", img.name.c_str());
			return false;
		}
		sfo.SetString("CATEGORY", "MG");
		std::vector<std::vector<u8>> parts(8);
		parts[0] = sfo.Serialize();
		const char *const files[] = {"/PSP_GAME/ICON0.PNG", "/PSP_GAME/ICON1.PMF", "/PSP_GAME/PIC0.PNG",
		                             "/PSP_GAME/PIC1.PNG", "/PSP_GAME/SND0.AT3"};
		for (int i = 0; i < 5; i++) {
			if (iso.ReadWholeFile(files[i], parts[1 + i], vfs::kMaxMetadataBytes) != 0) parts[1 + i].clear();
		}
		std::vector<u8> &pbp = img.pbp;
		pbp.assign(0x28, 0);
		memcpy(pbp.data(), "\0PBP", 4);
		PutU32(pbp, 4, 0x00010000);
		for (int i = 0; i < 8; i++) {
			PutU32(pbp, 8 + 4 * i, (u32)pbp.size());
			pbp.insert(pbp.end(), parts[i].begin(), parts[i].end());
		}
		LOGI("M33", "ISO %s: %s (%s)", img.name.c_str(), sfo.GetString("TITLE").c_str(), sfo.GetString("DISC_ID").c_str());
		return true;
	}

	vfs::HostFileSystem *inner_;
	std::mutex mu_;
	std::vector<Image> images_;
	std::vector<PbpGame> pbpGames_;
	std::map<s32, VFile> files_;
	std::map<s32, VDir> dirs_;
	s32 next_ = kFirstHandle;
};

}  // namespace

void InstallIsoBrowser(const std::vector<std::string> &extraDirs) {
	g_extraDirs.clear();
	for (const std::string &d : extraDirs)
		if (!d.empty()) g_extraDirs.push_back(fs::u8path(d));
	io::SetFsOverlay("ms0", [](vfs::HostFileSystem *host) {
		auto fs = std::make_shared<IsoBrowserFs>(host);
		g_browser = fs.get();
		return fs;
	});
}

bool IsoForVirtualGamePath(const std::string &pspPath, std::string *hostPath) {
	std::string device, path;
	if (!g_browser || !io::ResolvePath(pspPath, device, path) || device != "ms0") return false;
	int n = ParseVirtual(path, nullptr);
	if (n < 0) return false;
	*hostPath = g_browser->HostPathOf(n);
	return !hostPath->empty();
}

bool HostPathForVirtualGame(const std::string &pspPath, std::string *hostPath) {
	std::string device, path;
	if (!g_browser || !io::ResolvePath(pspPath, device, path) || device != "ms0") return false;
	*hostPath = g_browser->GameHostPath(path);
	return !hostPath->empty();
}

}  // namespace m33
