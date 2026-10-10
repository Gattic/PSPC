// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/seplugins.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace seplugins {

const char *FileName(List l) {
	switch (l) {
	case List::Vsh: return "vsh.txt";
	case List::Pops: return "pops.txt";
	default: return "game.txt";
	}
}

int ConfBase(List l) {
	switch (l) {
	case List::Vsh: return 0;
	case List::Pops: return 10;
	default: return 5;
	}
}

namespace {

void TrimRight(std::string &s) {
	while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
}

std::vector<std::string> Lines(const std::string &text) {
	std::vector<std::string> lines;
	std::istringstream in(text);
	for (std::string l; std::getline(in, l);) lines.push_back(l);
	return lines;
}

fs::path ListPath(const fs::path &root, List list) { return root / "seplugins" / FileName(list); }

}  // namespace

std::vector<Entry> Parse(const std::string &text, List list, const std::vector<u8> &conf) {
	std::vector<Entry> out;
	const std::vector<std::string> lines = Lines(text);
	int index = 0;
	for (size_t i = 0; i < lines.size(); i++) {
		std::string line = lines[i];
		TrimRight(line);
		const size_t start = line.find_first_not_of(" \t");
		if (start == std::string::npos) continue;
		line = line.substr(start);
		Entry e;
		e.line = (int)i;
		e.path = line;
		const size_t slot = (size_t)(ConfBase(list) + index);
		e.enabled = slot < conf.size() ? conf[slot] != 0 : true;
		const size_t sp = line.find_first_of(" \t");
		if (sp != std::string::npos) {
			e.path = line.substr(0, sp);
			const size_t flag = line.find_first_not_of(" \t", sp);
			if (flag != std::string::npos) e.enabled = line.substr(flag) != "0";
		}
		index++;
		out.push_back(std::move(e));
	}
	return out;
}

std::vector<Entry> ReadList(const fs::path &root, List list) {
	std::vector<u8> txt, conf;
	if (!ReadWholeFile(ListPath(root, list), txt)) return {};
	ReadWholeFile(root / "seplugins" / "conf.bin", conf);
	return Parse(std::string(txt.begin(), txt.end()), list, conf);
}

bool WriteList(const fs::path &root, List list, const std::vector<Entry> &entries) {
	std::vector<u8> old;
	ReadWholeFile(ListPath(root, list), old);
	const std::string text(old.begin(), old.end());
	const bool crlf = text.empty() || text.find("\r\n") != std::string::npos;
	std::vector<std::string> lines = Lines(text);
	for (std::string &l : lines) TrimRight(l);
	for (const Entry &e : entries) {
		const std::string line = e.path + (e.enabled ? " 1" : " 0");
		if (e.line >= 0 && (size_t)e.line < lines.size()) lines[(size_t)e.line] = line;
		else lines.push_back(line);
	}
	std::string out;
	for (const std::string &l : lines) out += l + (crlf ? "\r\n" : "\n");
	std::error_code ec;
	fs::create_directories(root / "seplugins", ec);
	return WriteWholeFile(ListPath(root, list), out.data(), out.size());
}

fs::path HostPath(const fs::path &root, const std::string &ms0Path) {
	std::string p = ms0Path;
	const size_t colon = p.find(':');
	if (colon != std::string::npos) {
		std::string dev = p.substr(0, colon);
		std::transform(dev.begin(), dev.end(), dev.begin(), [](unsigned char c) { return (char)tolower(c); });
		if (dev != "ms0" && dev != "ef0") return {};
		p = p.substr(colon + 1);
	}
	fs::path cur = root;
	std::istringstream parts(p);
	std::error_code ec;
	for (std::string part; std::getline(parts, part, '/');) {
		if (part.empty() || part == ".") continue;
		fs::path next = cur / fs::u8path(part);
		if (!fs::exists(next, ec)) {
			// FAT names are case-insensitive.
			bool found = false;
			for (auto it = fs::directory_iterator(cur, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
				std::string a = PathToUtf8(it->path().filename()), b = part;
				std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return (char)tolower(c); });
				std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return (char)tolower(c); });
				if (a == b) {
					next = it->path();
					found = true;
					break;
				}
			}
			if (!found) return {};
		}
		cur = next;
	}
	return fs::is_regular_file(cur, ec) ? cur : fs::path();
}

}  // namespace seplugins
