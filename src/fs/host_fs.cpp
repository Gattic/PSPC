// SPDX-License-Identifier: GPL-2.0-or-later
// FAT behaviour ported from PPSSPP (Core/FileSystems/DirectoryFileSystem.cpp and Core/HLE/sceIo.cpp),
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later. Host I/O uses the same PSP rules on all hosts.
#include "fs/host_fs.h"
#include <algorithm>
#include <cerrno>
#include <ctime>
#include "common/fileutil.h"
#include "common/host_file.h"
#include "fs/fat_names.h"
#include "fs/path_util.h"

namespace vfs {
namespace {
s32 MapError(std::error_code ec) {
	using E = std::errc;
	if (ec == E::no_such_file_or_directory) return SCE_ERRNO_ENOENT;
	if (ec == E::permission_denied || ec == E::operation_not_permitted) return SCE_ERRNO_EACCES;
	if (ec == E::file_exists) return SCE_ERRNO_EEXIST;
	if (ec == E::directory_not_empty) return SCE_ERRNO_ENOTEMPTY;
	if (ec == E::no_space_on_device) return SCE_ERRNO_ENOSPC;
	if (ec == E::read_only_file_system) return SCE_ERRNO_EROFS;
	if (ec == E::filename_too_long) return SCE_ERRNO_ENAMETOOLONG;
	if (ec == E::not_enough_memory) return SCE_ERRNO_ENOMEM;
	if (ec == E::too_many_files_open) return SCE_ERRNO_EMFILE;
	if (ec == E::invalid_argument) return SCE_ERRNO_EINVAL;
	return SCE_ERRNO_EIO;
}
s32 IoError() { return MapError(std::error_code(errno, std::generic_category())); }

void ToPspTime(s64 us, ScePspDateTime *out) {
	*out = {};
	time_t sec = us / 1000000;
	std::tm t{};
#ifdef _WIN32
	if (localtime_s(&t, &sec)) return;
#else
	if (!localtime_r(&sec, &t)) return;
#endif
	out->year = static_cast<u16>(t.tm_year + 1900);
	out->month = static_cast<u16>(t.tm_mon + 1);
	out->day = static_cast<u16>(t.tm_mday);
	out->hour = static_cast<u16>(t.tm_hour);
	out->minute = static_cast<u16>(t.tm_min);
	out->second = static_cast<u16>(t.tm_sec);
	out->microsecond = (u32)((us % 1000000 + 1000000) % 1000000);
}
bool FromPspTime(const ScePspDateTime &in, s64 *us) {
	std::tm t{};
	t.tm_year = in.year - 1900; t.tm_mon = in.month - 1; t.tm_mday = in.day;
	t.tm_hour = in.hour; t.tm_min = in.minute; t.tm_sec = in.second; t.tm_isdst = -1;
	time_t sec = mktime(&t);
	if (t.tm_year != in.year - 1900 || t.tm_mon != in.month - 1 || t.tm_mday != in.day) return false;
	*us = (s64)sec * 1000000 + in.microsecond % 1000000;
	return true;
}
s32 Component(std::string name, fs::path *out) {
	for (unsigned char c : name)
		if (c < 0x20 || strchr("<>:\"|?*\\", c)) return SCE_ERRNO_EINVAL;
	while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
	if (name.empty()) return SCE_ERRNO_EINVAL;
	if (name.size() > 255) return SCE_ERRNO_ENAMETOOLONG;
	*out = fs::u8path(name);
	return 0;
}
std::vector<fs::path> Names(const fs::path &dir) {
	std::vector<fs::path> out;
	std::error_code ec;
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
		out.push_back(it->path().filename());
	// Stable FAT short-name numbering and directory order on case-sensitive hosts.
	std::sort(out.begin(), out.end());
	return out;
}
std::string UpperCase83(const std::string &name) {
	static const char *const chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&'(){}-_`~";
	auto valid = [](std::string_view s, size_t max) {
		if (s.size() > max) return false;
		bool lower = false, upper = false;
		for (char c : s) {
			if (!c || !strchr(chars, c)) return false;
			lower |= c >= 'a' && c <= 'z'; upper |= c >= 'A' && c <= 'Z';
		}
		return !(lower && upper);
	};
	size_t dot = name.find('.');
	std::string_view base = std::string_view(name).substr(0, dot);
	std::string_view ext = dot == std::string::npos ? std::string_view() : std::string_view(name).substr(dot + 1);
	if (base.empty() || !valid(base, 8) || !valid(ext, 3)) return name;
	std::string out = name;
	for (char &c : out) if (c >= 'a' && c <= 'z') c -= 32;
	return out;
}
void CopyName(char (&dst)[256], const std::string &src) {
	size_t n = std::min(src.size(), sizeof(dst) - 1);
	while (n && n < src.size() && ((unsigned char)src[n] & 0xC0) == 0x80) --n;
	memcpy(dst, src.data(), n); dst[n] = 0;
}
}
struct HostFileSystem::EntryInfo {
	std::string name;
	hostfile::Info info;
	bool created = false;
};
struct HostFileSystem::OpenFile {
	std::mutex m;
	FILE *file = nullptr;
	u32 flags = 0;
	~OpenFile() { if (file) fclose(file); }
};
struct HostFileSystem::OpenDir { std::vector<EntryInfo> entries; size_t index = 0; };

HostFileSystem::HostFileSystem(fs::path root, HostFsOptions options) : opt_(options) {
	std::error_code ec;
	// Not a read-only root (flash0): nothing could be written into it, and a run given an old
	// --flash0 path would leave an empty folder there.
	if (!opt_.readOnly) fs::create_directories(root, ec);
	root_ = fs::weakly_canonical(fs::absolute(root));
}
HostFileSystem::~HostFileSystem() = default;
void HostFileSystem::AddFallbackRoot(const fs::path &dir) {
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) return;
	fs::path p = fs::weakly_canonical(dir, ec);
	if (ec || p == root_ || std::find(fallbackRoots_.begin(), fallbackRoots_.end(), p) != fallbackRoots_.end()) return;
	fallbackRoots_.push_back(p);
}
u32 HostFileSystem::Caps() const {
	return (opt_.readOnly ? FS_CAP_READ_ONLY : 0) | (opt_.fat ? FS_CAP_FAT : 0) | FS_CAP_CASE_INSENSITIVE;
}
bool HostFileSystem::Created(const fs::path &p) const {
	std::lock_guard lock(createdMutex_);
	return created_.count(fs::u8path(ToLower(PathToUtf8(p)))) != 0;
}
void HostFileSystem::SetCreated(const fs::path &p, bool created) {
	std::lock_guard lock(createdMutex_);
	auto key = fs::u8path(ToLower(PathToUtf8(p)));
	if (created) created_.insert(key); else created_.erase(key);
}
s32 HostFileSystem::ResolveIn(const fs::path &root, std::string_view path, bool missing,
                              fs::path *out, std::vector<std::string> *comps) {
	auto parts = SplitPspPathComponents(path);
	fs::path cur = root;
	s32 exists = 1;
	for (size_t i = 0; i < parts.size(); ++i) {
		fs::path name;
		s32 error = Component(parts[i], &name);
		if (error < 0) return error;
		auto names = Names(cur);
		bool found = false;
		if (opt_.fat && parts[i].find('~') != std::string::npos) {
			std::vector<std::string> text;
			for (const auto &n : names) text.push_back(PathToUtf8(n));
			auto shorts = GenerateFatShortNames(text);
			for (size_t j = 0; j < shorts.size(); ++j)
				if (EqualsNoCase(shorts[j], parts[i])) { name = names[j]; found = true; break; }
		}
		if (!found) {
			for (const auto &n : names)
				if (EqualsNoCase(PathToUtf8(n), PathToUtf8(name))) { name = n; found = true; break; }
		}
		cur /= name;
		if (!found) {
			if (i + 1 != parts.size() || !missing) return SCE_ERRNO_ENOENT;
			exists = 0;
		} else if (i + 1 != parts.size()) {
			std::error_code ec;
			if (!fs::is_directory(cur, ec)) return SCE_ERRNO_ENOENT;
		}
	}
	*out = cur;
	if (comps) *comps = std::move(parts);
	return exists;
}
s32 HostFileSystem::Resolve(std::string_view path, bool missing, fs::path *out, std::vector<std::string> *comps) {
	std::string_view name = path;
	while (!name.empty() && (name.front() == '/' || name.front() == '\\')) name.remove_prefix(1);
	if (!name.empty() && name.find_first_of("/\\") == std::string_view::npos) {
		for (const auto &r : opt_.redirects) if (r.first == ToLower(std::string(name))) {
			*out = r.second;
			if (comps) *comps = {std::string(name)};
			std::error_code ec;
			if (fs::exists(*out, ec)) return 1;
			return missing ? 0 : SCE_ERRNO_ENOENT;
		}
	}
	s32 result = ResolveIn(root_, path, missing, out, comps);
	if (result == 1) return result;
	for (const auto &root : fallbackRoots_) {
		fs::path candidate;
		std::vector<std::string> cs;
		s32 r = ResolveIn(root, path, missing, &candidate, &cs);
		if (r == 1 || (r == 0 && result < 0)) {
			*out = candidate; if (comps) *comps = cs; result = r;
		}
		if (r == 1) break;
	}
	return result;
}
s32 HostFileSystem::HostPath(std::string_view path, fs::path *out) {
	s32 r = Resolve(path, true, out, nullptr); return r < 0 ? r : 0;
}
s32 HostFileSystem::AllocHandle() {
	for (;;) {
		s32 h = nextHandle_;
		nextHandle_ = h == INT32_MAX ? 1 : h + 1;
		if (!files_.count(h) && !dirs_.count(h)) return h;
	}
}
std::shared_ptr<HostFileSystem::OpenFile> HostFileSystem::GetFile(s32 fd) {
	std::lock_guard lock(mutex_);
	auto it = files_.find(fd); return it == files_.end() ? nullptr : it->second;
}
s32 HostFileSystem::Open(std::string_view path, u32 flags, s32 mode) {
	bool read = flags & PSP_O_RDONLY, write = flags & PSP_O_WRONLY;
	if (!read && !write) return SCE_ERRNO_EINVAL;
	if (opt_.readOnly && (write || (flags & (PSP_O_CREAT | PSP_O_TRUNC | PSP_O_APPEND)))) return SCE_ERRNO_EROFS;
	fs::path hp;
	std::vector<std::string> cs;
	s32 r = Resolve(path, flags & PSP_O_CREAT, &hp, &cs);
	if (r < 0) return r;
	if (cs.empty()) return SCE_ERRNO_ENOENT;
	if (r) {
		hostfile::Info info; std::error_code ec;
		if (!hostfile::Stat(hp, info, ec)) return MapError(ec);
		if (info.directory) return SCE_ERRNO_ENOENT;
		if ((flags & PSP_O_CREAT) && (flags & PSP_O_EXCL)) return SCE_ERRNO_EEXIST;
		if (write && info.readOnly) return SCE_ERRNO_EACCES;
	}
	auto f = std::make_shared<OpenFile>();
	f->file = hostfile::Open(hp, read, write, flags & PSP_O_CREAT, (flags & PSP_O_CREAT) && (flags & PSP_O_EXCL));
	if (!f->file) return IoError();
	f->flags = flags;
	if (!r && !opt_.fat) SetCreated(hp, true);
	if ((flags & PSP_O_TRUNC) && write && !hostfile::Truncate(f->file, 0)) return IoError();
	if ((flags & PSP_O_APPEND) && hostfile::Seek(f->file, 0, SEEK_END)) return IoError();
	std::lock_guard lock(mutex_);
	s32 fd = AllocHandle(); files_[fd] = std::move(f); return fd;
}
s32 HostFileSystem::Close(s32 fd) { std::lock_guard lock(mutex_); return files_.erase(fd) ? 0 : SCE_KERNEL_ERROR_BADF; }
s64 HostFileSystem::Read(s32 fd, void *buf, u64 size) {
	auto f = GetFile(fd);
	if (!f || !(f->flags & PSP_O_RDONLY)) return SCE_KERNEL_ERROR_BADF;
	std::lock_guard lock(f->m);
	// A positioning operation permits switching between reading and writing on an update stream.
	if (hostfile::Seek(f->file, 0, SEEK_CUR)) return IoError();
	size_t got = fread(buf, 1, (size_t)size, f->file);
	return !got && ferror(f->file) ? IoError() : (s64)got;
}
s64 HostFileSystem::Write(s32 fd, const void *buf, u64 size) {
	auto f = GetFile(fd);
	if (!f || !(f->flags & PSP_O_WRONLY)) return SCE_KERNEL_ERROR_BADF;
	std::lock_guard lock(f->m);
	if (hostfile::Seek(f->file, 0, f->flags & PSP_O_APPEND ? SEEK_END : SEEK_CUR)) return IoError();
	size_t wrote = fwrite(buf, 1, (size_t)size, f->file);
	return !wrote && ferror(f->file) ? IoError() : (s64)wrote;
}
s64 HostFileSystem::Seek(s32 fd, s64 offset, s32 whence) {
	auto f = GetFile(fd); if (!f) return SCE_KERNEL_ERROR_BADF;
	std::lock_guard lock(f->m);
	s64 base = 0;
	if (whence == PSP_SEEK_CUR) base = hostfile::Tell(f->file);
	else if (whence == PSP_SEEK_END) {
		s64 old = hostfile::Tell(f->file);
		if (hostfile::Seek(f->file, 0, SEEK_END)) return -1;
		base = hostfile::Tell(f->file);
		if (hostfile::Seek(f->file, old, SEEK_SET)) return -1;
	} else if (whence != PSP_SEEK_SET) return SCE_KERNEL_ERROR_INVAL;
	if (base < 0 || (offset < 0 && offset < -base) || (offset > 0 && base > INT64_MAX - offset)) return -1;
	return hostfile::Seek(f->file, base + offset, SEEK_SET) ? -1 : base + offset;
}
s32 HostFileSystem::GetHandleInfo(s32 fd, HandleInfo *out) {
	auto f = GetFile(fd); if (!f) return SCE_KERNEL_ERROR_BADF;
	std::lock_guard lock(f->m);
	s64 pos = hostfile::Tell(f->file);
	if (pos < 0 || hostfile::Seek(f->file, 0, SEEK_END)) return IoError();
	s64 size = hostfile::Tell(f->file);
	if (hostfile::Seek(f->file, pos, SEEK_SET) || size < 0) return IoError();
	*out = {}; out->flags = f->flags; out->size = (u64)size; out->position = (u64)pos; return 0;
}
s32 HostFileSystem::Ioctl(s32 fd, u32, const void *, u32, void *, u32) {
	return GetFile(fd) ? SCE_ERRNO_FUNCTION_NOT_SUPPORTED : SCE_KERNEL_ERROR_BADF;
}
void HostFileSystem::FillStat(const EntryInfo &e, SceIoStat *out) const {
	bool dir = e.info.directory, ro = e.info.readOnly && !dir;
	u32 type = dir ? FIO_S_IFDIR : FIO_S_IFREG;
	if (opt_.fat) {
		out->st_mode = type | (ro ? 0555 : 0777);
		out->st_attr = (dir ? FIO_SO_IFDIR : FIO_SO_IFREG) | (ro ? FAT_ATTR_READONLY : 0) |
		              (e.info.hidden ? FAT_ATTR_HIDDEN : 0) | (e.info.system ? FAT_ATTR_SYSTEM : 0);
	} else {
		out->st_mode = type | (dir ? 0777 : ro ? 0444 : e.created ? 0644 : 0664);
		out->st_attr = dir ? FIO_SO_IFDIR : FIO_SO_IFREG;
	}
	out->st_size = dir ? 0 : (s64)e.info.size;
	ToPspTime(e.info.created, &out->st_ctime); ToPspTime(e.info.accessed, &out->st_atime); ToPspTime(e.info.modified, &out->st_mtime);
	if (opt_.fat) { out->st_atime.hour = out->st_atime.minute = out->st_atime.second = 0; out->st_atime.microsecond = 0; }
}
s32 HostFileSystem::GetStat(std::string_view path, SceIoStat *out) {
	fs::path hp; s32 r = Resolve(path, false, &hp, nullptr); if (r < 0) return r;
	EntryInfo e; std::error_code ec;
	if (!hostfile::Stat(hp, e.info, ec)) return MapError(ec);
	e.created = Created(hp); FillStat(e, out); return 0;
}
s32 HostFileSystem::ChStat(std::string_view path, const SceIoStat &in, u32 bits) {
	if (opt_.readOnly) return SCE_ERRNO_EROFS;
	fs::path hp; s32 r = Resolve(path, false, &hp, nullptr); if (r < 0) return r;
	hostfile::Info info; std::error_code ec;
	if (!hostfile::Stat(hp, info, ec)) return MapError(ec);
	if (bits & (SCE_CST_CT | SCE_CST_AT | SCE_CST_MT)) {
		s64 c, a, m;
		bool hc = (bits & SCE_CST_CT) && FromPspTime(in.st_ctime, &c);
		bool ha = (bits & SCE_CST_AT) && FromPspTime(in.st_atime, &a);
		bool hm = (bits & SCE_CST_MT) && FromPspTime(in.st_mtime, &m);
		if (!hostfile::SetTimes(hp, hc ? &c : nullptr, ha ? &a : nullptr, hm ? &m : nullptr, ec)) return MapError(ec);
	}
	if ((bits & (SCE_CST_MODE | SCE_CST_ATTR)) && !info.directory) {
		bool writable = bits & SCE_CST_ATTR ? !(in.st_attr & FAT_ATTR_READONLY) : (in.st_mode & 0222) != 0;
		fs::permissions(hp, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
		                writable ? fs::perm_options::add : fs::perm_options::remove, ec);
		if (ec) return MapError(ec);
	}
	return 0;
}
s32 HostFileSystem::Remove(std::string_view path) {
	if (opt_.readOnly) return SCE_ERRNO_EROFS;
	fs::path hp; std::vector<std::string> cs; s32 r = Resolve(path, false, &hp, &cs); if (r < 0) return r;
	hostfile::Info info; std::error_code ec;
	if (!hostfile::Stat(hp, info, ec)) return MapError(ec);
	if (cs.empty() || info.directory) return SCE_ERRNO_EISDIR;
	if (info.readOnly) return SCE_ERRNO_EACCES;
	if (!fs::remove(hp, ec)) return ec ? MapError(ec) : SCE_ERRNO_ENOENT;
	SetCreated(hp, false); return 0;
}
s32 HostFileSystem::Mkdir(std::string_view path, s32) {
	if (opt_.readOnly) return SCE_ERRNO_EROFS;
	fs::path hp; s32 r = Resolve(path, true, &hp, nullptr); if (r < 0) return r;
	if (r == 1) return SCE_ERRNO_EEXIST;
	std::error_code ec; fs::create_directory(hp, ec); return ec ? MapError(ec) : 0;
}
s32 HostFileSystem::Rmdir(std::string_view path) {
	if (opt_.readOnly) return SCE_ERRNO_EROFS;
	fs::path hp; std::vector<std::string> cs; s32 r = Resolve(path, false, &hp, &cs); if (r < 0) return r;
	if (cs.empty()) return SCE_ERRNO_EBUSY;
	std::error_code ec;
	if (!fs::is_directory(hp, ec)) return SCE_ERRNO_ENOTDIR;
	fs::remove(hp, ec); return ec ? MapError(ec) : 0;
}
s32 HostFileSystem::Rename(std::string_view from, std::string_view to) {
	if (opt_.readOnly) return SCE_ERRNO_EROFS;
	if (from.find_first_of("*?") != from.npos || to.find_first_of("*?") != to.npos) return SCE_ERRNO_EINVAL;
	auto fromComps = SplitPspPathComponents(from), toComps = SplitPspPathComponents(to);
	if (fromComps.empty() || toComps.empty()) return SCE_ERRNO_EINVAL;
	if (opt_.fat) { auto name = toComps.back(); toComps = fromComps; toComps.back() = name; }
	std::string toPath;
	for (const auto &c : toComps) toPath += "/" + c;
	fs::path src, dst;
	s32 r = Resolve(from, false, &src, nullptr); if (r < 0) return r;
	r = Resolve(toPath, true, &dst, nullptr); if (r < 0) return r;
	if (r == 1) return SCE_ERRNO_EEXIST;
	std::error_code ec;
	fs::rename(src, dst, ec);
	if (ec == std::errc::cross_device_link) {
		fs::copy(src, dst, fs::copy_options::recursive, ec);
		if (!ec) fs::remove_all(src, ec);
	}
	if (ec) return MapError(ec);
	if (Created(src)) { SetCreated(src, false); SetCreated(dst, true); }
	return 0;
}
void HostFileSystem::ListInto(const fs::path &dir, std::vector<EntryInfo> *entries) const {
	for (const auto &name : Names(dir)) {
		EntryInfo e; std::error_code ec;
		if (!hostfile::Stat(dir / name, e.info, ec)) continue;
		e.name = PathToUtf8(name); if (opt_.upperCase83) e.name = UpperCase83(e.name);
		e.created = Created(dir / name); entries->push_back(std::move(e));
	}
}
s32 HostFileSystem::DOpen(std::string_view path) {
	fs::path hp; s32 r = Resolve(path, false, &hp, nullptr); if (r < 0) return r;
	EntryInfo dot; std::error_code ec;
	if (!hostfile::Stat(hp, dot.info, ec)) return MapError(ec);
	if (!dot.info.directory) return SCE_ERRNO_ENOTDIR;
	auto d = std::make_shared<OpenDir>();
	dot.name = "."; d->entries.push_back(dot); dot.name = ".."; d->entries.push_back(dot);
	std::vector<fs::path> roots{root_}; roots.insert(roots.end(), fallbackRoots_.begin(), fallbackRoots_.end());
	for (const auto &root : roots) {
		fs::path dir; if (ResolveIn(root, path, false, &dir, nullptr) != 1) continue;
		std::vector<EntryInfo> more; ListInto(dir, &more);
		for (auto &e : more) {
			bool dup = std::any_of(d->entries.begin(), d->entries.end(), [&](const auto &have) { return EqualsNoCase(have.name, e.name); });
			if (!dup) d->entries.push_back(std::move(e));
		}
	}
	std::lock_guard lock(mutex_); s32 dd = AllocHandle(); dirs_[dd] = d; return dd;
}
s32 HostFileSystem::DRead(s32 dd, SceIoDirent *out) {
	std::lock_guard lock(mutex_);
	auto it = dirs_.find(dd); if (it == dirs_.end()) return SCE_KERNEL_ERROR_BADF;
	auto &d = *it->second;
	if (d.index >= d.entries.size()) { out->d_name[0] = 0; return 0; }
	const auto &e = d.entries[d.index++]; FillStat(e, &out->d_stat);
	if (e.name == "..") out->d_stat.st_size = 4096;
	CopyName(out->d_name, e.name); return 1;
}
s32 HostFileSystem::DClose(s32 dd) { std::lock_guard lock(mutex_); return dirs_.erase(dd) ? 0 : SCE_KERNEL_ERROR_BADF; }
bool HostFileSystem::QueryFreeSpace(u64 *free, u64 *total) {
	std::error_code ec; auto info = fs::space(root_, ec); if (ec) return false;
	*free = info.available; *total = info.capacity; return true;
}
}
