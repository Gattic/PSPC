// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/FileSystems/FileSystem.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
// pspautotests io/shortname records what the PSP generates.
#include "fs/fat_names.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string_view>

namespace vfs {

namespace {

bool IsValidShortNameChar(char c) {
	if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
		return true;
	}
	return strchr("$%'-_@~`!(){}^#&", c) != nullptr;
}

// Uppercases and strips what FAT doesn't accept; `lossy` says whether that lost information,
// which decides between the plain name and a ~N one.
std::string CleanPart(std::string_view part, size_t maxLen, bool *lossy) {
	std::string out;
	for (char c : part) {
		if (c >= 'a' && c <= 'z') {
			c = (char)(c - 'a' + 'A');  // short names ignore case: not a loss by itself
		}
		if (c == ' ' || c == '.') {
			*lossy = true;
			continue;
		}
		if (!IsValidShortNameChar(c)) {
			c = '_';
			*lossy = true;
		}
		if (out.size() >= maxLen) {
			*lossy = true;
			break;
		}
		out.push_back(c);
	}
	return out;
}

// The PSP only honours the base's lowercase flag: a lowercase extension (or a mixed-case base)
// needs a long name entry and so a ~1 short name ("shrt" -> SHRT, "readme.txt" -> README~1.TXT).
bool CaseSurvives(std::string_view base, std::string_view ext) {
	bool lower = false, upper = false;
	for (char c : base) {
		lower = lower || (c >= 'a' && c <= 'z');
		upper = upper || (c >= 'A' && c <= 'Z');
	}
	if (lower && upper) {
		return false;
	}
	for (char c : ext) {
		if (c >= 'a' && c <= 'z') {
			return false;
		}
	}
	return true;
}

}  // namespace

std::vector<std::string> GenerateFatShortNames(const std::vector<std::string> &names) {
	std::vector<std::string> out;
	out.reserve(names.size());
	std::set<std::string> taken;
	for (const std::string &name : names) {
		if (name == "." || name == "..") {
			out.push_back(name);
			continue;
		}
		// The extension follows the last dot; a leading dot belongs to the name.
		size_t dot = name.find_last_of('.');
		bool lossy = false;
		std::string_view baseIn = name, extIn;
		if (dot != std::string::npos && dot != 0) {
			baseIn = std::string_view(name).substr(0, dot);
			extIn = std::string_view(name).substr(dot + 1);
		} else if (dot == 0) {
			lossy = true;
		}
		if (!CaseSurvives(baseIn, extIn)) {
			lossy = true;
		}
		std::string base = CleanPart(baseIn, 8, &lossy);
		std::string ext = CleanPart(extIn, 3, &lossy);
		if (base.empty()) {
			base = "_";
			lossy = true;
		}
		std::string candidate;
		if (!lossy) {
			candidate = ext.empty() ? base : base + "." + ext;
			if (taken.count(candidate)) {
				candidate.clear();
			}
		}
		for (int n = 1; candidate.empty(); n++) {
			char suffix[16];
			snprintf(suffix, sizeof(suffix), "~%d", n);
			std::string attempt = base.substr(0, std::max<size_t>(1, 8 - strlen(suffix))) + suffix;
			if (!ext.empty()) {
				attempt += "." + ext;
			}
			if (!taken.count(attempt)) {
				candidate = attempt;
			}
		}
		taken.insert(candidate);
		out.push_back(candidate);
	}
	return out;
}

}  // namespace vfs
