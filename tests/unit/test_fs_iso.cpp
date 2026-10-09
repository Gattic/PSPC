// SPDX-License-Identifier: GPL-2.0-or-later
// Block devices (ISO, CSO v1/v2, ZSO, DAX) and IsoFileSystem, on images built by the test.
#include <cstdio>
#include <cstring>
#include <thread>

#include "fs/block_device.h"
#include "fs/iso_fs.h"
#include "test_fs_helpers.h"
#include "testing.h"

using namespace vfs;
using fstest::TestIso;

namespace {

const std::vector<fstest::IsoInput> &Inputs() {
	static const std::vector<fstest::IsoInput> inputs = [] {
		std::vector<fstest::IsoInput> in;
		in.push_back({"UMD_DATA.BIN", fstest::Text(20, 64)});
		in.push_back({"PSP_GAME/PARAM.SFO", fstest::Binary(21, 700)});
		in.push_back({"PSP_GAME/ICON0.PNG", fstest::Binary(22, 5000)});
		in.push_back({"PSP_GAME/SYSDIR/EBOOT.BIN", fstest::Text(23, 70000)});
		in.push_back({"PSP_GAME/SYSDIR/BOOT.BIN", fstest::Binary(24, 4096)});
		in.push_back({"PSP_GAME/USRDIR/DATA/LEVEL01.DAT", fstest::Binary(25, 20000)});
		in.push_back({"PSP_GAME/USRDIR/EMPTY.TXT", {}});
		in.push_back({"PSP_GAME/USRDIR/NOEXT", fstest::Text(26, 300)});
		in.push_back({"PSP_GAME/USRDIR/EMPTYDIR/", {}});
		for (int i = 0; i < 80; i++) {
			char name[64];
			snprintf(name, sizeof(name), "PSP_GAME/USRDIR/MANY/F%03d.BIN", i);
			in.push_back({name, fstest::Text(100 + i, 50 + i * 7)});
		}
		return in;
	}();
	return inputs;
}

const TestIso &Iso() {
	static const TestIso iso = fstest::BuildIso(Inputs());
	return iso;
}

const std::vector<u8> &FileData(const char *path) {
	for (const auto &in : Inputs()) {
		if (in.path == path) return in.data;
	}
	static const std::vector<u8> none;
	return none;
}

std::vector<std::string> ListDir(FileSystem &fs, const char *path, std::vector<SceIoDirent> *ents = nullptr) {
	std::vector<std::string> names;
	s32 dd = fs.DOpen(path);
	CHECK(dd > 0);
	if (dd <= 0) return names;
	SceIoDirent e;
	memset(&e, 0xCC, sizeof(e));
	s32 r;
	while ((r = fs.DRead(dd, &e)) == 1) {
		names.push_back(e.d_name);
		if (ents) ents->push_back(e);
		CHECK_EQ(e.d_private, 0xCCCCCCCCu);
	}
	CHECK_EQ(r, 0);
	CHECK_EQ(e.d_name[0], '\0');
	CHECK_EQ(fs.DClose(dd), 0);
	return names;
}

// Reads a whole file in awkward chunk sizes.
std::vector<u8> ReadInChunks(FileSystem &fs, s32 fd) {
	static const u32 sizes[] = {1, 7, 2047, 2049, 4096, 100, 2048, 13, 9000};
	std::vector<u8> out;
	for (int i = 0;; i++) {
		u32 want = sizes[i % 9];
		std::vector<u8> buf(want);
		s64 n = fs.Read(fd, buf.data(), want);
		if (n <= 0) {
			CHECK_EQ(n, (s64)0);
			break;
		}
		out.insert(out.end(), buf.begin(), buf.begin() + (size_t)n);
	}
	return out;
}

void SeekInfo(u8 (&info)[16], s64 offset, u32 whence) {
	memset(info, 0, sizeof(info));
	fstest::Put32LE(info, (u32)offset);
	fstest::Put32LE(info + 4, (u32)((u64)offset >> 32));
	fstest::Put32LE(info + 12, whence);
}

u32 GetU32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

// Every sector of the device equals the image.
void CheckDeviceMatches(BlockDevice &dev, const std::vector<u8> &image) {
	const u32 blocks = (u32)((image.size() + 2047) / 2048);
	CHECK_EQ(dev.NumBlocks(), blocks);
	CHECK_EQ(dev.SizeBytes(), (u64)image.size());
	std::vector<u8> buf(7 * 2048);
	bool same = true;
	for (u32 lba = 0; lba < blocks; lba += 7) {
		u32 n = std::min<u32>(7, blocks - lba);
		CHECK(dev.ReadBlocks(lba, n, buf.data()));
		size_t bytes = std::min<size_t>((size_t)n * 2048, image.size() - (size_t)lba * 2048);
		same = same && !memcmp(buf.data(), image.data() + (size_t)lba * 2048, bytes);
		for (size_t i = bytes; i < (size_t)n * 2048; i++) same = same && buf[i] == 0;
	}
	CHECK(same);
	// Single blocks out of order hit the frame cache in a different pattern.
	for (u32 lba : {blocks - 1, 0u, blocks / 2, 17u, blocks / 2 + 1, 3u}) {
		CHECK(dev.ReadBlock(lba, buf.data()));
		size_t bytes = std::min<size_t>(2048, image.size() - (size_t)lba * 2048);
		CHECK(!memcmp(buf.data(), image.data() + (size_t)lba * 2048, bytes));
	}
	// Past the end: zeros and failure.
	memset(buf.data(), 0x55, 4096);
	CHECK(!dev.ReadBlocks(blocks - 1, 2, buf.data()));
	CHECK_EQ(buf[2048], 0);
	CHECK(!dev.ReadBlock(blocks, buf.data()));
	CHECK_EQ(buf[0], 0);
}

void CheckIsoFs(IsoFileSystem &fs) {
	const TestIso &iso = Iso();
	const u32 blocks = (u32)(iso.image.size() / 2048);
	CHECK(fs.IsValid());
	CHECK(fs.Caps() & FS_CAP_READ_ONLY);
	CHECK(fs.Caps() & FS_CAP_BLOCK_DEVICE);
	CHECK(fs.VolumeId() == "PSPC_TEST");

	// Listings, in disc order, without "." and "..".
	std::vector<SceIoDirent> ents;
	std::vector<std::string> root = ListDir(fs, "/");
	CHECK(root == std::vector<std::string>({"PSP_GAME", "UMD_DATA.BIN"}));
	CHECK(ListDir(fs, "") == root);
	std::vector<std::string> game = ListDir(fs, "/PSP_GAME", &ents);
	CHECK(game == std::vector<std::string>({"ICON0.PNG", "PARAM.SFO", "SYSDIR", "USRDIR"}));
	if (ents.size() == 4) {
		CHECK_EQ(ents[0].d_stat.st_attr, (u32)FIO_SO_IFREG);
		CHECK_EQ(ents[0].d_stat.st_size, (s64)5000);
		CHECK_EQ(ents[0].d_stat.st_private[0], iso.lba.at("PSP_GAME/ICON0.PNG"));
		CHECK_EQ(ents[0].d_stat.st_private[1], 0xCCCCCCCCu);
		CHECK_EQ(ents[2].d_stat.st_attr, (u32)FIO_SO_IFDIR);
		CHECK_EQ(ents[2].d_stat.st_mode, (s32)(FIO_S_IFDIR | 0555));
	}
	CHECK(ListDir(fs, "/PSP_GAME/USRDIR") == std::vector<std::string>({"DATA", "EMPTY.TXT", "EMPTYDIR", "MANY", "NOEXT"}));
	CHECK(ListDir(fs, "/PSP_GAME/USRDIR/EMPTYDIR").empty());
	std::vector<std::string> many = ListDir(fs, "/psp_game/usrdir/many");  // spans two directory sectors
	CHECK_EQ(many.size(), (size_t)80);
	if (many.size() == 80) {
		CHECK(many[0] == "F000.BIN");
		CHECK(many[79] == "F079.BIN");
	}
	CHECK_EQ(fs.DOpen("/NOPE"), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.DOpen("/UMD_DATA.BIN"), SCE_ERRNO_ENOTDIR);
	SceIoDirent e;
	CHECK_EQ(fs.DRead(12345, &e), SCE_KERNEL_ERROR_BADF);
	CHECK_EQ(fs.DClose(12345), SCE_KERNEL_ERROR_BADF);

	// Stat: r-x everywhere, the ISO date, the LBA in st_private[0], the rest untouched.
	const u32 ebootLba = iso.lba.at("PSP_GAME/SYSDIR/EBOOT.BIN");
	SceIoStat st;
	memset(&st, 0xCC, sizeof(st));
	CHECK_EQ(fs.GetStat("/PSP_GAME/SYSDIR/EBOOT.BIN", &st), 0);
	CHECK_EQ(st.st_mode, (s32)0x216D);
	CHECK_EQ(st.st_attr, 0x20u);
	CHECK_EQ(st.st_size, (s64)70000);
	CHECK_EQ(st.st_private[0], ebootLba);
	CHECK_EQ(st.st_private[1], 0xCCCCCCCCu);
	CHECK_EQ(st.st_private[5], 0xCCCCCCCCu);
	CHECK_EQ(st.st_mtime.year, 2008);
	CHECK_EQ(st.st_mtime.month, 6);
	CHECK_EQ(st.st_mtime.day, 28);
	CHECK_EQ(st.st_mtime.hour, 12);
	CHECK_EQ(st.st_mtime.minute, 34);
	CHECK_EQ(st.st_mtime.second, 56);
	CHECK_EQ(st.st_ctime.year, 2008);
	CHECK_EQ(fs.GetStat("/PSP_GAME/USRDIR", &st), 0);
	CHECK_EQ(st.st_mode, (s32)0x116D);
	CHECK_EQ(st.st_attr, 0x10u);
	CHECK_EQ(st.st_private[0], iso.lba.at("PSP_GAME/USRDIR"));
	CHECK_EQ(fs.GetStat("psp_game/usrdir/noext", &st), 0);
	CHECK_EQ(st.st_size, (s64)300);
	CHECK_EQ(fs.GetStat("/PSP_GAME/USRDIR/EMPTY.TXT", &st), 0);
	CHECK_EQ(st.st_size, (s64)0);
	CHECK_EQ(fs.GetStat("/PSP_GAME/NOPE.BIN", &st), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.GetStat("/UMD_DATA.BIN/X", &st), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.GetStat("", &st), 0);
	CHECK_EQ(st.st_size, (s64)blocks);
	u32 lba = 0;
	u64 size = 0;
	CHECK(fs.GetExtent("/PSP_GAME/SYSDIR/EBOOT.BIN", &lba, &size));
	CHECK_EQ(lba, ebootLba);
	CHECK_EQ(size, (u64)70000);
	std::vector<u8> whole;
	CHECK_EQ(fs.ReadWholeFile("/PSP_GAME/USRDIR/DATA/LEVEL01.DAT", whole), 0);
	CHECK(whole == FileData("PSP_GAME/USRDIR/DATA/LEVEL01.DAT"));

	// Files read back across sector boundaries, through messy but valid paths.
	const std::vector<u8> &eboot = FileData("PSP_GAME/SYSDIR/EBOOT.BIN");
	s32 fd = fs.Open("PSP_GAME//sysdir/../SYSDIR/./eboot.bin", PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	CHECK(ReadInChunks(fs, fd) == eboot);
	std::vector<u8> buf(10000);
	CHECK_EQ(fs.Seek(fd, 2000, PSP_SEEK_SET), (s64)2000);
	CHECK_EQ(fs.Read(fd, buf.data(), 5000), (s64)5000);
	CHECK(!memcmp(buf.data(), eboot.data() + 2000, 5000));
	CHECK_EQ(fs.Seek(fd, -100, PSP_SEEK_END), (s64)69900);
	CHECK_EQ(fs.Read(fd, buf.data(), 1000), (s64)100);  // clamped at EOF
	CHECK(!memcmp(buf.data(), eboot.data() + 69900, 100));
	CHECK_EQ(fs.Read(fd, buf.data(), 1000), (s64)0);
	CHECK_EQ(fs.Seek(fd, -30000, PSP_SEEK_CUR), (s64)40000);
	CHECK_EQ(fs.Seek(fd, -50000, PSP_SEEK_CUR), (s64)-1);  // position stays at 40000
	CHECK_EQ(fs.Read(fd, buf.data(), 4), (s64)4);
	CHECK(!memcmp(buf.data(), eboot.data() + 40000, 4));
	CHECK_EQ(fs.Seek(fd, 1000000, PSP_SEEK_SET), (s64)1000000);
	CHECK_EQ(fs.Read(fd, buf.data(), 10), (s64)0);
	CHECK_EQ(fs.Seek(fd, 0, 7), SCE_KERNEL_ERROR_INVAL);
	CHECK_EQ(fs.Write(fd, buf.data(), 1), SCE_KERNEL_ERROR_BADF);
	HandleInfo hi;
	CHECK_EQ(fs.GetHandleInfo(fd, &hi), 0);
	CHECK(!hi.sectorMode && !hi.rawLbn && !hi.device);
	CHECK_EQ(hi.startLba, ebootLba);
	CHECK_EQ(hi.size, (u64)70000);

	// UMD ioctls on a file.
	u8 out[4096], info[16];
	u32 count;
	CHECK_EQ(fs.Seek(fd, 10, PSP_SEEK_SET), (s64)10);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_GET_START_LBA, nullptr, 0, out, 4), 0);
	CHECK_EQ(GetU32(out), ebootLba);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_GET_SECTOR_SIZE, nullptr, 0, out, 4), 0);
	CHECK_EQ(GetU32(out), 2048u);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_GET_SIZE, nullptr, 0, out, 8), 0);
	CHECK_EQ(GetU32(out), 70000u);
	CHECK_EQ(GetU32(out + 4), 0u);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_TELL, nullptr, 0, out, 4), 0);
	CHECK_EQ(GetU32(out), 10u);
	SeekInfo(info, 4096, PSP_SEEK_SET);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SEEK, info, 16, nullptr, 0), 0);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SECTOR_TELL, nullptr, 0, out, 4), 0);
	CHECK_EQ(GetU32(out), 4096u);
	SeekInfo(info, 70001, PSP_SEEK_SET);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SEEK, info, 16, nullptr, 0), SCE_ERRNO_EIO);
	SeekInfo(info, -96, PSP_SEEK_CUR);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SEEK, info, 16, nullptr, 0), 0);  // 4000
	count = 100;
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_READ, &count, 4, out, sizeof(out)), 100);
	CHECK(!memcmp(out, eboot.data() + 4000, 100));
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_READ, &count, 4, out, 50), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_GET_PVD, nullptr, 0, out, 2048), 0);
	CHECK(!memcmp(out, iso.image.data() + 16 * 2048, 2048));
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_GET_PVD, nullptr, 0, out, 2047), SCE_ERRNO_EINVAL);
	memset(out, 0, sizeof(out));
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_GET_PATH_TABLE, nullptr, 0, out, sizeof(out)), 0);
	CHECK(!memcmp(out, iso.pathTable.data(), iso.pathTable.size()));
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_GET_PATH_TABLE, nullptr, 0, out, 4), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SET_CACHE, nullptr, 0, nullptr, 0), 0);
	CHECK_EQ(fs.Ioctl(fd, 0x12345678, nullptr, 0, nullptr, 0), SCE_ERRNO_FUNCTION_NOT_SUPPORTED);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_GET_START_LBA, nullptr, 0, out, 3), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Close(fd), 0);
	CHECK_EQ(fs.Close(fd), SCE_KERNEL_ERROR_BADF);
	CHECK_EQ(fs.Read(fd, buf.data(), 1), SCE_KERNEL_ERROR_BADF);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_TELL, nullptr, 0, out, 4), SCE_KERNEL_ERROR_BADF);

	// Small and empty files, and errors.
	fd = fs.Open("/PSP_GAME/USRDIR/EMPTY.TXT", PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	CHECK_EQ(fs.Read(fd, buf.data(), 10), (s64)0);
	fs.Close(fd);
	fd = fs.Open("/UMD_DATA.BIN", PSP_O_RDONLY, 0);
	CHECK(ReadInChunks(fs, fd) == FileData("UMD_DATA.BIN"));
	fs.Close(fd);
	for (int i : {0, 41, 79}) {
		char name[64];
		snprintf(name, sizeof(name), "/PSP_GAME/USRDIR/MANY/F%03d.BIN", i);
		std::vector<u8> data;
		CHECK_EQ(fs.ReadWholeFile(name, data), 0);
		CHECK(data == FileData(name + 1));
	}
	CHECK_EQ(fs.Open("/PSP_GAME/NOPE", PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.Open("/UMD_DATA.BIN", PSP_O_WRONLY, 0), SCE_ERRNO_INVALID_FLAG);
	CHECK_EQ(fs.Open("/UMD_DATA.BIN", PSP_O_RDWR | PSP_O_CREAT, 0), SCE_ERRNO_INVALID_FLAG);
	CHECK_EQ(fs.Remove("/UMD_DATA.BIN"), SCE_ERRNO_EROFS);
	CHECK_EQ(fs.Mkdir("/X", 0777), SCE_ERRNO_EROFS);
	CHECK_EQ(fs.Rmdir("/PSP_GAME"), SCE_ERRNO_EROFS);
	CHECK_EQ(fs.Rename("/UMD_DATA.BIN", "/X"), SCE_ERRNO_EROFS);
	CHECK_EQ(fs.ChStat("/UMD_DATA.BIN", st, SCE_CST_MODE), SCE_ERRNO_EROFS);

	// Raw sector files: "/sce_lbn<start>_size<bytes>", hex with or without 0x.
	char lbn[64];
	snprintf(lbn, sizeof(lbn), "/sce_lbn0x%x_size0x%x", ebootLba, 70000);
	memset(&st, 0xCC, sizeof(st));
	CHECK_EQ(fs.GetStat(lbn, &st), 0);
	CHECK_EQ(st.st_size, (s64)70000);
	CHECK_EQ(st.st_private[0], ebootLba);
	CHECK_EQ(st.st_mode, (s32)(FIO_S_IFREG | 0444));
	fd = fs.Open(lbn, PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	CHECK(ReadInChunks(fs, fd) == eboot);
	CHECK_EQ(fs.GetHandleInfo(fd, &hi), 0);
	CHECK(hi.rawLbn && !hi.sectorMode);
	CHECK_EQ(fs.Seek(fd, -6, PSP_SEEK_END), (s64)69994);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_GET_START_LBA, nullptr, 0, out, 4), 0);
	CHECK_EQ(GetU32(out), ebootLba);
	fs.Close(fd);
	snprintf(lbn, sizeof(lbn), "sce_lbn%X_size%X", ebootLba, 3000);
	fd = fs.Open(lbn, PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	CHECK_EQ(fs.Read(fd, buf.data(), 5000), (s64)3000);
	CHECK(!memcmp(buf.data(), eboot.data(), 3000));
	fs.Close(fd);
	// A raw extent stops at the end of the disc; a start past it doesn't open.
	snprintf(lbn, sizeof(lbn), "/sce_lbn0x%x_size0x100000", blocks - 1);
	fd = fs.Open(lbn, PSP_O_RDONLY, 0);
	CHECK_EQ(fs.Read(fd, buf.data(), buf.size()), (s64)2048);
	fs.Close(fd);
	snprintf(lbn, sizeof(lbn), "/sce_lbn0x%x_size0x800", blocks + 1);
	CHECK_EQ(fs.Open(lbn, PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);
	// Raw in sector mode (umd0:): units are sectors.
	snprintf(lbn, sizeof(lbn), "/sce_lbn0x%x_size0x%x", ebootLba, 70000);
	fd = fs.Open(lbn, PSP_O_RDONLY | PSPC_O_SECTOR_MODE, 0);
	CHECK_EQ(fs.GetHandleInfo(fd, &hi), 0);
	CHECK(hi.sectorMode);
	CHECK_EQ(hi.size, (u64)35);
	CHECK_EQ(fs.Seek(fd, 1, PSP_SEEK_SET), (s64)1);
	CHECK_EQ(fs.Read(fd, buf.data(), 2), (s64)2);
	CHECK(!memcmp(buf.data(), iso.image.data() + (size_t)(ebootLba + 1) * 2048, 4096));
	fs.Close(fd);

	// The device itself (umd0: / disc0: opened as ""): sector mode over the whole disc.
	fd = fs.Open("", PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	CHECK_EQ(fs.GetHandleInfo(fd, &hi), 0);
	CHECK(hi.device && hi.sectorMode);
	CHECK_EQ(hi.size, (u64)blocks);
	CHECK_EQ(fs.Seek(fd, 16, PSP_SEEK_SET), (s64)16);
	CHECK_EQ(fs.Read(fd, buf.data(), 2), (s64)2);
	CHECK(!memcmp(buf.data(), iso.image.data() + 16 * 2048, 4096));
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SECTOR_TELL, nullptr, 0, out, 4), 0);
	CHECK_EQ(GetU32(out), 18u);
	SeekInfo(info, ebootLba, PSP_SEEK_SET);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SECTOR_SEEK, info, 16, nullptr, 0), 0);
	count = 2;
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SECTOR_READ, &count, 4, buf.data(), 4096), 2);
	CHECK(!memcmp(buf.data(), eboot.data(), 4096));
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SECTOR_READ, &count, 4, buf.data(), 4095), SCE_ERRNO_EINVAL);
	count = 0;
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SECTOR_READ, &count, 4, buf.data(), 4096), SCE_ERRNO_EINVAL);
	SeekInfo(info, blocks + 1, PSP_SEEK_SET);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SECTOR_SEEK, info, 16, nullptr, 0), SCE_ERRNO_INVALID_FILE_SIZE);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_GET_PVD, nullptr, 0, out, 2048), SCE_ERRNO_FUNCTION_NOT_SUPPORTED);
	CHECK_EQ(fs.Seek(fd, -1, PSP_SEEK_END), (s64)(blocks - 1));
	CHECK_EQ(fs.Read(fd, buf.data(), 4), (s64)1);
	CHECK(!memcmp(buf.data(), iso.image.data() + (size_t)(blocks - 1) * 2048, 2048));
	CHECK_EQ(fs.Read(fd, buf.data(), 4), (s64)0);
	fs.Close(fd);
	// A file opened in sector mode reads its own extent by sectors.
	fd = fs.Open("/PSP_GAME/SYSDIR/BOOT.BIN", PSP_O_RDONLY | PSPC_O_SECTOR_MODE, 0);
	CHECK_EQ(fs.Read(fd, buf.data(), 5), (s64)2);
	CHECK(!memcmp(buf.data(), FileData("PSP_GAME/SYSDIR/BOOT.BIN").data(), 4096));
	fs.Close(fd);
}

void CheckImage(std::unique_ptr<BlockDevice> dev, ImageFormat format) {
	CHECK(dev != nullptr);
	if (!dev) return;
	CHECK(dev->Format() == format);
	CheckDeviceMatches(*dev, Iso().image);
	IsoFileSystem fs(std::move(dev));
	CheckIsoFs(fs);
}

}  // namespace

TEST(fs_iso_plain) {
	std::string err;
	CheckImage(OpenBlockDevice(MakeMemorySource(Iso().image), &err), ImageFormat::Iso);
	fstest::TempDir tmp("iso");
	auto path = tmp.Path() / "game.iso";
	CHECK(WriteWholeFile(path, Iso().image.data(), Iso().image.size()));
	CheckImage(OpenBlockDevice(path, &err), ImageFormat::Iso);
}

TEST(fs_iso_cso_v1) {
	CheckImage(OpenBlockDevice(MakeMemorySource(fstest::MakeCso(Iso().image, fstest::CsoFlavor::V1, 2048, 0))), ImageFormat::CsoV1);
	// Bigger frames and an index shift, read from a file.
	fstest::TempDir tmp("cso");
	auto path = tmp.Path() / "game.cso";
	std::vector<u8> cso = fstest::MakeCso(Iso().image, fstest::CsoFlavor::V1, 8192, 2);
	CHECK(cso.size() < Iso().image.size());
	CHECK(WriteWholeFile(path, cso.data(), cso.size()));
	CheckImage(OpenBlockDevice(path), ImageFormat::CsoV1);
}

TEST(fs_iso_cso_v2_zso_dax) {
	CheckImage(OpenBlockDevice(MakeMemorySource(fstest::MakeCso(Iso().image, fstest::CsoFlavor::V2, 4096, 1))), ImageFormat::CsoV2);
	CheckImage(OpenBlockDevice(MakeMemorySource(fstest::MakeCso(Iso().image, fstest::CsoFlavor::V2, 2048, 0))), ImageFormat::CsoV2);
	CheckImage(OpenBlockDevice(MakeMemorySource(fstest::MakeCso(Iso().image, fstest::CsoFlavor::Zso, 2048, 0))), ImageFormat::Zso);
	CheckImage(OpenBlockDevice(MakeMemorySource(fstest::MakeCso(Iso().image, fstest::CsoFlavor::Zso, 16384, 3))), ImageFormat::Zso);
	CheckImage(OpenBlockDevice(MakeMemorySource(fstest::MakeDax(Iso().image))), ImageFormat::Dax);
}

TEST(fs_iso_partial_last_sector) {
	// An image that stops in the middle of its last sector, with a file running to its last byte.
	std::vector<fstest::IsoInput> in = {{"A.BIN", fstest::Binary(1, 3000)}, {"Z.BIN", fstest::Text(2, 5000)}};
	TestIso iso = fstest::BuildIso(in, true);
	CHECK_EQ(iso.image.size() % 2048, (size_t)(5000 % 2048));
	for (int kind = 0; kind < 3; kind++) {
		std::vector<u8> img = kind == 0 ? iso.image : kind == 1 ? fstest::MakeCso(iso.image, fstest::CsoFlavor::V1, 8192, 0) : fstest::MakeCso(iso.image, fstest::CsoFlavor::Zso, 2048, 0);
		auto dev = OpenBlockDevice(MakeMemorySource(img));
		CHECK(dev != nullptr);
		if (!dev) continue;
		CheckDeviceMatches(*dev, iso.image);
		IsoFileSystem fs(std::move(dev));
		std::vector<u8> data;
		CHECK_EQ(fs.ReadWholeFile("/Z.BIN", data), 0);
		CHECK(data == in[1].data);
	}
}

TEST(fs_block_device_errors) {
	std::string err;
	CHECK(OpenBlockDevice(std::filesystem::path("Z:/no/such/image.iso"), &err) == nullptr);
	CHECK(!err.empty());
	CHECK(OpenBlockDevice(MakeMemorySource({}), &err) == nullptr);
	std::vector<u8> jso(64, 0);
	memcpy(jso.data(), "JISO", 4);
	CHECK(OpenBlockDevice(MakeMemorySource(jso), &err) == nullptr);
	CHECK(err.find("JSO") != std::string::npos);
	// CSO headers that don't add up.
	std::vector<u8> cso = fstest::MakeCso(Iso().image, fstest::CsoFlavor::V1, 2048, 0);
	std::vector<u8> bad = cso;
	fstest::Put32LE(&bad[16], 3000);  // frame size not a power of two
	CHECK(OpenBlockDevice(MakeMemorySource(bad), &err) == nullptr);
	bad = cso;
	bad[20] = 9;  // version
	CHECK(OpenBlockDevice(MakeMemorySource(bad), &err) == nullptr);
	bad = cso;
	bad.resize(bad.size() / 2);  // truncated data
	CHECK(OpenBlockDevice(MakeMemorySource(bad), &err) == nullptr);
	bad = cso;
	fstest::Put32LE(&bad[24 + 4 * 5], 1);  // index going backwards
	CHECK(OpenBlockDevice(MakeMemorySource(bad), &err) == nullptr);
	// A plain ISO without a volume descriptor still gives raw access.
	std::vector<u8> blank(40 * 2048, 0x5A);
	IsoFileSystem fs(OpenBlockDevice(MakeMemorySource(blank)));
	CHECK(!fs.IsValid());
	CHECK_EQ(fs.Open("/PSP_GAME/PARAM.SFO", PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);
	s32 fd = fs.Open("", PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	u8 sec[2048];
	CHECK_EQ(fs.Read(fd, sec, 1), (s64)1);
	CHECK_EQ(sec[100], 0x5A);
}

TEST(fs_cso_corrupt_frame) {
	std::vector<u8> cso = fstest::MakeCso(Iso().image, fstest::CsoFlavor::V1, 2048, 0);
	// Scramble a compressed frame inside EBOOT.BIN (frames are sectors here).
	u32 frame = Iso().lba.at("PSP_GAME/SYSDIR/EBOOT.BIN") + 2;
	while (cso[27 + frame * 4] & 0x80) frame++;
	u32 pos = GetU32(&cso[24 + frame * 4]);
	for (int i = 0; i < 40; i++) cso[pos + i] ^= 0xA5;
	auto dev = OpenBlockDevice(MakeMemorySource(cso));
	CHECK(dev != nullptr);
	if (!dev) return;
	u8 sec[2048];
	memset(sec, 0x77, sizeof(sec));
	CHECK(!dev->ReadBlock(frame, sec));
	CHECK_EQ(sec[0], 0);
	CHECK(dev->ReadBlock(frame + 1, sec));
	IsoFileSystem fs(std::move(dev));
	std::vector<u8> data;
	CHECK_EQ(fs.ReadWholeFile("/PSP_GAME/SYSDIR/EBOOT.BIN", data), SCE_ERRNO_EIO);
	CHECK_EQ(fs.ReadWholeFile("/PSP_GAME/ICON0.PNG", data), 0);
}

TEST(fs_cso_threaded_reads) {
	std::shared_ptr<BlockDevice> dev = OpenBlockDevice(MakeMemorySource(fstest::MakeCso(Iso().image, fstest::CsoFlavor::V1, 8192, 0)));
	CHECK(dev != nullptr);
	if (!dev) return;
	const std::vector<u8> &image = Iso().image;
	const u32 blocks = dev->NumBlocks();
	std::atomic<int> bad{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 6; t++) {
		threads.emplace_back([&, t] {
			fstest::Lcg r(1000 + t);
			std::vector<u8> buf(4 * 2048);
			for (int i = 0; i < 400; i++) {
				u32 lba = r.Next() % (blocks - 3);
				u32 n = 1 + r.Next() % 4;
				if (!dev->ReadBlocks(lba, n, buf.data()) || memcmp(buf.data(), image.data() + (size_t)lba * 2048, (size_t)n * 2048)) {
					bad++;
				}
			}
		});
	}
	for (auto &th : threads) th.join();
	CHECK_EQ(bad.load(), 0);
}

// Optional cross-check against independently made images: set PSPC_FS_IMAGE_DIR to a directory
// holding NAME.iso plus any of NAME.cso / NAME.zso / NAME.dax (e.g. from tools/mkcso.py or maxcso).
TEST(fs_external_images) {
	const char *dir = getenv("PSPC_FS_IMAGE_DIR");
	if (!dir) {
		return;
	}
	int compared = 0;
	for (auto &ent : std::filesystem::directory_iterator(dir)) {
		if (ent.path().extension() != ".iso") continue;
		std::vector<u8> image;
		CHECK(ReadWholeFile(ent.path(), image));
		for (const char *ext : {".cso", ".zso", ".dax", ".cso2"}) {
			auto other = ent.path();
			other.replace_extension(ext);
			if (!std::filesystem::exists(other)) continue;
			std::string err;
			auto dev = OpenBlockDevice(other, &err);
			CHECK(dev != nullptr);
			if (!dev) {
				printf("  %s: %s\n", other.string().c_str(), err.c_str());
				continue;
			}
			CheckDeviceMatches(*dev, image);
			compared++;
		}
	}
	printf("  compared %d external images\n", compared);
}
