// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/common.h"
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace gputest {
inline u8 *Reserve(size_t size) {
#ifdef _WIN32
	auto *memory = (u8 *)VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
	if (!memory) throw std::runtime_error("Cannot reserve test guest memory");
#else
	auto *memory = (u8 *)mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED) throw std::runtime_error("Cannot reserve test guest memory");
#endif
	return memory;
}
inline void Commit(u8 *memory, size_t size) {
#ifdef _WIN32
	bool ok = VirtualAlloc(memory, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
	bool ok = mprotect(memory, size, PROT_READ | PROT_WRITE) == 0;
#endif
	if (!ok) throw std::runtime_error("Cannot commit test guest memory");
}
inline void Release(u8 *memory, size_t size) {
#ifdef _WIN32
	bool ok = VirtualFree(memory, 0, MEM_RELEASE) != 0;
#else
	bool ok = munmap(memory, size) == 0;
#endif
	if (!ok) throw std::runtime_error("Cannot release test guest memory");
}
}  // namespace gputest
