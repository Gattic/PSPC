// SPDX-License-Identifier: GPL-2.0-or-later
#include "fs/filesystem.h"

#include <cstdint>

namespace vfs {

bool ReadRest(FileSystem &fs, s32 handle, std::vector<u8> &out) {
	HandleInfo info;
	if (fs.GetHandleInfo(handle, &info) < 0 || info.sectorMode) {
		return false;
	}
	const s64 pos = fs.Seek(handle, 0, PSP_SEEK_CUR);
	const s64 end = fs.Seek(handle, 0, PSP_SEEK_END);
	fs.Seek(handle, pos, PSP_SEEK_SET);
	if (pos < 0 || end < pos || (u64)(end - pos) > SIZE_MAX / 2) {
		return false;
	}
	out.resize((size_t)(end - pos));
	const s64 n = out.empty() ? 0 : fs.Read(handle, out.data(), out.size());
	fs.Seek(handle, pos, PSP_SEEK_SET);
	if (n < 0) {
		out.clear();
		return false;
	}
	out.resize((size_t)n);
	return true;
}

}  // namespace vfs
