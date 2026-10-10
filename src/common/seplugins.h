// SPDX-License-Identifier: GPL-2.0-or-later
//
// M33's plugin lists: ms0:/seplugins/vsh.txt (XMB), game.txt (games and homebrew) and pops.txt
// (PS1 games), one PRX path per line. Whether a plugin is on comes from a second token on its
// line ("ms0:/seplugins/cheat.prx 1"), else from its byte in seplugins/conf.bin (five slots per
// list: vsh 0-4, game 5-9, pops 10-14, as M33's recovery menu writes them), else on.
// pspc reads the lists at boot (m33::LoadPlugins) and the launcher edits them.
#pragma once

#include <string>
#include <vector>

#include "common/common.h"
#include "common/fileutil.h"

namespace seplugins {

enum class List { Vsh, Game, Pops };
constexpr List kLists[] = {List::Vsh, List::Game, List::Pops};
const char *FileName(List l);  // "vsh.txt"...
int ConfBase(List l);           // the list's first conf.bin slot

struct Entry {
	std::string path;   // as written, e.g. "ms0:/seplugins/cheat.prx"
	bool enabled = true;
	int line = 0;       // line number in the list file (0-based)
};

// Entries of one list's text (blank lines skipped).
std::vector<Entry> Parse(const std::string &text, List list, const std::vector<u8> &conf);

// File helpers for a host folder laid over ms0: (pspc's plugins folder).
std::vector<Entry> ReadList(const fs::path &root, List list);
// Rewrites the list with every entry's flag ("path 1"/"path 0"), keeping other lines and the
// file's line endings. Entries without a line (line < 0) are appended.
bool WriteList(const fs::path &root, List list, const std::vector<Entry> &entries);
// Host file an "ms0:/..." path names under `root` (case-insensitive lookup); empty if it doesn't
// exist.
fs::path HostPath(const fs::path &root, const std::string &ms0Path);

}  // namespace seplugins
