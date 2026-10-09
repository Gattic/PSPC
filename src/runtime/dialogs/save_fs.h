// SPDX-License-Identifier: GPL-2.0-or-later
//
// File access for the savedata/gamedata utilities through the PSP device namespace (io::Device),
// so they see exactly what the game sees through sceIo.
#pragma once

#include <string>
#include <vector>

#include "fs/psp_io_types.h"

namespace utility::savefs {

struct FileInfo {
	std::string name;  // last path component
	bool exists = false;
	bool isDir = false;
	s64 size = 0;
	vfs::ScePspDateTime ctime{}, atime{}, mtime{};
};

FileInfo Stat(const std::string &pspPath);
bool Exists(const std::string &pspPath);
// Entries of a directory, without "." and "..". `exists` tells whether the directory was there.
std::vector<FileInfo> List(const std::string &pspDir, bool *exists = nullptr);
bool ReadAll(const std::string &pspPath, std::vector<u8> &out);
// Reads at most maxSize bytes into dst. Returns the bytes read, or -1.
s64 ReadInto(const std::string &pspPath, u8 *dst, u32 maxSize);
bool WriteAll(const std::string &pspPath, const void *data, size_t size);
bool Mkdir(const std::string &pspPath);
bool Remove(const std::string &pspPath);
// Removes a directory and everything in it.
bool RemoveTree(const std::string &pspPath);
// Total size of the files under a directory, recursively.
u64 TreeSize(const std::string &pspPath);

}  // namespace utility::savefs
