// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc-iso: inspect UMD images (ISO/CSO/ZSO/DAX).
//
//   pspc-iso info <image>                   disc id, title, executables
//   pspc-iso ls <image> [dir]               list a directory
//   pspc-iso get <image> <path> <outfile>   extract one file
//   pspc-iso modules <image> <outdir>       extract every executable (EBOOT.BIN, BOOT.BIN, *.PRX)
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "common/fileutil.h"
#include "fs/vfs.h"

namespace {

std::shared_ptr<vfs::IsoFileSystem> Open(const std::string &path) {
	std::string err;
	auto bd = vfs::OpenBlockDevice(fs::u8path(path), &err);
	if (!bd) {
		fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
		return nullptr;
	}
	auto iso = std::make_shared<vfs::IsoFileSystem>(std::shared_ptr<vfs::BlockDevice>(std::move(bd)));
	if (!iso->IsValid()) {
		fprintf(stderr, "%s: %s\n", path.c_str(), iso->Error().c_str());
		return nullptr;
	}
	return iso;
}

void Walk(vfs::IsoFileSystem &iso, const std::string &dir, const std::function<void(const std::string &, bool, u64)> &fn) {
	s32 d = iso.DOpen(dir);
	if (d < 0) return;
	vfs::SceIoDirent e{};
	std::vector<std::pair<std::string, bool>> subdirs;
	while (iso.DRead(d, &e) > 0) {
		std::string name = e.d_name;
		if (name == "." || name == "..") continue;
		bool isDir = (e.d_stat.st_mode & vfs::FIO_S_IFDIR) != 0;
		std::string full = dir == "/" ? "/" + name : dir + "/" + name;
		fn(full, isDir, (u64)e.d_stat.st_size);
		if (isDir) subdirs.push_back({full, true});
	}
	iso.DClose(d);
	for (auto &s : subdirs) Walk(iso, s.first, fn);
}

// An ELF or a ~PSP (encrypted) executable, from its first 4 bytes. Only those are read, so
// listing a big image doesn't read every file on it.
bool IsExecutable(vfs::IsoFileSystem &iso, const std::string &path) {
	s32 fd = iso.Open(path, vfs::PSP_O_RDONLY, 0);
	if (fd < 0) return false;
	u8 d[4] = {};
	s64 n = iso.Read(fd, d, sizeof(d));
	iso.Close(fd);
	return n == 4 && ((d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F') || (d[0] == '~' && d[1] == 'P' && d[2] == 'S' && d[3] == 'P'));
}

// Copies a file of the image to a host file 1 MB at a time (a file can be up to 4 GB).
// Returns 0, 2 if the file isn't on the image, or 3 if reading or writing failed.
int Extract(vfs::IsoFileSystem &iso, const std::string &path, const fs::path &out) {
	s32 fd = iso.Open(path, vfs::PSP_O_RDONLY, 0);
	if (fd < 0) return 2;
	std::ofstream f(out, std::ios::binary | std::ios::trunc);
	std::vector<u8> buf(1u << 20);
	bool ok = (bool)f;
	while (ok) {
		s64 n = iso.Read(fd, buf.data(), buf.size());
		if (n <= 0) {
			ok = n == 0;
			break;
		}
		ok = (bool)f.write((const char *)buf.data(), (std::streamsize)n);
	}
	iso.Close(fd);
	f.close();
	if (!ok || f.fail()) {
		std::error_code ec;
		fs::remove(out, ec);
		return 3;
	}
	return 0;
}

}  // namespace

static int Main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: pspc-iso info|ls|get|modules <image> [...]\n");
		return 1;
	}
	std::string cmd = argv[1];
	auto iso = Open(argv[2]);
	if (!iso) return 2;
	if (cmd == "info") {
		std::vector<u8> sfo;
		if (iso->ReadWholeFile("/PSP_GAME/PARAM.SFO", sfo, vfs::kMaxMetadataBytes) == 0) {
			vfs::ParamSfo p;
			if (p.Parse(sfo)) {
				printf("DISC_ID=%s\nTITLE=%s\nDISC_VERSION=%s\nPSP_SYSTEM_VER=%s\n", p.GetString("DISC_ID").c_str(),
				       p.GetString("TITLE").c_str(), p.GetString("DISC_VERSION").c_str(),
				       p.GetString("PSP_SYSTEM_VER").c_str());
			}
		}
		Walk(*iso, "/", [&](const std::string &path, bool isDir, u64 size) {
			if (isDir || size < 4 || size > 64u << 20) return;
			if (IsExecutable(*iso, path)) printf("EXEC %s %llu\n", path.c_str(), (unsigned long long)size);
		});
		return 0;
	}
	if (cmd == "ls") {
		std::string dir = argc > 3 ? argv[3] : "/";
		Walk(*iso, dir, [](const std::string &path, bool isDir, u64 size) {
			printf("%s %10llu %s\n", isDir ? "d" : "-", (unsigned long long)size, path.c_str());
		});
		return 0;
	}
	if (cmd == "get" && argc >= 5) {
		int r = Extract(*iso, argv[3], fs::u8path(argv[4]));
		if (r == 2) fprintf(stderr, "%s not found\n", argv[3]);
		if (r == 3) fprintf(stderr, "%s: can't read it or write %s\n", argv[3], argv[4]);
		return r ? 2 : 0;
	}
	if (cmd == "modules" && argc >= 4) {
		fs::path out = fs::u8path(argv[3]);
		int n = 0;
		Walk(*iso, "/", [&](const std::string &path, bool isDir, u64 size) {
			if (isDir || size < 4 || size > 64u << 20) return;
			// SYSDIR/UPDATE holds the Sony firmware updater, which a game never runs.
			if (path.rfind("/PSP_GAME/SYSDIR/UPDATE/", 0) == 0) return;
			// BOOT.BIN is often an all-zero placeholder; IsExecutable filters that.
			if (!IsExecutable(*iso, path)) return;
			std::string flat = path.substr(1);
			for (char &ch : flat)
				if (ch == '/') ch = '_';
			if (Extract(*iso, path, out / flat) == 0) {
				printf("%s\t%s\n", path.c_str(), PathToUtf8(out / flat).c_str());
				n++;
			}
		});
		return n ? 0 : 3;
	}
	fprintf(stderr, "unknown command %s\n", cmd.c_str());
	return 1;
}

int wmain(int argc, wchar_t **argv) { return RunUtf8Main(Main, argc, argv); }
