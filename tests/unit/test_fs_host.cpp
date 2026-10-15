// SPDX-License-Identifier: GPL-2.0-or-later
// PSP path helpers and HostFileSystem. Expected codes and modes follow pspautotests io/file,
// io/file/rename, io/directory and io/stat(/readonly).
#include <algorithm>
#include <cstring>

#include "fs/fat_names.h"
#include "fs/host_fs.h"
#include "fs/path_util.h"
#include "test_fs_helpers.h"
#include "testing.h"

using namespace vfs;
namespace sfs = std::filesystem;

namespace {

std::vector<SceIoDirent> List(FileSystem &fs, const char *path) {
	std::vector<SceIoDirent> out;
	s32 dd = fs.DOpen(path);
	CHECK(dd > 0);
	if (dd <= 0) return out;
	SceIoDirent e;
	memset(&e, 0xCC, sizeof(e));
	while (fs.DRead(dd, &e) == 1) {
		out.push_back(e);
	}
	CHECK_EQ(e.d_name[0], '\0');
	CHECK_EQ(fs.DClose(dd), 0);
	return out;
}

std::vector<std::string> Names(const std::vector<SceIoDirent> &ents) {
	std::vector<std::string> n;
	for (auto &e : ents) n.push_back(e.d_name);
	return n;
}

const SceIoDirent *Find(const std::vector<SceIoDirent> &ents, const char *name) {
	for (auto &e : ents) {
		if (!strcmp(e.d_name, name)) return &e;
	}
	return nullptr;
}

void WriteHost(const sfs::path &p, const std::string &s) {
	CHECK(WriteTextFile(p, s));
}

std::string ReadHost(const sfs::path &p) {
	std::vector<u8> d;
	if (!ReadWholeFile(p, d)) return "<missing>";
	return std::string(d.begin(), d.end());
}

}  // namespace

TEST(fs_path_normalize) {
	CHECK(NormalizePspPath("ms0:PSP//GAME/./x/../") == "ms0:/PSP/GAME");
	CHECK(NormalizePspPath("a\\b") == "/a/b");
	CHECK(NormalizePspPath("/../..") == "/");
	CHECK(NormalizePspPath("") == "");
	CHECK(NormalizePspPath("umd0:") == "umd0:");
	CHECK(NormalizePspPath("umd0:/") == "umd0:/");
	CHECK(NormalizePspPath("disc0:/sce_lbn0x10_size0x800") == "disc0:/sce_lbn0x10_size0x800");
	CHECK(NormalizePspPath("ms0:/PSP/GAME", "EBOOT.PBP") == "ms0:/PSP/GAME/EBOOT.PBP");
	CHECK(NormalizePspPath("ms0:/PSP/GAME", "../../../x") == "ms0:/x");
	CHECK(NormalizePspPath("ms0:/PSP/GAME", "/SAVEDATA") == "ms0:/PSP/GAME/SAVEDATA");  // no device: relative
	CHECK(NormalizePspPath("ms0:/PSP/GAME", "host0:/a/./b") == "host0:/a/b");
	CHECK(NormalizePspPath("ms0:/PSP/GAME", "") == "ms0:/PSP/GAME");
	CHECK(NormalizePspPath("ms0:/PSP", ".\\GAME\\") == "ms0:/PSP/GAME");
	CHECK(NormalizePspPath("", "x/y") == "/x/y");
	std::string_view dev, rest;
	CHECK(SplitPspDevice("flash0:/kd/a.prx", &dev, &rest));
	CHECK(dev == "flash0:");
	CHECK(rest == "/kd/a.prx");
	CHECK(!SplitPspDevice("/a:b", &dev, &rest));
	CHECK(rest == "/a:b");
	CHECK(!SplitPspDevice(":x", &dev, &rest));
	CHECK(SplitPspPathComponents("/a/../../b//c/.") == std::vector<std::string>({"b", "c"}));
	CHECK(EqualsNoCase("Eboot.Pbp", "EBOOT.PBP"));
	CHECK(!EqualsNoCase("a", "ab"));
}

TEST(fs_host_files) {
	fstest::TempDir tmp("host");
	const sfs::path root = tmp.Path() / "ms0";
	HostFileSystem fs(root);
	CHECK(sfs::is_directory(root));
	CHECK(fs.Caps() & FS_CAP_FAT);
	CHECK(!(fs.Caps() & FS_CAP_READ_ONLY));

	CHECK_EQ(fs.Mkdir("/PSP", 0777), 0);
	CHECK_EQ(fs.Mkdir("/PSP/SAVEDATA", 0777), 0);
	CHECK_EQ(fs.Mkdir("/psp/savedata", 0777), SCE_ERRNO_EEXIST);
	CHECK_EQ(fs.Mkdir("/NOPE/X", 0777), SCE_ERRNO_ENOENT);

	// io/file, step by step.
	const char *text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	char buf[128];
	s32 fd = fs.Open("/PSP/SAVEDATA/testfile", PSP_O_WRONLY | PSP_O_CREAT, 0777);
	CHECK(fd > 0);
	CHECK_EQ(fs.Write(fd, text, 36), (s64)36);
	CHECK_EQ(fs.Read(fd, buf, 128), SCE_KERNEL_ERROR_BADF);
	CHECK_EQ(fs.Close(fd), 0);
	CHECK(ReadHost(root / "PSP" / "SAVEDATA" / "testfile") == text);
	fd = fs.Open("/psp/SaveData/TESTFILE", PSP_O_RDONLY, 0777);
	CHECK(fd > 0);
	memset(buf, 0, sizeof(buf));
	CHECK_EQ(fs.Read(fd, buf, 128), (s64)36);
	CHECK(!memcmp(buf, text, 36));
	CHECK_EQ(fs.Seek(fd, -10, PSP_SEEK_END), (s64)26);
	CHECK_EQ(fs.Read(fd, buf, 128), (s64)10);
	CHECK(!memcmp(buf, "0123456789", 10));
	CHECK_EQ(fs.Seek(fd, -10, PSP_SEEK_CUR), (s64)26);
	CHECK_EQ(fs.Read(fd, buf, 5), (s64)5);
	CHECK(!memcmp(buf, "01234", 5));
	CHECK_EQ(fs.Seek(fd, -10, PSP_SEEK_CUR), (s64)21);
	CHECK_EQ(fs.Read(fd, buf, 128), (s64)15);
	CHECK(!memcmp(buf, "VWXYZ0123456789", 15));
	CHECK_EQ(fs.Seek(fd, -10, PSP_SEEK_SET), (s64)-1);
	CHECK_EQ(fs.Seek(fd, 200, PSP_SEEK_SET), (s64)200);
	CHECK_EQ(fs.Read(fd, buf, 128), (s64)0);
	CHECK_EQ(fs.Seek(fd, -10, PSP_SEEK_CUR), (s64)190);
	CHECK_EQ(fs.Seek(fd, 0, PSP_SEEK_END), (s64)36);
	CHECK_EQ(fs.Seek(fd, 0, 3), SCE_KERNEL_ERROR_INVAL);
	CHECK_EQ(fs.Write(fd, text, 36), SCE_KERNEL_ERROR_BADF);
	HandleInfo hi;
	CHECK_EQ(fs.GetHandleInfo(fd, &hi), 0);
	CHECK_EQ(hi.size, (u64)36);
	CHECK_EQ(hi.position, (u64)36);
	CHECK(!hi.sectorMode);
	CHECK_EQ(fs.Ioctl(fd, 0x01020001, nullptr, 0, buf, sizeof(buf)), SCE_ERRNO_FUNCTION_NOT_SUPPORTED);
	CHECK_EQ(fs.Close(fd), 0);
	// Append writes at the end whatever the position; reads still follow it.
	fd = fs.Open("/PSP/SAVEDATA/testfile", PSP_O_RDWR | PSP_O_APPEND, 0777);
	CHECK_EQ(fs.Write(fd, text, 36), (s64)36);
	CHECK_EQ(fs.Seek(fd, 0, PSP_SEEK_SET), (s64)0);
	CHECK_EQ(fs.Write(fd, "xy", 2), (s64)2);
	CHECK_EQ(fs.Seek(fd, 0, PSP_SEEK_SET), (s64)0);
	CHECK_EQ(fs.Read(fd, buf, 128), (s64)74);
	CHECK(!memcmp(buf + 36, text, 36));
	CHECK(!memcmp(buf + 72, "xy", 2));
	fs.Close(fd);
	// Truncate and exclusive create.
	fd = fs.Open("/PSP/SAVEDATA/testfile", PSP_O_WRONLY | PSP_O_TRUNC, 0777);
	CHECK_EQ(fs.Write(fd, "hello", 5), (s64)5);
	fs.Close(fd);
	CHECK(ReadHost(root / "PSP" / "SAVEDATA" / "testfile") == "hello");
	CHECK_EQ(fs.Open("/PSP/SAVEDATA/testfile", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777), SCE_ERRNO_EEXIST);
	fd = fs.Open("/PSP/SAVEDATA/new.bin", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
	CHECK(fd > 0);
	fs.Close(fd);
	// Open errors.
	CHECK_EQ(fs.Open("/PSP/SAVEDATA/nofile", PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.Open("/PSP/SAVEDATA", PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);  // a directory
	CHECK_EQ(fs.Open("/", PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.Open("/NODIR/file", PSP_O_WRONLY | PSP_O_CREAT, 0), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.Open("/PSP/SAVEDATA/testfile/x", PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.Open("/PSP/SAVEDATA/testfile", 0, 0), SCE_ERRNO_EINVAL);

	// Stat: FAT modes, private words untouched (io/stat).
	SceIoStat st;
	memset(&st, 0xCC, sizeof(st));
	CHECK_EQ(fs.GetStat("/PSP/SAVEDATA/testfile", &st), 0);
	CHECK_EQ(st.st_mode, (s32)0x21FF);
	CHECK_EQ(st.st_attr, 0x20u);
	CHECK_EQ(st.st_size, (s64)5);
	for (u32 p : st.st_private) CHECK_EQ(p, 0xCCCCCCCCu);
	CHECK(st.st_mtime.year >= 2020 && st.st_mtime.month >= 1 && st.st_mtime.month <= 12);
	CHECK(st.st_ctime.year >= 2020);
	CHECK(st.st_mtime.microsecond < 1000000);
	CHECK_EQ(fs.GetStat("/PSP/SAVEDATA", &st), 0);
	CHECK_EQ(st.st_mode, (s32)0x11FF);
	CHECK_EQ(st.st_attr, 0x10u);
	CHECK_EQ(st.st_size, (s64)0);
	CHECK_EQ(fs.GetStat("/PSP/nofile", &st), SCE_ERRNO_ENOENT);

	// Chstat: read-only through the mode or the attribute (io/stat/readonly).
	SceIoStat chg{};
	chg.st_mode = 0444;
	CHECK_EQ(fs.ChStat("/PSP/SAVEDATA/testfile", chg, SCE_CST_MODE), 0);
	CHECK_EQ(fs.GetStat("/PSP/SAVEDATA/testfile", &st), 0);
	CHECK_EQ(st.st_mode, (s32)0x216D);
	CHECK_EQ(st.st_attr, 0x21u);
	CHECK_EQ(fs.Open("/PSP/SAVEDATA/testfile", PSP_O_WRONLY, 0), SCE_ERRNO_EACCES);
	CHECK_EQ(fs.Remove("/PSP/SAVEDATA/testfile"), SCE_ERRNO_EACCES);
	fd = fs.Open("/PSP/SAVEDATA/testfile", PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	fs.Close(fd);
	chg.st_mode = 0777;
	CHECK_EQ(fs.ChStat("/PSP/SAVEDATA/testfile", chg, SCE_CST_MODE), 0);
	CHECK_EQ(fs.GetStat("/PSP/SAVEDATA/testfile", &st), 0);
	CHECK_EQ(st.st_mode, (s32)0x21FF);
	CHECK_EQ(st.st_attr, 0x20u);
	chg.st_attr = 0x21;
	CHECK_EQ(fs.ChStat("/PSP/SAVEDATA/testfile", chg, SCE_CST_ATTR), 0);
	CHECK_EQ(fs.GetStat("/PSP/SAVEDATA/testfile", &st), 0);
	CHECK_EQ(st.st_mode, (s32)0x216D);
	chg.st_attr = 0x20;
	CHECK_EQ(fs.ChStat("/PSP/SAVEDATA/testfile", chg, SCE_CST_MODE | SCE_CST_ATTR), 0);
	CHECK_EQ(fs.GetStat("/PSP/SAVEDATA/testfile", &st), 0);
	CHECK_EQ(st.st_attr, 0x20u);
	// Times, in local time to the microsecond.
	chg.st_mtime = {2010, 3, 4, 5, 6, 7, 123456};
	chg.st_ctime = {2009, 1, 2, 3, 4, 5, 0};
	CHECK_EQ(fs.ChStat("/PSP/SAVEDATA/testfile", chg, SCE_CST_MT | SCE_CST_CT), 0);
	CHECK_EQ(fs.GetStat("/PSP/SAVEDATA/testfile", &st), 0);
	CHECK_EQ(st.st_mtime.year, 2010);
	CHECK_EQ(st.st_mtime.month, 3);
	CHECK_EQ(st.st_mtime.day, 4);
	CHECK_EQ(st.st_mtime.hour, 5);
	CHECK_EQ(st.st_mtime.minute, 6);
	CHECK_EQ(st.st_mtime.second, 7);
	CHECK_EQ(st.st_mtime.microsecond, 123456u);
	CHECK_EQ(st.st_ctime.year, 2009);
	CHECK_EQ(st.st_ctime.second, 5);
	CHECK_EQ(fs.ChStat("/PSP/nofile", chg, SCE_CST_MODE), SCE_ERRNO_ENOENT);

	// Rename (io/file/rename): the file keeps its directory, targets that exist are EEXIST.
	CHECK_EQ(fs.Rename("/PSP/SAVEDATA/testfile", "testfile2"), 0);
	CHECK(ReadHost(root / "PSP" / "SAVEDATA" / "testfile2") == "hello");
	CHECK_EQ(fs.Rename("/PSP/SAVEDATA/testfile2", "/ELSEWHERE/testfile3"), 0);
	CHECK(sfs::exists(root / "PSP" / "SAVEDATA" / "testfile3"));
	CHECK_EQ(fs.Rename("/PSP/SAVEDATA/nofile", "nofile2"), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.Rename("/PSP/SAVEDATA/testfile3", "/PSP/SAVEDATA/testfile3"), SCE_ERRNO_EEXIST);
	CHECK_EQ(fs.Rename("/PSP/SAVEDATA/testfile3", "TESTFILE3"), SCE_ERRNO_EEXIST);
	CHECK_EQ(fs.Rename("/PSP/SAVEDATA/testfile3", "new.bin"), SCE_ERRNO_EEXIST);
	CHECK_EQ(fs.Rename("/PSP/SAVEDATA/test*", "x"), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Rename("/PSP/SAVEDATA/testfile3", "x?"), SCE_ERRNO_EINVAL);

	// Remove and rmdir.
	CHECK_EQ(fs.Rmdir("/PSP/SAVEDATA"), SCE_ERRNO_ENOTEMPTY);
	CHECK_EQ(fs.Remove("/PSP/SAVEDATA"), SCE_ERRNO_EISDIR);
	CHECK_EQ(fs.Rmdir("/PSP/SAVEDATA/new.bin"), SCE_ERRNO_ENOTDIR);
	CHECK_EQ(fs.Remove("/PSP/SAVEDATA/testfile3"), 0);
	CHECK_EQ(fs.Remove("/PSP/SAVEDATA/testfile3"), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.Remove("/PSP/SAVEDATA/new.bin"), 0);
	CHECK_EQ(fs.Rmdir("/PSP/SAVEDATA"), 0);
	CHECK_EQ(fs.Rmdir("/PSP/SAVEDATA"), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.Rmdir("/"), SCE_ERRNO_EBUSY);

	CHECK_EQ(fs.Close(9999), SCE_KERNEL_ERROR_BADF);
	CHECK_EQ(fs.Read(9999, buf, 1), SCE_KERNEL_ERROR_BADF);
	CHECK_EQ(fs.Seek(9999, 0, PSP_SEEK_SET), SCE_KERNEL_ERROR_BADF);
	u64 freeBytes = 0, totalBytes = 0;
	CHECK(fs.QueryFreeSpace(&freeBytes, &totalBytes));
	CHECK(totalBytes > 0 && freeBytes <= totalBytes);
}

TEST(fs_host_listing) {
	fstest::TempDir tmp("list");
	const sfs::path root = tmp.Path() / "ms0";
	sfs::create_directories(root / "folder" / "subfolder");
	WriteHost(root / "folder" / "1.txt", "hello");
	WriteHost(root / "folder" / "2.txt", "hello!");
	WriteHost(root / "folder" / "TEST.TXT", "fifteen chars!!");
	WriteHost(root / "folder" / "largefile.bin", std::string(211894, 'x'));
	WriteHost(root / "folder" / "subfolder" / "subtext.txt", "s");
	WriteHost(root / "top.dat", "t");

	HostFsOptions opt;
	opt.upperCase83 = true;
	HostFileSystem fs(root, opt);
	std::vector<SceIoDirent> ents = List(fs, "/folder");
	std::vector<std::string> names = Names(ents);
	CHECK(names.size() == 7 && names[0] == "." && names[1] == "..");
	std::sort(names.begin(), names.end());
	CHECK(names == std::vector<std::string>({".", "..", "1.TXT", "2.TXT", "TEST.TXT", "largefile.bin", "subfolder"}));
	if (const SceIoDirent *e = Find(ents, "..")) {
		CHECK_EQ(e->d_stat.st_size, (s64)4096);
		CHECK_EQ(e->d_stat.st_attr, 16u);
	}
	if (const SceIoDirent *e = Find(ents, "1.TXT")) {
		CHECK_EQ(e->d_stat.st_size, (s64)5);
		CHECK_EQ(e->d_stat.st_attr, 32u);
		CHECK_EQ(e->d_stat.st_mode, (s32)0x21FF);
		CHECK_EQ(e->d_stat.st_private[0], 0xCCCCCCCCu);
		CHECK_EQ(e->d_private, 0xCCCCCCCCu);
	}
	CHECK(Find(ents, "largefile.bin") && Find(ents, "largefile.bin")->d_stat.st_size == 211894);
	if (const SceIoDirent *e = Find(ents, "subfolder")) {
		CHECK_EQ(e->d_stat.st_size, (s64)0);
		CHECK_EQ(e->d_stat.st_attr, 16u);
		CHECK_EQ(e->d_stat.st_mode, (s32)0x11FF);
	}
	// The root lists "." and ".." too.
	std::vector<std::string> top = Names(List(fs, "/"));
	std::sort(top.begin(), top.end());
	CHECK(top == std::vector<std::string>({".", "..", "FOLDER", "TOP.DAT"}));
	CHECK(Names(List(fs, "")).size() == 4);
	CHECK(Names(List(fs, "/FOLDER/SUBFOLDER")) == std::vector<std::string>({".", "..", "SUBTEXT.TXT"}));
	CHECK_EQ(fs.DOpen("/top.dat"), SCE_ERRNO_ENOTDIR);
	CHECK_EQ(fs.DOpen("/FakeDir"), SCE_ERRNO_ENOENT);
	SceIoDirent e;
	CHECK_EQ(fs.DRead(4321, &e), SCE_KERNEL_ERROR_BADF);
	CHECK_EQ(fs.DClose(4321), SCE_KERNEL_ERROR_BADF);
	// The listed names open.
	s32 fd = fs.Open("/folder/1.TXT", PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	fs.Close(fd);
	// Without the 8.3 quirk names come through as on the host.
	HostFileSystem plain(root);
	names = Names(List(plain, "/folder"));
	CHECK(std::find(names.begin(), names.end(), "1.txt") != names.end());
}

TEST(fs_host_confinement) {
	fstest::TempDir tmp("jail");
	const sfs::path root = tmp.Path() / "ms0";
	WriteHost(tmp.Path() / "secret.txt", "secret");
	HostFileSystem fs(root);
	// ".." stops at the root.
	CHECK_EQ(fs.Open("../secret.txt", PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.Open("/../../secret.txt", PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.Open("a/../../secret.txt", PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);
	CHECK_EQ(fs.Open("..\\secret.txt", PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);
	SceIoStat st;
	CHECK_EQ(fs.GetStat("/../secret.txt", &st), SCE_ERRNO_ENOENT);
	s32 fd = fs.Open("../../escaped.txt", PSP_O_WRONLY | PSP_O_CREAT, 0777);
	CHECK(fd > 0);
	fs.Write(fd, "e", 1);
	fs.Close(fd);
	CHECK(sfs::exists(root / "escaped.txt"));
	CHECK_EQ(fs.Mkdir("../../outside", 0777), 0);
	CHECK(sfs::is_directory(root / "outside"));
	CHECK_EQ(fs.Rename("/escaped.txt", "../../moved.txt"), 0);
	CHECK(sfs::exists(root / "moved.txt"));
	// Host-only syntax is not a path: drives, streams, wildcards, reserved characters.
	CHECK_EQ(fs.Open("C:/Windows/win.ini", PSP_O_RDONLY, 0), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Open("/moved.txt:stream", PSP_O_WRONLY | PSP_O_CREAT, 0), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Open("/a*b", PSP_O_WRONLY | PSP_O_CREAT, 0), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Open("/a?b", PSP_O_WRONLY | PSP_O_CREAT, 0), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Open("/a<b>", PSP_O_WRONLY | PSP_O_CREAT, 0), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Open("/a\"b|", PSP_O_WRONLY | PSP_O_CREAT, 0), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Open("/tab\there", PSP_O_WRONLY | PSP_O_CREAT, 0), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Open("/...", PSP_O_WRONLY | PSP_O_CREAT, 0), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Mkdir("/x:y", 0777), SCE_ERRNO_EINVAL);
	CHECK_EQ(fs.Open(std::string(300, 'n'), PSP_O_WRONLY | PSP_O_CREAT, 0), SCE_ERRNO_ENAMETOOLONG);
	// FAT drops trailing dots and spaces.
	fd = fs.Open("/name. .", PSP_O_WRONLY | PSP_O_CREAT, 0777);
	CHECK(fd > 0);
	fs.Close(fd);
	CHECK(sfs::exists(root / "name"));
	// Device names are plain files under "\\?\".
	fd = fs.Open("/CON", PSP_O_WRONLY | PSP_O_CREAT, 0777);
	CHECK(fd > 0);
	CHECK_EQ(fs.Write(fd, "c", 1), (s64)1);
	fs.Close(fd);
	CHECK_EQ(fs.Remove("/CON"), 0);
	sfs::path hp;
	CHECK_EQ(fs.HostPath("/../a.txt", &hp), 0);
	CHECK(sfs::equivalent(hp.parent_path(), root));
	CHECK_EQ(fs.HostPath("/nodir/a.txt", &hp), SCE_ERRNO_ENOENT);
	// Nothing appeared outside the root.
	std::vector<std::string> outside;
	for (auto &ent : sfs::directory_iterator(tmp.Path())) outside.push_back(ent.path().filename().string());
	std::sort(outside.begin(), outside.end());
	CHECK(outside == std::vector<std::string>({"ms0", "secret.txt"}));
	CHECK(ReadHost(tmp.Path() / "secret.txt") == "secret");
}

TEST(fs_host_readonly_and_host0) {
	fstest::TempDir tmp("ro");
	const sfs::path root = tmp.Path() / "flash0";
	sfs::create_directories(root / "kd");
	WriteHost(root / "kd" / "a.prx", "prx");
	HostFsOptions ro;
	ro.readOnly = true;
	HostFileSystem fs(root, ro);
	CHECK(fs.Caps() & FS_CAP_READ_ONLY);
	s32 fd = fs.Open("/KD/A.PRX", PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	char buf[8];
	CHECK_EQ(fs.Read(fd, buf, 8), (s64)3);
	fs.Close(fd);
	CHECK_EQ(fs.Open("/kd/a.prx", PSP_O_RDWR, 0), SCE_ERRNO_EROFS);
	CHECK_EQ(fs.Open("/kd/b.prx", PSP_O_RDONLY | PSP_O_CREAT, 0), SCE_ERRNO_EROFS);
	CHECK_EQ(fs.Remove("/kd/a.prx"), SCE_ERRNO_EROFS);
	CHECK_EQ(fs.Mkdir("/x", 0777), SCE_ERRNO_EROFS);
	CHECK_EQ(fs.Rmdir("/kd"), SCE_ERRNO_EROFS);
	CHECK_EQ(fs.Rename("/kd/a.prx", "b.prx"), SCE_ERRNO_EROFS);
	SceIoStat st{};
	CHECK_EQ(fs.ChStat("/kd/a.prx", st, SCE_CST_MODE), SCE_ERRNO_EROFS);

	// host0:-style: unix-like modes, rename takes the full target path.
	HostFsOptions h0;
	h0.fat = false;
	HostFileSystem host(root, h0);
	CHECK(!(host.Caps() & FS_CAP_FAT));
	CHECK_EQ(host.GetStat("/kd/a.prx", &st), 0);
	CHECK_EQ(st.st_mode, (s32)(FIO_S_IFREG | 0664));
	CHECK_EQ(st.st_attr, 0x20u);
	CHECK_EQ(host.GetStat("/kd", &st), 0);
	CHECK_EQ(st.st_mode, (s32)(FIO_S_IFDIR | 0777));
	CHECK_EQ(host.Mkdir("/other", 0777), 0);
	CHECK_EQ(host.Rename("/kd/a.prx", "/other/b.prx"), 0);
	CHECK(sfs::exists(root / "other" / "b.prx"));
	CHECK_EQ(host.Rename("/other/b.prx", "/nodir/b.prx"), SCE_ERRNO_ENOENT);
}

TEST(fs_host_rename_and_remove_open_file) {
	fstest::TempDir tmp("open_rename");
	HostFileSystem fs(tmp.Path());
	s32 fd = fs.Open("/open.bin", PSP_O_RDWR | PSP_O_CREAT, 0777);
	CHECK(fd > 0);
	CHECK_EQ(fs.Write(fd, "hello", 5), (s64)5);
	CHECK_EQ(fs.Rename("/open.bin", "renamed.bin"), 0);
	CHECK(!sfs::exists(tmp.Path() / "open.bin"));
	CHECK(sfs::exists(tmp.Path() / "renamed.bin"));
	CHECK_EQ(fs.Remove("/renamed.bin"), 0);
	CHECK(!sfs::exists(tmp.Path() / "renamed.bin"));
	CHECK_EQ(fs.Seek(fd, 0, PSP_SEEK_SET), (s64)0);
	char text[5]{};
	CHECK_EQ(fs.Read(fd, text, sizeof(text)), (s64)5);
	CHECK(!memcmp(text, "hello", sizeof(text)));
	CHECK_EQ(fs.Close(fd), 0);
}

TEST(fs_fat_short_names) {
	// Hardware values from pspautotests io/shortname, in creation order.
	std::vector<std::string> names = {".", "..", "readme.txt", "UPPER.TXT", "MiXeD.txt", "LongFileName.txt", "LongFileName2.txt",
		"LongFileNameThree.txt", "a.b.c.txt", "noextensionhere", "shrt", "sp ace.txt", "+plus[brack].txt", ".hidden",
		"toolongextension.mpeg", "LongDirectoryName"};
	std::vector<std::string> want = {".", "..", "README~1.TXT", "UPPER.TXT", "MIXED~1.TXT", "LONGFI~1.TXT", "LONGFI~2.TXT",
		"LONGFI~3.TXT", "ABC~1.TXT", "NOEXTE~1", "SHRT", "SPACE~1.TXT", "_PLUS_~1.TXT", "HIDDEN~1", "TOOLON~1.MPE", "LONGDI~1"};
	CHECK(GenerateFatShortNames(names) == want);

	// Files open by their short name on a FAT volume.
	fstest::TempDir tmp("short");
	const sfs::path root = tmp.Path() / "ms0";
	sfs::create_directories(root / "dir");
	WriteHost(root / "dir" / "LongFileName.txt", "1");
	WriteHost(root / "dir" / "LongFileName2.txt", "2");
	HostFileSystem fs(root);
	char c = 0;
	s32 fd = fs.Open("/dir/longfi~2.txt", PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	CHECK_EQ(fs.Read(fd, &c, 1), (s64)1);
	CHECK_EQ(c, '2');
	fs.Close(fd);
	CHECK_EQ(fs.Open("/dir/NOSUCH~1.TXT", PSP_O_RDONLY, 0), SCE_ERRNO_ENOENT);
}
