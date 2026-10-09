// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/dialogs/save_fs.h"

#include <cstring>

#include "fs/filesystem.h"
#include "runtime/io/io.h"

namespace utility::savefs {

namespace {

struct Target {
	io::Resolved r;
	vfs::FileSystem *fs = nullptr;
};

bool Resolve(const std::string &pspPath, Target &t) {
	if (io::Resolve(pspPath, &t.r) != 0 || !t.r.dev) return false;
	t.fs = t.r.dev->Fs();
	return t.fs != nullptr;
}

std::string LastComponent(const std::string &p) {
	size_t end = p.size();
	while (end > 0 && p[end - 1] == '/') end--;
	size_t start = end;
	while (start > 0 && p[start - 1] != '/' && p[start - 1] != ':') start--;
	return p.substr(start, end - start);
}

bool IsDirStat(const vfs::SceIoStat &st) {
	return ((u32)st.st_mode & vfs::FIO_S_IFMT) == vfs::FIO_S_IFDIR || (st.st_attr & vfs::FIO_SO_IFDIR) != 0;
}

void FillInfo(FileInfo &fi, const vfs::SceIoStat &st) {
	fi.exists = true;
	fi.isDir = IsDirStat(st);
	fi.size = fi.isDir ? 0 : st.st_size;
	fi.ctime = st.st_ctime;
	fi.atime = st.st_atime;
	fi.mtime = st.st_mtime;
}

}  // namespace

FileInfo Stat(const std::string &pspPath) {
	FileInfo fi;
	fi.name = LastComponent(pspPath);
	Target t;
	if (!Resolve(pspPath, t)) return fi;
	vfs::SceIoStat st{};
	if (t.fs->GetStat(t.r.path, &st) < 0) return fi;
	FillInfo(fi, st);
	return fi;
}

bool Exists(const std::string &pspPath) { return Stat(pspPath).exists; }

std::vector<FileInfo> List(const std::string &pspDir, bool *exists) {
	std::vector<FileInfo> out;
	if (exists) *exists = false;
	Target t;
	if (!Resolve(pspDir, t)) return out;
	s32 dd = t.fs->DOpen(t.r.path);
	if (dd < 0) return out;
	if (exists) *exists = true;
	vfs::SceIoDirent ent;
	for (;;) {
		memset(&ent, 0, sizeof(ent));
		if (t.fs->DRead(dd, &ent) <= 0) break;
		std::string name(ent.d_name, strnlen(ent.d_name, sizeof(ent.d_name)));
		if (name.empty() || name == "." || name == "..") continue;
		FileInfo fi;
		fi.name = name;
		FillInfo(fi, ent.d_stat);
		out.push_back(fi);
	}
	t.fs->DClose(dd);
	return out;
}

bool ReadAll(const std::string &pspPath, std::vector<u8> &out) {
	out.clear();
	return io::ReadFile(pspPath, out);
}

s64 ReadInto(const std::string &pspPath, u8 *dst, u32 maxSize) {
	Target t;
	if (!Resolve(pspPath, t)) return -1;
	s32 fd = t.fs->Open(t.r.path, vfs::PSP_O_RDONLY, 0);
	if (fd < 0) return -1;
	s64 got = maxSize ? t.fs->Read(fd, dst, maxSize) : 0;
	t.fs->Close(fd);
	return got;
}

bool WriteAll(const std::string &pspPath, const void *data, size_t size) {
	Target t;
	if (!Resolve(pspPath, t)) return false;
	s32 fd = t.fs->Open(t.r.path, vfs::PSP_O_WRONLY | vfs::PSP_O_CREAT | vfs::PSP_O_TRUNC, 0777);
	if (fd < 0) return false;
	s64 put = size ? t.fs->Write(fd, data, size) : 0;
	t.fs->Close(fd);
	return put == (s64)size;
}

bool Mkdir(const std::string &pspPath) {
	Target t;
	if (!Resolve(pspPath, t)) return false;
	return t.fs->Mkdir(t.r.path, 0777) >= 0;
}

bool Remove(const std::string &pspPath) {
	Target t;
	if (!Resolve(pspPath, t)) return false;
	return t.fs->Remove(t.r.path) >= 0;
}

bool RemoveTree(const std::string &pspPath) {
	for (const FileInfo &f : List(pspPath)) {
		std::string child = pspPath + "/" + f.name;
		if (f.isDir) RemoveTree(child);
		else Remove(child);
	}
	Target t;
	if (!Resolve(pspPath, t)) return false;
	return t.fs->Rmdir(t.r.path) >= 0;
}

u64 TreeSize(const std::string &pspPath) {
	u64 total = 0;
	for (const FileInfo &f : List(pspPath)) {
		if (f.isDir) total += TreeSize(pspPath + "/" + f.name);
		else total += (u64)f.size;
	}
	return total;
}

}  // namespace utility::savefs
