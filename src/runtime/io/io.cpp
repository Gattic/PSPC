// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/io/io.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>

#include "allegrex/guestmem.h"
#include "common/fileutil.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/errors.h"

namespace io {

namespace {

std::map<std::string, std::shared_ptr<Device>> g_mounts;
std::string g_cwd = "ms0:/";

std::string CanonDevice(std::string d) {
	d = ToLower(d);
	if (d == "fatms0" || d == "fatms" || d == "ms") return "ms0";
	if (d == "umd1" || d == "umd" || d == "isofs0") return "umd0";
	return d;
}

// Collapse "." and ".." and duplicate slashes; never climbs above "/".
std::string NormalizePath(const std::string &p) {
	std::vector<std::string> parts;
	std::string cur;
	auto flush = [&]() {
		if (cur.empty() || cur == ".") {
		} else if (cur == "..") {
			if (!parts.empty()) parts.pop_back();
		} else {
			parts.push_back(cur);
		}
		cur.clear();
	};
	for (char ch : p) {
		if (ch == '/' || ch == '\\') flush();
		else cur.push_back(ch);
	}
	flush();
	std::string out = "/";
	for (size_t i = 0; i < parts.size(); i++) {
		if (i) out += "/";
		out += parts[i];
	}
	return out;
}

PspDateTime ToPspTime(std::chrono::system_clock::time_point tp) {
	time_t t = std::chrono::system_clock::to_time_t(tp);
	tm lt{};
	localtime_s(&lt, &t);
	PspDateTime d{};
	d.year = (u16)(lt.tm_year + 1900);
	d.month = (u16)(lt.tm_mon + 1);
	d.day = (u16)lt.tm_mday;
	d.hour = (u16)lt.tm_hour;
	d.minute = (u16)lt.tm_min;
	d.second = (u16)lt.tm_sec;
	return d;
}

std::chrono::system_clock::time_point FileTimeToSys(fs::file_time_type ft) {
	return std::chrono::clock_cast<std::chrono::system_clock>(ft);
}

class HostDevice : public Device {
public:
	HostDevice(const std::string &root, bool readOnly) : root_(fs::path(root)), readOnly_(readOnly) {
		std::error_code ec;
		fs::create_directories(root_, ec);
	}

	fs::path Host(const std::string &path) const {
		std::string n = NormalizePath(path);
		fs::path p = root_;
		if (n.size() > 1) p /= fs::u8path(n.substr(1));
		return p;
	}

	int Open(const std::string &path, int flags, int mode) override {
		(void)mode;
		fs::path p = Host(path);
		std::error_code ec;
		bool exists = fs::exists(p, ec);
		if (exists && fs::is_directory(p, ec)) return (int)SCE_KERNEL_ERROR_ERRNO_IS_DIRECTORY;
		bool write = (flags & PSP_O_WRONLY) != 0;
		if (write && readOnly_) return (int)SCE_KERNEL_ERROR_ERRNO_READ_ONLY;
		if ((flags & PSP_O_CREAT) && (flags & PSP_O_EXCL) && exists) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_ALREADY_EXISTS;
		if (!exists && !(flags & PSP_O_CREAT)) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
		const wchar_t *fmode;
		if (!write) fmode = L"rb";
		else if (!exists || (flags & PSP_O_TRUNC)) fmode = (flags & PSP_O_RDONLY) ? L"w+b" : L"wb";
		else fmode = (flags & PSP_O_RDONLY) ? L"r+b" : L"r+b";
		if (!exists && write && p.has_parent_path() && !fs::exists(p.parent_path(), ec))
			return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
		FILE *f = _wfopen(p.wstring().c_str(), fmode);
		if (!f) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
		int h = next_++;
		files_[h] = {f, (flags & PSP_O_APPEND) != 0};
		return h;
	}
	int Close(int h) override {
		auto it = files_.find(h);
		if (it == files_.end()) return (int)SCE_KERNEL_ERROR_BADF;
		fclose(it->second.f);
		files_.erase(it);
		return 0;
	}
	s64 Read(int h, void *buf, s64 size) override {
		auto it = files_.find(h);
		if (it == files_.end()) return (s64)(s32)SCE_KERNEL_ERROR_BADF;
		return (s64)fread(buf, 1, (size_t)size, it->second.f);
	}
	s64 Write(int h, const void *buf, s64 size) override {
		auto it = files_.find(h);
		if (it == files_.end()) return (s64)(s32)SCE_KERNEL_ERROR_BADF;
		if (it->second.append) _fseeki64(it->second.f, 0, SEEK_END);
		s64 n = (s64)fwrite(buf, 1, (size_t)size, it->second.f);
		fflush(it->second.f);
		return n;
	}
	s64 Seek(int h, s64 offset, int whence) override {
		auto it = files_.find(h);
		if (it == files_.end()) return (s64)(s32)SCE_KERNEL_ERROR_BADF;
		int w = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
		if (_fseeki64(it->second.f, offset, w) != 0) return (s64)(s32)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
		return _ftelli64(it->second.f);
	}
	int GetStat(const std::string &path, PspIoStat &st) override {
		fs::path p = Host(path);
		std::error_code ec;
		if (!fs::exists(p, ec)) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
		memset(&st, 0, sizeof(st));
		bool dir = fs::is_directory(p, ec);
		st.mode = (dir ? FIO_S_IFDIR : FIO_S_IFREG) | 0777;
		st.attr = (dir ? FIO_SO_IFDIR : FIO_SO_IFREG) | 0x7;
		st.size = dir ? 0 : (s64)fs::file_size(p, ec);
		auto t = ToPspTime(FileTimeToSys(fs::last_write_time(p, ec)));
		st.ctime = st.atime = st.mtime = t;
		return 0;
	}
	int Remove(const std::string &path) override {
		if (readOnly_) return (int)SCE_KERNEL_ERROR_ERRNO_READ_ONLY;
		std::error_code ec;
		fs::path p = Host(path);
		if (!fs::exists(p, ec)) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
		if (fs::is_directory(p, ec)) return (int)SCE_KERNEL_ERROR_ERRNO_IS_DIRECTORY;
		return fs::remove(p, ec) ? 0 : (int)SCE_KERNEL_ERROR_ERRNO_NO_PERM;
	}
	int Mkdir(const std::string &path, int mode) override {
		(void)mode;
		if (readOnly_) return (int)SCE_KERNEL_ERROR_ERRNO_READ_ONLY;
		std::error_code ec;
		fs::path p = Host(path);
		if (fs::exists(p, ec)) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_ALREADY_EXISTS;
		return fs::create_directory(p, ec) ? 0 : (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
	}
	int Rmdir(const std::string &path) override {
		if (readOnly_) return (int)SCE_KERNEL_ERROR_ERRNO_READ_ONLY;
		std::error_code ec;
		fs::path p = Host(path);
		if (!fs::is_directory(p, ec)) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
		return fs::remove(p, ec) ? 0 : (int)SCE_KERNEL_ERROR_ERRNO_DIRECTORY_IS_NOT_EMPTY;
	}
	int Rename(const std::string &from, const std::string &to) override {
		if (readOnly_) return (int)SCE_KERNEL_ERROR_ERRNO_READ_ONLY;
		std::error_code ec;
		fs::path a = Host(from), b = Host(to);
		if (!fs::exists(a, ec)) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
		if (fs::exists(b, ec)) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_ALREADY_EXISTS;
		fs::rename(a, b, ec);
		return ec ? (int)SCE_KERNEL_ERROR_ERRNO_NO_PERM : 0;
	}
	int DOpen(const std::string &path) override {
		fs::path p = Host(path);
		std::error_code ec;
		if (!fs::is_directory(p, ec)) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
		std::vector<PspIoDirent> ents;
		auto add = [&](const std::string &name, const fs::path &hp) {
			PspIoDirent e{};
			std::error_code ec2;
			bool dir = fs::is_directory(hp, ec2);
			e.stat.mode = (dir ? FIO_S_IFDIR : FIO_S_IFREG) | 0777;
			e.stat.attr = (dir ? FIO_SO_IFDIR : FIO_SO_IFREG) | 0x7;
			e.stat.size = dir ? 0 : (s64)fs::file_size(hp, ec2);
			auto t = ToPspTime(FileTimeToSys(fs::last_write_time(hp, ec2)));
			e.stat.ctime = e.stat.atime = e.stat.mtime = t;
			strncpy(e.name, name.c_str(), 255);
			ents.push_back(e);
		};
		add(".", p);
		add("..", p);
		for (auto &de : fs::directory_iterator(p, ec)) add(PathToUtf8(de.path().filename()), de.path());
		int h = next_++;
		dirs_[h] = {std::move(ents), 0};
		return h;
	}
	int DRead(int h, PspIoDirent &ent) override {
		auto it = dirs_.find(h);
		if (it == dirs_.end()) return (int)SCE_KERNEL_ERROR_BADF;
		if (it->second.pos >= it->second.ents.size()) return 0;
		ent = it->second.ents[it->second.pos++];
		return 1;
	}
	int DClose(int h) override {
		if (!dirs_.erase(h)) return (int)SCE_KERNEL_ERROR_BADF;
		return 0;
	}
	int Devctl(const std::string &devName, u32 cmd, u32 inAddr, u32 inLen, u32 outAddr, u32 outLen) override {
		(void)devName;
		(void)inAddr;
		(void)inLen;
		switch (cmd) {
		case 0x02425818: {  // memory stick: get free space info
			if (!mem::IsValidRange(inAddr, 4)) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
			u32 ptr = GRead32(inAddr);
			if (mem::IsValidRange(ptr, 20)) {
				const u32 cluster = 0x8000;
				GWrite32(ptr, 0x200000);       // max clusters
				GWrite32(ptr + 4, 0x100000);   // free clusters
				GWrite32(ptr + 8, 0x100000);   // max sectors
				GWrite32(ptr + 12, 0x200);     // sector size
				GWrite32(ptr + 16, cluster / 0x200);
			}
			return 0;
		}
		case 0x02425823:  // memory stick inserted?
			if (mem::IsValidRange(outAddr, 4) && outLen >= 4) GWrite32(outAddr, 1);
			return 0;
		case 0x02415821:  // register ms callback
		case 0x02415822:  // unregister ms callback
		case 0x02025801:  // ms state
			if (cmd == 0x02025801 && mem::IsValidRange(outAddr, 4)) GWrite32(outAddr, 4);
			return 0;
		case 0x02025806:  // inserted
			if (mem::IsValidRange(outAddr, 4)) GWrite32(outAddr, 1);
			return 0;
		default: return Device::Devctl(devName, cmd, inAddr, inLen, outAddr, outLen);
		}
	}

private:
	struct OpenFile {
		FILE *f;
		bool append;
	};
	struct OpenDir {
		std::vector<PspIoDirent> ents;
		size_t pos;
	};
	fs::path root_;
	bool readOnly_;
	int next_ = 1;
	std::map<int, OpenFile> files_;
	std::map<int, OpenDir> dirs_;
};

class EmulatorDevice : public Device {
public:
	explicit EmulatorDevice(bool hasDisplay) : hasDisplay_(hasDisplay) {}
	int Open(const std::string &, int, int) override { return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND; }
	int Close(int) override { return 0; }
	s64 Read(int, void *, s64) override { return 0; }
	s64 Write(int, const void *, s64) override { return 0; }
	s64 Seek(int, s64, int) override { return 0; }
	int GetStat(const std::string &, PspIoStat &) override { return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND; }
	int DOpen(const std::string &) override { return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND; }
	int DRead(int, PspIoDirent &) override { return 0; }
	int DClose(int) override { return 0; }
	int Devctl(const std::string &, u32 cmd, u32 inAddr, u32 inLen, u32 outAddr, u32 outLen) override {
		switch (cmd) {
		case 1:  // GET_HAS_DISPLAY
			if (mem::IsValidRange(outAddr, 4) && outLen >= 4) GWrite32(outAddr, hasDisplay_ ? 1 : 0);
			return 0;
		case 2:  // SEND_OUTPUT
			if (mem::IsValidRange(inAddr, inLen)) logging::GuestOutput((const char *)GPtr(inAddr), inLen);
			return 0;
		case 3:  // IS_EMULATOR
			if (mem::IsValidRange(outAddr, 4) && outLen >= 4) GWrite32(outAddr, 1);
			return 0;
		case 0x10:  // SEND_CTRLDATA
		case 0x20:  // EMIT_SCREENSHOT
			return 0;
		default: return (int)SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED;
		}
	}

private:
	bool hasDisplay_;
};

}  // namespace

int Device::Remove(const std::string &) { return (int)SCE_KERNEL_ERROR_ERRNO_READ_ONLY; }
int Device::Mkdir(const std::string &, int) { return (int)SCE_KERNEL_ERROR_ERRNO_READ_ONLY; }
int Device::Rmdir(const std::string &) { return (int)SCE_KERNEL_ERROR_ERRNO_READ_ONLY; }
int Device::Rename(const std::string &, const std::string &) { return (int)SCE_KERNEL_ERROR_ERRNO_READ_ONLY; }
int Device::Ioctl(int, u32 cmd, u32, u32, u32, u32) {
	LOGW("IO", "Unsupported ioctl %08x", cmd);
	return (int)SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED;
}
int Device::Devctl(const std::string &devName, u32 cmd, u32, u32, u32, u32) {
	LOGW("IO", "Unsupported devctl %s %08x", devName.c_str(), cmd);
	return (int)SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED;
}

void Init() {
	g_mounts.clear();
	g_cwd = "ms0:/";
}

void Shutdown() { g_mounts.clear(); }

void Mount(const std::string &device, std::shared_ptr<Device> dev) { g_mounts[CanonDevice(device)] = std::move(dev); }
void Unmount(const std::string &device) { g_mounts.erase(CanonDevice(device)); }

std::shared_ptr<Device> MountedDevice(const std::string &device) {
	auto it = g_mounts.find(CanonDevice(device));
	return it == g_mounts.end() ? nullptr : it->second;
}

bool ResolvePath(const std::string &pspPath, std::string &device, std::string &path) {
	std::string full = pspPath;
	size_t colon = full.find(':');
	if (colon == std::string::npos) {
		std::string cwd = g_cwd;
		if (!cwd.empty() && cwd.back() != '/') cwd += "/";
		full = (!pspPath.empty() && pspPath[0] == '/') ? cwd.substr(0, cwd.find(':') + 1) + pspPath : cwd + pspPath;
		colon = full.find(':');
		if (colon == std::string::npos) return false;
	}
	device = CanonDevice(full.substr(0, colon));
	path = NormalizePath(full.substr(colon + 1));
	return g_mounts.count(device) != 0;
}

void SetCwd(const std::string &pspDir) { g_cwd = pspDir; }
const std::string &Cwd() { return g_cwd; }

bool ReadFile(const std::string &pspPath, std::vector<u8> &out) {
	std::string dev, path;
	if (!ResolvePath(pspPath, dev, path)) return false;
	auto d = MountedDevice(dev);
	int h = d->Open(path, PSP_O_RDONLY, 0);
	if (h < 0) return false;
	s64 size = d->Seek(h, 0, 2);
	d->Seek(h, 0, 0);
	if (size < 0) {
		d->Close(h);
		return false;
	}
	out.resize((size_t)size);
	s64 got = size ? d->Read(h, out.data(), size) : 0;
	d->Close(h);
	return got == size;
}

std::shared_ptr<Device> MakeHostDevice(const std::string &hostRoot, bool readOnly) {
	return std::make_shared<HostDevice>(hostRoot, readOnly);
}

std::shared_ptr<Device> MakeEmulatorDevice(bool hasDisplay) { return std::make_shared<EmulatorDevice>(hasDisplay); }

std::string HostPathFor(const std::string &pspPath) {
	std::string dev, path;
	if (!ResolvePath(pspPath, dev, path)) return "";
	auto d = MountedDevice(dev);
	if (auto *h = dynamic_cast<HostDevice *>(d.get())) return h->Host(path).string();
	return "";
}

}  // namespace io
