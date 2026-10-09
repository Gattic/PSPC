// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/FileSystems/ISOFileSystem.cpp and the UMD ioctls of Core/HLE/sceIo.cpp),
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
#include "fs/iso_fs.h"

#include <algorithm>
#include <cstring>

#include "common/log.h"
#include "fs/path_util.h"

namespace vfs {

namespace {

u32 LE32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
void PutLE32(void *dst, u32 v) {
	u8 *p = (u8 *)dst;
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

// Directory record fields (ECMA-119 9.1).
constexpr int kRecLba = 2;
constexpr int kRecSize = 10;
constexpr int kRecDate = 18;
constexpr int kRecFlags = 25;
constexpr int kRecNameLen = 32;
constexpr int kRecName = 33;
constexpr int kPvdRootRecord = 156;
constexpr u32 kMaxDirBytes = 16u << 20;

u32 ParseHex(std::string_view s) {
	size_t i = 0;
	if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		i = 2;
	}
	u64 v = 0;
	for (; i < s.size(); i++) {
		char c = s[i];
		int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else break;
		v = std::min<u64>((v << 4) | (u64)d, 0xFFFFFFFFull);
	}
	return (u32)v;
}

void DateFromRecord(const u8 *d, ScePspDateTime *out) {
	memset(out, 0, sizeof(*out));
	if (d[1] < 1 || d[1] > 12 || d[2] < 1 || d[2] > 31) {
		return;  // many images leave the date zeroed
	}
	// The record holds local time (plus a GMT offset we don't need); the PSP shows it as is.
	out->year = (u16)(1900 + d[0]);
	out->month = d[1];
	out->day = d[2];
	out->hour = d[3];
	out->minute = d[4];
	out->second = d[5];
}

}  // namespace

bool ParseSceLbnPath(std::string_view path, u32 *startLba, u32 *size) {
	// "/sce_lbn" HEX* ANY* "_size" HEX* ANY*, under 32 characters (as the firmware's parser).
	if (path.substr(0, 8) != "/sce_lbn" || path.size() >= 32) {
		return false;
	}
	size_t sizePos = path.find("_size");
	if (sizePos == std::string_view::npos) {
		return false;
	}
	*startLba = ParseHex(path.substr(8));
	*size = ParseHex(path.substr(sizePos + 5));
	return true;
}

struct IsoFileSystem::Node {
	std::string name;
	u32 lba = 0;
	u32 size = 0;
	bool isDir = false;
	u8 date[7] = {};
	Node *parent = nullptr;
	bool loaded = false;
	std::vector<std::unique_ptr<Node>> children;
};

struct IsoFileSystem::OpenFile {
	std::mutex m;
	const Node *node = nullptr;
	u32 flags = 0;
	bool sectorMode = false;
	bool raw = false;
	bool device = false;
	u32 lba = 0;
	u64 bytes = 0;
	u64 pos = 0;

	u64 SizeUnits() const { return sectorMode ? (bytes + kSectorSize - 1) / kSectorSize : bytes; }
};

struct IsoFileSystem::OpenDir {
	std::vector<const Node *> entries;
	size_t index = 0;
};

IsoFileSystem::IsoFileSystem(std::shared_ptr<BlockDevice> device) : device_(std::move(device)) {
	if (!device_) {
		error_ = "no block device";
		return;
	}
	// The primary volume descriptor normally sits at sector 16; tolerate others before it.
	std::vector<u8> sector(kSectorSize);
	for (u32 lba = 16; lba < 32 && lba < device_->NumBlocks(); lba++) {
		if (!device_->ReadBlock(lba, sector.data())) {
			break;
		}
		if (memcmp(sector.data() + 1, "CD001", 5) != 0 || sector[0] == 255) {
			break;
		}
		if (sector[0] == 1) {
			pvd_ = sector;
			break;
		}
	}
	if (pvd_.empty()) {
		error_ = "no ISO9660 primary volume descriptor";
		return;
	}
	const u8 *rec = pvd_.data() + kPvdRootRecord;
	root_ = std::make_unique<Node>();
	root_->isDir = true;
	root_->lba = LE32(rec + kRecLba);
	root_->size = LE32(rec + kRecSize);
	memcpy(root_->date, rec + kRecDate, 7);
}

IsoFileSystem::~IsoFileSystem() = default;

std::string IsoFileSystem::VolumeId() const {
	if (pvd_.empty()) {
		return std::string();
	}
	std::string id((const char *)pvd_.data() + 40, 32);
	while (!id.empty() && (id.back() == ' ' || id.back() == '\0')) {
		id.pop_back();
	}
	return id;
}

void IsoFileSystem::LoadDir(Node *dir) {
	if (dir->loaded) {
		return;
	}
	dir->loaded = true;
	const u64 imageBytes = device_->SizeBytes();
	const u32 dirBytes = std::min(dir->size, kMaxDirBytes);
	const u32 numSectors = (dirBytes + kSectorSize - 1) / kSectorSize;
	std::vector<u8> sec(kSectorSize);
	for (u32 s = 0; s < numSectors; s++) {
		if (!device_->ReadBlock(dir->lba + s, sec.data())) {
			LOGW("fs", "ISO: can't read directory sector %u", dir->lba + s);
			return;
		}
		u32 off = 0;
		while (off < kSectorSize) {
			const u8 *r = sec.data() + off;
			u32 len = r[0];
			if (len == 0) {
				break;  // rest of the sector is padding
			}
			u32 nameLen = r[kRecNameLen];
			if (len < kRecName || off + len > kSectorSize || kRecName + nameLen > len) {
				LOGW("fs", "ISO: corrupt directory record in sector %u", dir->lba + s);
				break;
			}
			off += len;
			const char *name = (const char *)r + kRecName;
			if (nameLen == 1 && (name[0] == 0 || name[0] == 1)) {
				continue;  // "." and ".."
			}
			auto node = std::make_unique<Node>();
			std::string_view n(name, nameLen);
			size_t semi = n.find(';');
			if (semi != std::string_view::npos) {
				n = n.substr(0, semi);
				if (!n.empty() && n.back() == '.') {
					n.remove_suffix(1);  // "NAME.;1" names a file without extension
				}
			}
			node->name = std::string(n);
			node->lba = LE32(r + kRecLba);
			node->size = LE32(r + kRecSize);
			node->isDir = (r[kRecFlags] & 2) != 0;
			memcpy(node->date, r + kRecDate, 7);
			node->parent = dir;
			if (node->isDir) {
				bool loop = false;
				for (const Node *a = dir; a; a = a->parent) {
					loop = loop || a->lba == node->lba;
				}
				if (loop) {
					LOGW("fs", "ISO: directory '%s' loops back, skipped", node->name.c_str());
					continue;
				}
			} else {
				// Records are untrusted; never claim more data than the image holds.
				const u64 first = (u64)node->lba * kSectorSize;
				const u64 avail = first >= imageBytes ? 0 : imageBytes - first;
				if (node->size > avail) {
					LOGW("fs", "ISO: file '%s' runs past the end of the image", node->name.c_str());
					node->size = (u32)avail;
				}
			}
			dir->children.push_back(std::move(node));
		}
	}
}

const IsoFileSystem::Node *IsoFileSystem::Lookup(std::string_view path) {
	if (!root_) {
		return nullptr;
	}
	Node *cur = root_.get();
	for (const std::string &comp : SplitPspPathComponents(path)) {
		if (!cur->isDir) {
			return nullptr;
		}
		LoadDir(cur);
		Node *next = nullptr;
		for (auto &c : cur->children) {
			if (EqualsNoCase(c->name, comp)) {
				next = c.get();
				break;
			}
		}
		if (!next) {
			return nullptr;
		}
		cur = next;
	}
	return cur;
}

bool IsoFileSystem::GetExtent(std::string_view path, u32 *lba, u64 *size) {
	std::lock_guard<std::mutex> lock(treeMutex_);
	const Node *n = Lookup(path);
	if (!n) {
		return false;
	}
	*lba = n->lba;
	*size = n->size;
	return true;
}

s32 IsoFileSystem::ReadWholeFile(std::string_view path, std::vector<u8> &out) {
	s32 fd = Open(path, PSP_O_RDONLY, 0);
	if (fd < 0) {
		return fd;
	}
	HandleInfo info;
	GetHandleInfo(fd, &info);
	out.resize((size_t)info.size);
	s64 n = out.empty() ? 0 : Read(fd, out.data(), out.size());
	Close(fd);
	if (n < 0) {
		out.clear();
		return (s32)n;
	}
	out.resize((size_t)n);
	return 0;
}

s32 IsoFileSystem::AllocHandle() {
	// Caller holds mutex_.
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

std::shared_ptr<IsoFileSystem::OpenFile> IsoFileSystem::GetFile(s32 fd) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = files_.find(fd);
	return it == files_.end() ? nullptr : it->second;
}

s32 IsoFileSystem::Open(std::string_view path, u32 flags, s32 mode) {
	if (flags & PSP_O_WRONLY) {
		return SCE_ERRNO_INVALID_FLAG;
	}
	auto f = std::make_shared<OpenFile>();
	f->flags = flags;
	f->sectorMode = (flags & PSPC_O_SECTOR_MODE) != 0;
	u32 lbnStart, lbnSize;
	std::vector<std::string> comps = SplitPspPathComponents(path);
	if (path.empty()) {
		f->device = true;
		f->sectorMode = true;
		f->bytes = device_->SizeBytes();
	} else if (comps.size() == 1 && ParseSceLbnPath("/" + comps[0], &lbnStart, &lbnSize)) {
		if (lbnStart > device_->NumBlocks()) {
			return SCE_ERRNO_ENOENT;
		}
		const u64 first = (u64)lbnStart * kSectorSize;
		const u64 imageBytes = device_->SizeBytes();
		f->raw = true;
		f->lba = lbnStart;
		f->bytes = std::min<u64>(lbnSize, first >= imageBytes ? 0 : imageBytes - first);
	} else {
		std::lock_guard<std::mutex> lock(treeMutex_);
		const Node *n = Lookup(path);
		if (!n) {
			return SCE_ERRNO_ENOENT;
		}
		f->node = n;
		f->lba = n->lba;
		f->bytes = n->size;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	s32 h = AllocHandle();
	files_[h] = std::move(f);
	return h;
}

s32 IsoFileSystem::Close(s32 fd) {
	std::lock_guard<std::mutex> lock(mutex_);
	return files_.erase(fd) ? 0 : SCE_KERNEL_ERROR_BADF;
}

bool IsoFileSystem::ReadBytes(u64 offset, u64 size, u8 *out) {
	bool ok = true;
	u8 sec[kSectorSize];
	u32 lba = (u32)(offset / kSectorSize);
	u32 inSec = (u32)(offset % kSectorSize);
	if (inSec && size) {
		u32 n = (u32)std::min<u64>(size, kSectorSize - inSec);
		ok = device_->ReadBlock(lba++, sec) && ok;
		memcpy(out, sec + inSec, n);
		out += n;
		size -= n;
	}
	while (size >= kSectorSize) {
		u32 count = (u32)std::min<u64>(size / kSectorSize, 4096);
		ok = device_->ReadBlocks(lba, count, out) && ok;
		lba += count;
		out += (size_t)count * kSectorSize;
		size -= (u64)count * kSectorSize;
	}
	if (size) {
		ok = device_->ReadBlock(lba, sec) && ok;
		memcpy(out, sec, (size_t)size);
	}
	return ok;
}

s64 IsoFileSystem::ReadLocked(OpenFile &f, void *buf, u64 size) {
	if (!(f.flags & PSP_O_RDONLY)) {
		return SCE_KERNEL_ERROR_BADF;
	}
	const u64 units = f.SizeUnits();
	if (size == 0 || f.pos >= units) {
		return 0;
	}
	u64 n = std::min(size, units - f.pos);
	bool ok;
	if (f.sectorMode) {
		ok = true;
		u8 *out = (u8 *)buf;
		u64 done = 0;
		while (done < n) {
			u32 count = (u32)std::min<u64>(n - done, 4096);
			ok = device_->ReadBlocks((u32)(f.lba + f.pos + done), count, out + done * kSectorSize) && ok;
			done += count;
		}
	} else {
		ok = ReadBytes((u64)f.lba * kSectorSize + f.pos, n, (u8 *)buf);
	}
	if (!ok) {
		return SCE_ERRNO_EIO;
	}
	f.pos += n;
	return (s64)n;
}

s64 IsoFileSystem::Read(s32 fd, void *buf, u64 size) {
	auto f = GetFile(fd);
	if (!f) {
		return SCE_KERNEL_ERROR_BADF;
	}
	std::lock_guard<std::mutex> lock(f->m);
	return ReadLocked(*f, buf, size);
}

s64 IsoFileSystem::Write(s32 fd, const void *buf, u64 size) {
	// Nothing on a UMD is open for writing.
	return SCE_KERNEL_ERROR_BADF;
}

s64 IsoFileSystem::SeekLocked(OpenFile &f, s64 offset, s32 whence) {
	s64 base;
	switch (whence) {
	case PSP_SEEK_SET: base = 0; break;
	case PSP_SEEK_CUR: base = (s64)f.pos; break;
	case PSP_SEEK_END: base = (s64)f.SizeUnits(); break;
	default: return SCE_KERNEL_ERROR_INVAL;
	}
	if ((offset < 0 && base + offset < 0) || (offset > 0 && base > INT64_MAX - offset)) {
		return -1;
	}
	f.pos = (u64)(base + offset);
	return (s64)f.pos;
}

s64 IsoFileSystem::Seek(s32 fd, s64 offset, s32 whence) {
	auto f = GetFile(fd);
	if (!f) {
		return SCE_KERNEL_ERROR_BADF;
	}
	std::lock_guard<std::mutex> lock(f->m);
	return SeekLocked(*f, offset, whence);
}

s32 IsoFileSystem::GetHandleInfo(s32 fd, HandleInfo *out) {
	auto f = GetFile(fd);
	if (!f) {
		return SCE_KERNEL_ERROR_BADF;
	}
	std::lock_guard<std::mutex> lock(f->m);
	out->flags = f->flags;
	out->sectorMode = f->sectorMode;
	out->rawLbn = f->raw;
	out->device = f->device;
	out->startLba = f->lba;
	out->size = f->SizeUnits();
	out->position = f->pos;
	return 0;
}

s32 IsoFileSystem::Ioctl(s32 fd, u32 cmd, const void *in, u32 inLen, void *out, u32 outLen) {
	auto f = GetFile(fd);
	if (!f) {
		return SCE_KERNEL_ERROR_BADF;
	}
	std::lock_guard<std::mutex> lock(f->m);
	auto putU32 = [&](u32 v) -> s32 {
		if (!out || outLen < 4) {
			return SCE_ERRNO_EINVAL;
		}
		PutLE32(out, v);
		return 0;
	};
	switch (cmd) {
	case UMD_IOCTL_GET_PVD:
		if (f->sectorMode) {
			return SCE_ERRNO_FUNCTION_NOT_SUPPORTED;
		}
		if (!out || outLen < kSectorSize) {
			return SCE_ERRNO_EINVAL;
		}
		if (pvd_.empty()) {
			return device_->ReadBlock(16, (u8 *)out) ? 0 : SCE_ERRNO_EIO;
		}
		memcpy(out, pvd_.data(), kSectorSize);
		return 0;

	case UMD_IOCTL_GET_PATH_TABLE: {
		if (f->sectorMode) {
			return SCE_ERRNO_FUNCTION_NOT_SUPPORTED;
		}
		if (pvd_.empty()) {
			return SCE_ERRNO_EIO;
		}
		const u32 tableSize = LE32(pvd_.data() + 132);
		const u32 tableLba = LE32(pvd_.data() + 140);
		if (!out || outLen < tableSize) {
			return SCE_ERRNO_EINVAL;
		}
		return ReadBytes((u64)tableLba * kSectorSize, tableSize, (u8 *)out) ? 0 : SCE_ERRNO_EIO;
	}

	case UMD_IOCTL_GET_SECTOR_SIZE:
		return putU32(kSectorSize);

	case UMD_IOCTL_TELL:
	case UMD_IOCTL_SECTOR_TELL:
		return putU32((u32)f->pos);

	case UMD_IOCTL_GET_START_LBA:
		return putU32(f->lba);

	case UMD_IOCTL_GET_SIZE: {
		if (!out || outLen < 8) {
			return SCE_ERRNO_EINVAL;
		}
		u64 size = f->SizeUnits();
		PutLE32(out, (u32)size);
		PutLE32((u8 *)out + 4, (u32)(size >> 32));
		return 0;
	}

	case UMD_IOCTL_SEEK:
	case UMD_IOCTL_SECTOR_SEEK: {
		// The firmware reads a 16-byte struct even when inLen is 4; we read what we were given.
		if (!in || inLen < 4) {
			return SCE_ERRNO_EINVAL;
		}
		u8 info[16] = {};
		memcpy(info, in, std::min<u32>(inLen, sizeof(info)));
		s64 offset = (s64)((u64)LE32(info) | ((u64)LE32(info + 4) << 32));
		s32 whence = (s32)LE32(info + 12);
		s64 base;
		switch (whence) {
		case PSP_SEEK_SET: base = 0; break;
		case PSP_SEEK_CUR: base = (s64)f->pos; break;
		case PSP_SEEK_END: base = (s64)f->SizeUnits(); break;
		default: return SCE_KERNEL_ERROR_INVAL;
		}
		// Not allowed to leave the file with these.
		const s64 target = base + offset;
		if (target < 0 || (u64)target > f->SizeUnits()) {
			return cmd == UMD_IOCTL_SEEK ? SCE_ERRNO_EIO : SCE_ERRNO_INVALID_FILE_SIZE;
		}
		f->pos = (u64)target;
		return 0;
	}

	case UMD_IOCTL_READ:
	case UMD_IOCTL_SECTOR_READ: {
		if (!in || inLen < 4) {
			return SCE_ERRNO_EINVAL;
		}
		const u32 count = LE32((const u8 *)in);
		const u64 bytes = f->sectorMode ? (u64)count * kSectorSize : count;
		if ((cmd == UMD_IOCTL_SECTOR_READ && count == 0) || !out || bytes > outLen) {
			return SCE_ERRNO_EINVAL;
		}
		return (s32)ReadLocked(*f, out, count);
	}

	case UMD_IOCTL_SET_CACHE:
		return 0;
	}
	return SCE_ERRNO_FUNCTION_NOT_SUPPORTED;
}

void IsoFileSystem::FillStat(const Node *node, SceIoStat *out) const {
	// Everything on a UMD reads as r-x, files included.
	out->st_mode = (s32)((node->isDir ? FIO_S_IFDIR : FIO_S_IFREG) | 0555);
	out->st_attr = node->isDir ? FIO_SO_IFDIR : FIO_SO_IFREG;
	out->st_size = node->size;
	DateFromRecord(node->date, &out->st_ctime);
	out->st_atime = out->st_ctime;
	out->st_mtime = out->st_ctime;
	out->st_private[0] = node->lba;
}

s32 IsoFileSystem::GetStat(std::string_view path, SceIoStat *out) {
	u32 lbnStart, lbnSize;
	std::vector<std::string> comps = SplitPspPathComponents(path);
	if (path.empty()) {
		Node dev;
		dev.size = device_->NumBlocks();
		FillStat(&dev, out);
		return 0;
	}
	if (comps.size() == 1 && ParseSceLbnPath("/" + comps[0], &lbnStart, &lbnSize)) {
		Node raw;
		raw.lba = lbnStart;
		raw.size = lbnSize;
		FillStat(&raw, out);
		out->st_mode = (s32)(FIO_S_IFREG | 0444);
		return 0;
	}
	std::lock_guard<std::mutex> lock(treeMutex_);
	const Node *n = Lookup(path);
	if (!n) {
		return SCE_ERRNO_ENOENT;
	}
	FillStat(n, out);
	return 0;
}

s32 IsoFileSystem::ChStat(std::string_view path, const SceIoStat &in, u32 bits) { return SCE_ERRNO_EROFS; }
s32 IsoFileSystem::Remove(std::string_view path) { return SCE_ERRNO_EROFS; }
s32 IsoFileSystem::Mkdir(std::string_view path, s32 mode) { return SCE_ERRNO_EROFS; }
s32 IsoFileSystem::Rmdir(std::string_view path) { return SCE_ERRNO_EROFS; }
s32 IsoFileSystem::Rename(std::string_view from, std::string_view to) { return SCE_ERRNO_EROFS; }

s32 IsoFileSystem::DOpen(std::string_view path) {
	auto d = std::make_shared<OpenDir>();
	{
		std::lock_guard<std::mutex> lock(treeMutex_);
		const Node *n = Lookup(path);
		if (!n) {
			return SCE_ERRNO_ENOENT;
		}
		if (!n->isDir) {
			return SCE_ERRNO_ENOTDIR;
		}
		LoadDir(const_cast<Node *>(n));
		for (auto &c : n->children) {
			d->entries.push_back(c.get());
		}
	}
	std::lock_guard<std::mutex> lock(mutex_);
	s32 h = AllocHandle();
	dirs_[h] = std::move(d);
	return h;
}

s32 IsoFileSystem::DRead(s32 dd, SceIoDirent *out) {
	std::shared_ptr<OpenDir> d;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = dirs_.find(dd);
		if (it == dirs_.end()) {
			return SCE_KERNEL_ERROR_BADF;
		}
		d = it->second;
	}
	std::lock_guard<std::mutex> lock(treeMutex_);
	if (d->index >= d->entries.size()) {
		out->d_name[0] = '\0';
		return 0;
	}
	const Node *n = d->entries[d->index++];
	FillStat(n, &out->d_stat);
	size_t len = std::min(n->name.size(), sizeof(out->d_name) - 1);
	memcpy(out->d_name, n->name.data(), len);
	out->d_name[len] = '\0';
	return 1;
}

s32 IsoFileSystem::DClose(s32 dd) {
	std::lock_guard<std::mutex> lock(mutex_);
	return dirs_.erase(dd) ? 0 : SCE_KERNEL_ERROR_BADF;
}

}  // namespace vfs
