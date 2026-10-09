// SPDX-License-Identifier: GPL-2.0-or-later
//
// High-level emulation of PSP system libraries. Each library is a table of NID -> native
// function. When a module is loaded, every import stub is rewritten to "jr ra; syscall CODE"
// and registered in the dispatch table, so both the interpreter and recompiled code reach the
// native implementation.
#pragma once

#include <filesystem>
#include <string>

#include "allegrex/ctx.h"

namespace hle {

using HleFn = void (*)(Ctx *);

enum HleFlags : u32 {
	HLE_NOT_IN_INTERRUPT = 1,
	HLE_KERNEL_SYSCALL = 2,
};

struct HleFunction {
	u32 nid;
	HleFn fn;
	const char *name;
	u32 flags;
};

void Init();
void RegisterModule(const char *library, const HleFunction *funcs, size_t count);
template <size_t N>
void RegisterModule(const char *library, const HleFunction (&funcs)[N]) {
	RegisterModule(library, funcs, N);
}

const HleFunction *FindFunction(const std::string &library, u32 nid);
bool IsLibraryKnown(const std::string &library);

// Syscall code for (library, nid); allocated on first use. Used for runtime-internal stubs.
u32 GetSyscallCode(const std::string &library, u32 nid);

// Writes "jr ra; syscall code" into the stub at stubAddr and registers it for dispatch.
void PatchImportStub(u32 stubAddr, const std::string &library, u32 nid);
// Points an import stub at a guest export of another module ("j target; nop").
void PatchImportStubToGuest(u32 stubAddr, const std::string &library, u32 nid, u32 target);

void Syscall(Ctx *c, u32 insn, u32 pc);

// Name database (data/nids.csv) used for logging unimplemented functions.
void LoadNidDatabase(const std::filesystem::path &csv);
std::string NidName(const std::string &library, u32 nid);

// Information about the HLE call in progress (valid inside an HLE function).
const char *CurrentFunctionName();
u32 CurrentCallerPc();

// Per-call controls.
void SetReturnU64(Ctx *c, u64 v);
// Report unimplemented NIDs that were called, sorted by call count.
void DumpUnimplementedStats();

}  // namespace hle
