// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "frontend/game_library.h"

namespace frontend {
enum class Native { None, Partial, Stale, Built };
struct NativeInfo {
	Native state = Native::None;
	int built = 0, total = 0, failed = 0;
	// A stale module was built by another PSPC version and won't load. Otherwise stale only means
	// play reached code its build missed (a newer .hints): the old DLL still runs.
	bool pspcChanged = false;
	std::string error;
	bool operator==(const NativeInfo &) const = default;
};
// Platform module suffix, semantics stamp, hints, duplicate modules and moved images
// are interpreted identically by both launchers.
NativeInfo InspectNativeCache(const std::filesystem::path &cache, const GameInfo &game, u32 semanticsVersion);
const char *NativeLabel(Native state);
} // namespace frontend
