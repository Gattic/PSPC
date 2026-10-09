// SPDX-License-Identifier: GPL-2.0-or-later
#include "fs/path_util.h"

namespace vfs {

bool SplitPspDevice(std::string_view path, std::string_view *device, std::string_view *rest) {
	for (size_t i = 0; i < path.size(); i++) {
		char c = path[i];
		if (c == '/' || c == '\\') {
			break;
		}
		if (c == ':') {
			if (i == 0) {
				break;
			}
			*device = path.substr(0, i + 1);
			*rest = path.substr(i + 1);
			return true;
		}
	}
	*device = std::string_view();
	*rest = path;
	return false;
}

std::vector<std::string> SplitPspPathComponents(std::string_view path) {
	std::vector<std::string> parts;
	size_t i = 0;
	while (i < path.size()) {
		size_t j = i;
		while (j < path.size() && path[j] != '/' && path[j] != '\\') {
			j++;
		}
		std::string_view part = path.substr(i, j - i);
		if (part.empty() || part == ".") {
			// Nothing.
		} else if (part == "..") {
			if (!parts.empty()) {
				parts.pop_back();
			}
		} else {
			parts.emplace_back(part);
		}
		i = j + 1;
	}
	return parts;
}

static std::string JoinComponents(std::string_view device, const std::vector<std::string> &parts) {
	std::string out(device);
	if (parts.empty()) {
		out += '/';
		return out;
	}
	for (const std::string &p : parts) {
		out += '/';
		out += p;
	}
	return out;
}

std::string NormalizePspPath(std::string_view path) {
	std::string_view device, rest;
	SplitPspDevice(path, &device, &rest);
	if (rest.empty()) {
		return std::string(device);
	}
	return JoinComponents(device, SplitPspPathComponents(rest));
}

std::string NormalizePspPath(std::string_view cwd, std::string_view path) {
	std::string_view device, rest;
	if (SplitPspDevice(path, &device, &rest)) {
		return NormalizePspPath(path);
	}
	std::string_view cwdDevice, cwdRest;
	SplitPspDevice(cwd, &cwdDevice, &cwdRest);
	if (path.empty()) {
		return NormalizePspPath(cwd);
	}
	std::string joined(cwdRest);
	joined += '/';
	joined += path;
	return JoinComponents(cwdDevice, SplitPspPathComponents(joined));
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); i++) {
		char x = a[i], y = b[i];
		if (x >= 'a' && x <= 'z') x -= 32;
		if (y >= 'a' && y <= 'z') y -= 32;
		if (x != y) {
			return false;
		}
	}
	return true;
}

}  // namespace vfs
