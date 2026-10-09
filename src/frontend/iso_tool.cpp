// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc-iso: inspect UMD images (ISO/CSO/ZSO/DAX).
//
//   pspc-iso info <image>                   disc id, title, executables
//   pspc-iso ls <image> [dir]               list a directory
//   pspc-iso get <image> <path> <outfile>   extract one file
//   pspc-iso modules <image> <outdir>       extract every executable (EBOOT.BIN, BOOT.BIN, *.PRX)
#include <cstdio>
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

bool IsExecutable(const std::vector<u8> &d) {
	return d.size() >= 4 && ((d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F') ||
	                         (d[0] == '~' && d[1] == 'P' && d[2] == 'S' && d[3] == 'P'));
}

}  // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: pspc-iso info|ls|get|modules <image> [...]\n");
		return 1;
	}
	std::string cmd = argv[1];
	auto iso = Open(argv[2]);
	if (!iso) return 2;
	if (cmd == "info") {
		std::vector<u8> sfo;
		if (iso->ReadWholeFile("/PSP_GAME/PARAM.SFO", sfo) == 0) {
			vfs::ParamSfo p;
			if (p.Parse(sfo)) {
				printf("DISC_ID=%s\nTITLE=%s\nDISC_VERSION=%s\nPSP_SYSTEM_VER=%s\n", p.GetString("DISC_ID").c_str(),
				       p.GetString("TITLE").c_str(), p.GetString("DISC_VERSION").c_str(),
				       p.GetString("PSP_SYSTEM_VER").c_str());
			}
		}
		Walk(*iso, "/", [&](const std::string &path, bool isDir, u64 size) {
			if (isDir) return;
			std::vector<u8> head;
			if (size < 4 || size > 64u << 20) return;
			if (iso->ReadWholeFile(path, head) == 0 && IsExecutable(head)) printf("EXEC %s %llu\n", path.c_str(), (unsigned long long)size);
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
		std::vector<u8> data;
		if (iso->ReadWholeFile(argv[3], data) != 0) {
			fprintf(stderr, "%s not found\n", argv[3]);
			return 2;
		}
		return WriteWholeFile(fs::u8path(argv[4]), data.data(), data.size()) ? 0 : 2;
	}
	if (cmd == "modules" && argc >= 4) {
		fs::path out = fs::u8path(argv[3]);
		int n = 0;
		Walk(*iso, "/", [&](const std::string &path, bool isDir, u64 size) {
			if (isDir || size < 4 || size > 64u << 20) return;
			// SYSDIR/UPDATE holds the Sony firmware updater, which a game never runs.
			if (path.rfind("/PSP_GAME/SYSDIR/UPDATE/", 0) == 0) return;
			std::vector<u8> data;
			if (iso->ReadWholeFile(path, data) != 0 || !IsExecutable(data)) return;
			// BOOT.BIN is often an all-zero placeholder; IsExecutable already filtered that.
			std::string flat = path.substr(1);
			for (char &ch : flat)
				if (ch == '/') ch = '_';
			if (WriteWholeFile(out / flat, data.data(), data.size())) {
				printf("%s\t%s\n", path.c_str(), (out / flat).string().c_str());
				n++;
			}
		});
		return n ? 0 : 3;
	}
	fprintf(stderr, "unknown command %s\n", cmd.c_str());
	return 1;
}
