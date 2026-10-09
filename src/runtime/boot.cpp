// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/boot.h"

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/sysmem.h"

namespace boot {

namespace {
bool g_loadExecPending = false;
std::string g_loadExecPath;
std::vector<u8> g_loadExecArgs;
}  // namespace

SceUID BootModule(const std::vector<u8> &image, const std::string &pspPath) {
	kernel::LoadOptions opts;
	SceUID id = kernel::LoadModuleFromMemory(image, pspPath, opts);
	if (id < 0) return id;
	kernel::PspModule *m = kernel::Get<kernel::PspModule>(id);
	// argv block: the executable path, NUL terminated.
	u32 argSize = (u32)pspPath.size() + 1;
	u32 argp = kernel::AllocKernelMemory(AlignUp(argSize, 16), "boot args");
	mem::WriteBytes(argp, pspPath.c_str(), argSize);
	int r = kernel::StartModule(m, argSize, argp, 0, false);
	if (r < 0) return r;
	return id;
}

void RequestLoadExec(const std::string &pspPath, const std::vector<u8> &args) {
	g_loadExecPending = true;
	g_loadExecPath = pspPath;
	g_loadExecArgs = args;
}

bool TakeLoadExecRequest(std::string &pspPath, std::vector<u8> &args) {
	if (!g_loadExecPending) return false;
	g_loadExecPending = false;
	pspPath = g_loadExecPath;
	args = g_loadExecArgs;
	return true;
}

}  // namespace boot
