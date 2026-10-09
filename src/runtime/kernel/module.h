// SPDX-License-Identifier: GPL-2.0-or-later
// Loaded PSP modules (executables and PRX libraries).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "loader/elf.h"
#include "loader/module_tables.h"
#include "runtime/kernel/kobject.h"

namespace kernel {

enum ModuleStatus : u32 {
	MODULE_STATUS_LOADED = 1,
	MODULE_STATUS_STARTING = 2,
	MODULE_STATUS_STARTED = 3,
	MODULE_STATUS_STOPPING = 4,
	MODULE_STATUS_STOPPED = 5,
};

struct PspModule : KObject {
	static constexpr KType kType = KType::Module;
	KType Type() const override { return kType; }

	std::string path;
	loader::LoadedImage image;
	std::vector<loader::ImportLib> imports;
	std::vector<loader::ExportLib> exports;
	std::array<u8, 20> sha1{};
	u32 memAddr = 0, memSize = 0;
	u32 gp = 0;
	u32 moduleStart = 0, moduleStop = 0;
	u32 startThreadPrio = 0, startThreadStack = 0, startThreadAttr = 0;
	bool hasStartThreadParam = false;
	u32 stopThreadPrio = 0, stopThreadStack = 0, stopThreadAttr = 0;
	u32 status = MODULE_STATUS_LOADED;
	bool kernelMode = false;
	bool aot = false;
	void *dll = nullptr;
	u32 aotRecompileBase = 0;
};

struct LoadOptions {
	bool kernel = false;   // load into the kernel partition
	u32 fixedBase = 0;     // force a load address for relocatable modules (0 = allocate)
};

// Loads an executable image (ELF/PRX, possibly ~PSP packed). `path` is the PSP path it came
// from. Returns the module UID or a negative PSP error.
SceUID LoadModuleFromMemory(const std::vector<u8> &data, const std::string &path, const LoadOptions &opts);
// Starts a module: runs module_start in a new thread (deleted when it returns). With
// waitForReturn (and a calling guest thread), blocks until module_start returns and stores its
// exit status at statusPtr. smOptionPtr is a guest SceKernelSMOption (0 = none). Returns 0 or an
// error.
int StartModule(PspModule *m, u32 argSize, u32 argPtr, u32 statusPtr, bool waitForReturn, u32 smOptionPtr = 0);
int StopModule(PspModule *m, u32 argSize, u32 argPtr, u32 statusPtr, u32 smOptionPtr = 0);
// Checks a guest SceKernelSMOption (size for newer SDKs, attribute bits).
int ValidateSMOption(u32 smOptionPtr);
int UnloadModule(PspModule *m);
PspModule *ModuleByAddress(u32 addr);
PspModule *ModuleByName(const std::string &name);
const std::vector<SceUID> &LoadedModules();

// Directory for AOT-compiled module DLLs (searched by SHA-1).
void SetAotCacheDir(const std::string &dir);
// Disable loading AOT DLLs (interpreter only).
void SetAotEnabled(bool enabled);
// Path of the pspc-decrypt helper, used for encrypted modules.
void SetDecryptTool(const std::string &exe);
// Directory where decrypted copies of encrypted modules are cached.
void SetDecryptCacheDir(const std::string &dir);

}  // namespace kernel
