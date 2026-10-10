// SPDX-License-Identifier: GPL-2.0-or-later
// M33 plugin support: the plugins folder laid over the memory stick (HostFileSystem fallback
// roots) and the seplugins list format (common/seplugins.h).
#include <cstring>

#include "common/seplugins.h"
#include "fs/host_fs.h"
#include "test_fs_helpers.h"
#include "testing.h"

using namespace vfs;
namespace sfs = std::filesystem;

namespace {

std::string ReadHost(const sfs::path &p) {
	std::vector<u8> d;
	if (!ReadWholeFile(p, d)) return "<missing>";
	return std::string(d.begin(), d.end());
}

std::vector<std::string> ListNames(FileSystem &fs, const char *path) {
	std::vector<std::string> out;
	s32 dd = fs.DOpen(path);
	if (dd <= 0) return out;
	SceIoDirent e;
	while (fs.DRead(dd, &e) == 1) out.push_back(e.d_name);
	fs.DClose(dd);
	return out;
}

bool Has(const std::vector<std::string> &v, const char *name) {
	for (const std::string &s : v)
		if (s == name) return true;
	return false;
}

}  // namespace

TEST(plugins_fallback_root) {
	fstest::TempDir tmp("plugins");
	const sfs::path ms = tmp.Path() / "ms0", plugins = tmp.Path() / "plugins";
	sfs::create_directories(ms / "PSP" / "SAVEDATA");
	sfs::create_directories(ms / "seplugins");
	sfs::create_directories(plugins / "seplugins");
	sfs::create_directories(plugins / "darkfrost" / "codes");
	CHECK(WriteTextFile(plugins / "seplugins" / "game.txt", "ms0:/darkfrost/df.prx\r\n"));
	CHECK(WriteTextFile(plugins / "darkfrost" / "df.prx", "PRX"));
	CHECK(WriteTextFile(ms / "seplugins" / "vsh.txt", "ms0:/seplugins/x.prx\r\n"));
	CHECK(WriteTextFile(ms / "PSP" / "readme.txt", "volume"));
	sfs::create_directories(plugins / "PSP");
	CHECK(WriteTextFile(plugins / "PSP" / "readme.txt", "overlay"));  // hidden by the volume's own

	HostFileSystem fs(ms);
	fs.AddFallbackRoot(plugins);
	fs.AddFallbackRoot(plugins);  // twice: ignored
	fs.AddFallbackRoot(tmp.Path() / "missing");

	// Lookups: the volume first, then the overlay (case-insensitively, like FAT).
	char buf[64] = {};
	s32 fd = fs.Open("/SEPLUGINS/GAME.TXT", PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	CHECK_EQ(fs.Read(fd, buf, sizeof(buf)), (s64)23);
	CHECK(!strncmp(buf, "ms0:/darkfrost/df.prx", 21));
	fs.Close(fd);
	SceIoStat st{};
	CHECK_EQ(fs.GetStat("/darkfrost/df.prx", &st), 0);
	CHECK_EQ(st.st_size, (s64)3);
	CHECK_EQ(fs.GetStat("/seplugins/vsh.txt", &st), 0);
	CHECK_EQ(fs.GetStat("/nothing.txt", &st), SCE_ERRNO_ENOENT);
	fd = fs.Open("/PSP/readme.txt", PSP_O_RDONLY, 0);
	memset(buf, 0, sizeof(buf));
	CHECK_EQ(fs.Read(fd, buf, sizeof(buf)), (s64)6);  // "volume"
	fs.Close(fd);

	// Listings merge; a name in both roots shows once.
	std::vector<std::string> root = ListNames(fs, "/");
	CHECK(Has(root, "PSP"));
	CHECK(Has(root, "darkfrost"));
	CHECK(Has(root, "seplugins"));
	int seps = 0;
	for (const std::string &n : root) seps += n == "seplugins";
	CHECK_EQ(seps, 1);
	std::vector<std::string> sep = ListNames(fs, "/seplugins");
	CHECK(Has(sep, "game.txt"));
	CHECK(Has(sep, "vsh.txt"));

	// New files go where their folder is: the plugin's own folders are in the overlay.
	fd = fs.Open("/darkfrost/codes/NPUG80114.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	CHECK(fd > 0);
	CHECK_EQ(fs.Write(fd, "#Cheat", 6), (s64)6);
	fs.Close(fd);
	CHECK(ReadHost(plugins / "darkfrost" / "codes" / "NPUG80114.txt") == "#Cheat");
	CHECK_EQ(fs.Mkdir("/darkfrost/dumps", 0777), 0);
	CHECK(sfs::is_directory(plugins / "darkfrost" / "dumps"));
	CHECK_EQ(fs.Mkdir("/darkfrost", 0777), SCE_ERRNO_EEXIST);
	// A folder in both: the volume wins; a new top-level name goes to the volume.
	fd = fs.Open("/seplugins/conf.bin", PSP_O_WRONLY | PSP_O_CREAT, 0777);
	CHECK(fd > 0);
	fs.Close(fd);
	CHECK(sfs::exists(ms / "seplugins" / "conf.bin"));
	CHECK_EQ(fs.Mkdir("/ISO", 0777), 0);
	CHECK(sfs::is_directory(ms / "ISO"));
	// Renames and removals act on the file where it is.
	CHECK_EQ(fs.Rename("/darkfrost/codes/NPUG80114.txt", "/darkfrost/codes/temp.txt"), 0);
	CHECK(sfs::exists(plugins / "darkfrost" / "codes" / "temp.txt"));
	CHECK_EQ(fs.Remove("/darkfrost/codes/temp.txt"), 0);
	CHECK(!sfs::exists(plugins / "darkfrost" / "codes" / "temp.txt"));
}

TEST(plugins_seplugins_lists) {
	using namespace seplugins;
	// Flags on the line win; else conf.bin's byte (game list: slots 5-9); else on.
	std::vector<u8> conf(15, 1);
	conf[6] = 0;
	std::vector<Entry> e = Parse("ms0:/a.prx\r\n\r\nms0:/b.prx\r\n  ms0:/c.prx 0\nms0:/d.prx\t1\n", List::Game, conf);
	CHECK_EQ(e.size(), (size_t)4);
	CHECK(e[0].path == "ms0:/a.prx" && e[0].enabled && e[0].line == 0);
	CHECK(e[1].path == "ms0:/b.prx" && !e[1].enabled && e[1].line == 2);  // conf.bin slot 6
	CHECK(e[2].path == "ms0:/c.prx" && !e[2].enabled);
	CHECK(e[3].path == "ms0:/d.prx" && e[3].enabled);
	CHECK(Parse("ms0:/x.prx\n", List::Vsh, {})[0].enabled);
	CHECK(!Parse("ms0:/x.prx\n", List::Vsh, {0})[0].enabled);
	CHECK_EQ(ConfBase(List::Pops), 10);

	fstest::TempDir tmp("seplugins");
	const sfs::path root = tmp.Path();
	sfs::create_directories(root / "seplugins");
	sfs::create_directories(root / "Cheats");
	CHECK(WriteTextFile(root / "seplugins" / "game.txt", "ms0:/cheats/DF.prx\r\nms0:/seplugins/one.prx\r\nms0:/seplugins/two.prx 0\r\n"));
	CHECK(WriteTextFile(root / "Cheats" / "df.prx", "x"));
	std::vector<Entry> game = ReadList(root, List::Game);
	CHECK_EQ(game.size(), (size_t)3);
	CHECK(sfs::equivalent(HostPath(root, game[0].path), root / "Cheats" / "df.prx"));
	CHECK(HostPath(root, game[2].path).empty());
	CHECK(HostPath(root, "host0:/x.prx").empty());
	game[0].enabled = false;
	game[2].enabled = true;
	Entry added;
	added.path = "ms0:/seplugins/new.prx";
	added.line = -1;
	game.push_back(added);
	CHECK(WriteList(root, List::Game, game));
	CHECK(ReadHost(root / "seplugins" / "game.txt") ==
	      "ms0:/cheats/DF.prx 0\r\nms0:/seplugins/one.prx 1\r\nms0:/seplugins/two.prx 1\r\nms0:/seplugins/new.prx 1\r\n");
	std::vector<Entry> back = ReadList(root, List::Game);
	CHECK(!back[0].enabled && back[2].enabled && back[3].enabled);
}
