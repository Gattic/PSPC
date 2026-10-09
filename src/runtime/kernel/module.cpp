// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/kernel/module.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <map>

#include "allegrex/aot_abi.h"
#include "pspc_sem_version.h"
#include "allegrex/guestmem.h"
#include "common/fileutil.h"
#include "common/log.h"
#include "common/sha1.h"
#include "loader/psp_header.h"
#include "runtime/core/cpu.h"
#include "runtime/core/dispatch.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/threads.h"

namespace kernel {

namespace {

std::vector<SceUID> g_modules;
std::string g_aotDir;
bool g_aotEnabled = true;
std::string g_decryptTool;
std::string g_decryptCache;

u8 *GuestPtr(u32 addr, u32 size) { return mem::GetPointer(addr, size); }
const u8 *GuestReadPtr(u32 addr, u32 size) { return mem::GetPointer(addr, size); }

bool RunDecryptTool(const std::vector<u8> &in, std::vector<u8> &out, std::string *err) {
	if (g_decryptTool.empty() || !fs::exists(g_decryptTool)) {
		if (err) *err = "module is encrypted or packed and pspc-decrypt.exe was not found";
		return false;
	}
	auto hash = Sha1::Hash(in.data(), in.size());
	std::string hex = HexString(hash.data(), hash.size());
	fs::path cacheDir = g_decryptCache.empty() ? fs::temp_directory_path() / "pspc-decrypt" : fs::path(g_decryptCache);
	std::error_code ec;
	fs::create_directories(cacheDir, ec);
	fs::path cached = cacheDir / (hex + ".elf");
	if (fs::exists(cached) && ReadWholeFile(cached, out)) return true;
	fs::path inPath = cacheDir / (hex + ".in");
	if (!WriteWholeFile(inPath, in.data(), in.size())) {
		if (err) *err = "cannot write decrypt input";
		return false;
	}
	std::wstring cmd = L"\"" + fs::path(g_decryptTool).wstring() + L"\" prx \"" + inPath.wstring() + L"\" \"" +
	                   cached.wstring() + L"\"";
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		if (err) *err = "failed to launch pspc-decrypt";
		return false;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	fs::remove(inPath, ec);
	if (code != 0 || !ReadWholeFile(cached, out)) {
		if (err) *err = code == 3 ? "pspc-decrypt has no key for this module" : "pspc-decrypt failed";
		return false;
	}
	return true;
}

// Bind every import of `m` either to a guest export of another loaded module or to HLE.
void ResolveImports(PspModule *m) {
	for (const loader::ImportLib &lib : m->imports) {
		for (const loader::ImportFunc &f : lib.funcs) {
			u32 target = 0;
			for (SceUID id : g_modules) {
				PspModule *other = Get<PspModule>(id);
				if (!other || other == m) continue;
				for (const loader::ExportLib &el : other->exports) {
					if (el.name != lib.name) continue;
					for (auto &ef : el.funcs)
						if (ef.first == f.nid) target = ef.second;
				}
			}
			if (target && !hle::FindFunction(lib.name, f.nid)) {
				hle::PatchImportStubToGuest(f.stubAddr, lib.name, f.nid, target);
			} else {
				hle::PatchImportStub(f.stubAddr, lib.name, f.nid);
			}
		}
		for (const loader::ImportVar &v : lib.vars) {
			u32 exportAddr = 0;
			for (SceUID id : g_modules) {
				PspModule *other = Get<PspModule>(id);
				if (!other || other == m) continue;
				for (const loader::ExportLib &el : other->exports) {
					if (el.name != lib.name) continue;
					for (auto &ev : el.vars)
						if (ev.first == v.nid) exportAddr = ev.second;
				}
			}
			if (!exportAddr) {
				LOGW("LOADER", "%s: unresolved variable import %s::%08x", m->name.c_str(), lib.name.c_str(), v.nid);
				continue;
			}
			// HI16 relocations are fixed up against the LO16 that follows (see PPSSPP WriteVarSymbol).
			std::vector<std::pair<u32, u32>> pendingHi;
			for (const loader::VarRef &ref : v.refs) {
				u32 op = GRead32(ref.addr);
				switch (ref.type) {
				case loader::R_MIPS_32: GWrite32(ref.addr, op + exportAddr); break;
				case loader::R_MIPS_HI16: pendingHi.push_back({ref.addr, op}); break;
				case loader::R_MIPS_LO16: {
					u32 offLo = (u32)(s32)(s16)(op & 0xFFFF);
					u32 full = offLo + exportAddr;
					for (auto &hi : pendingHi) {
						full = (hi.second << 16) + offLo + exportAddr;
						u16 high = (u16)((full >> 16) + ((full & 0x8000) ? 1 : 0));
						GWrite32(hi.first, (hi.second & ~0xFFFFu) | high);
					}
					pendingHi.clear();
					GWrite32(ref.addr, (op & ~0xFFFFu) | (full & 0xFFFF));
					break;
				}
				default: break;
				}
			}
		}
	}
}

// A new module exports libraries: rebind earlier modules that imported them but got HLE stubs
// with no implementation.
void RebindImportersOf(PspModule *exporter) {
	for (SceUID id : g_modules) {
		PspModule *m = Get<PspModule>(id);
		if (!m || m == exporter) continue;
		for (const loader::ImportLib &lib : m->imports) {
			for (const loader::ExportLib &el : exporter->exports) {
				if (el.name != lib.name || el.name.empty()) continue;
				for (const loader::ImportFunc &f : lib.funcs) {
					if (hle::FindFunction(lib.name, f.nid)) continue;
					for (auto &ef : el.funcs)
						if (ef.first == f.nid) hle::PatchImportStubToGuest(f.stubAddr, lib.name, f.nid, ef.second);
				}
			}
		}
	}
}

bool TryLoadAot(PspModule *m, const loader::ElfFile &elf, u32 *requiredBase) {
	if (!g_aotEnabled || g_aotDir.empty()) return false;
	std::string hex = HexString(m->sha1.data(), m->sha1.size());
	fs::path dllPath = fs::path(g_aotDir) / (hex + ".dll");
	if (!fs::exists(dllPath)) return false;
	HMODULE h = LoadLibraryW(dllPath.wstring().c_str());
	if (!h) {
		LOGW("AOT", "LoadLibrary(%s) failed: %lu", dllPath.string().c_str(), GetLastError());
		return false;
	}
	auto entry = (PspcAotModuleFn)GetProcAddress(h, PSPC_AOT_ENTRY_NAME);
	const AotModuleDesc *desc = entry ? entry(cpu::AotApi()) : nullptr;
	if (!desc || desc->abiVersion != PSPC_AOT_ABI_VERSION || desc->semVersion != PSPC_SEM_VERSION ||
	    memcmp(desc->sha1, m->sha1.data(), 20) != 0) {
		LOGW("AOT", "%s is stale or mismatched; ignoring", dllPath.string().c_str());
		FreeLibrary(h);
		return false;
	}
	m->dll = h;
	m->aot = true;
	m->aotRecompileBase = desc->recompileBase;
	*requiredBase = desc->recompileBase;
	(void)elf;
	return true;
}

void RegisterAotFunctions(PspModule *m) {
	auto entry = (PspcAotModuleFn)GetProcAddress((HMODULE)m->dll, PSPC_AOT_ENTRY_NAME);
	const AotModuleDesc *desc = entry(cpu::AotApi());
	u32 delta = m->image.relocatable ? (m->image.lowAddr - desc->recompileBase) : 0;
	*desc->delta = delta;
	for (u32 i = 0; i < desc->numFunctions; i++)
		dispatch::Register(desc->recompileBase + delta + desc->functions[i].offset, desc->functions[i].fn);
	LOGI("AOT", "%s: %u native functions (delta %08x)", m->name.c_str(), desc->numFunctions, delta);
}

}  // namespace

void SetAotCacheDir(const std::string &dir) { g_aotDir = dir; }
void SetAotEnabled(bool enabled) { g_aotEnabled = enabled; }
void SetDecryptTool(const std::string &exe) { g_decryptTool = exe; }
void SetDecryptCacheDir(const std::string &dir) { g_decryptCache = dir; }
const std::vector<SceUID> &LoadedModules() { return g_modules; }

SceUID LoadModuleFromMemory(const std::vector<u8> &input, const std::string &path, const LoadOptions &opts) {
	std::vector<u8> plain;
	const std::vector<u8> *data = &input;
	loader::ContainerKind kind = loader::Classify(input.data(), input.size());
	if (kind == loader::ContainerKind::Unknown) {
		LOGE("LOADER", "%s: not an executable", path.c_str());
		return (s32)SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;
	}
	if (kind != loader::ContainerKind::PlainElf) {
		std::string err;
		if (!RunDecryptTool(input, plain, &err)) {
			LOGE("LOADER", "%s: %s", path.c_str(), err.c_str());
			return (s32)SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;
		}
		data = &plain;
	}

	loader::ElfFile elf;
	std::string err;
	if (!elf.Parse(data->data(), data->size(), &err)) {
		LOGE("LOADER", "%s: %s", path.c_str(), err.c_str());
		return (s32)SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;
	}

	auto *m = new PspModule();
	m->path = path;
	m->sha1 = Sha1::Hash(data->data(), data->size());
	m->name = elf.HasModuleInfo() ? std::string(elf.ModuleInfo().name, strnlen(elf.ModuleInfo().name, 28)) : "ELF";
	m->kernelMode = opts.kernel || elf.IsKernelModule();

	u32 aotBase = 0;
	bool aot = TryLoadAot(m, elf, &aotBase);

	BlockAllocator &alloc = (m->kernelMode && elf.TotalStart() < 0x08800000 && !elf.Relocatable()) ||
	                                (m->kernelMode && elf.Relocatable())
	                            ? g_kernelMem
	                            : g_userMem;
	u32 size = elf.TotalSize();
	u32 base = 0;
	if (!elf.Relocatable()) {
		u32 start = elf.TotalStart();
		BlockAllocator *a = AllocatorForAddress(start);
		if (!a || !a->AllocAt(start, size, m->name.c_str())) {
			LOGE("LOADER", "%s: cannot reserve %08x-%08x", path.c_str(), start, start + size);
			delete m;
			return (s32)SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
		}
		m->memAddr = start;
	} else {
		u32 align = std::max<u32>(elf.FirstSegmentAlign(), 0x100);
		if (align & (align - 1)) align = 0x100;
		u32 addr = 0;
		if (opts.fixedBase) {
			addr = alloc.AllocAt(opts.fixedBase + elf.TotalStart(), size, m->name.c_str());
		} else if (aot) {
			// Prefer the exact recompile address; else keep the low 16 bits so LO16-only
			// relocations stay valid in the generated code.
			addr = alloc.AllocAt(aotBase + elf.TotalStart(), size, m->name.c_str());
			if (!addr) {
				u32 blk = alloc.Alloc(size + 0x10000, false, m->name.c_str(), 0x10000);
				if (blk) {
					alloc.Free(blk);
					addr = alloc.AllocAt(blk + (aotBase & 0xFFFF), size, m->name.c_str());
				}
			}
		} else {
			addr = alloc.Alloc(size, false, m->name.c_str(), align);
		}
		if (!addr) {
			LOGE("LOADER", "%s: out of memory (%u bytes)", path.c_str(), size);
			delete m;
			return (s32)SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
		}
		m->memAddr = addr;
		base = addr - elf.TotalStart();
	}
	m->memSize = size;

	if (!elf.Load(base, GuestPtr, m->image, &err)) {
		LOGE("LOADER", "%s: %s", path.c_str(), err.c_str());
		if (BlockAllocator *a = AllocatorForAddress(m->memAddr)) a->Free(m->memAddr);
		delete m;
		return (s32)SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;
	}
	if (m->image.relocErrors) LOGW("LOADER", "%s: %d bad relocations", m->name.c_str(), m->image.relocErrors);

	const loader::PspModuleInfoRaw &mi = m->image.modInfo;
	m->gp = mi.gp;
	loader::ParseImports(mi.libstub, mi.libstubEnd, GuestReadPtr, m->imports, nullptr);
	loader::ParseExports(mi.libent, mi.libentEnd, GuestReadPtr, m->exports, nullptr);
	m->moduleStart = m->image.entry;
	for (const loader::ExportLib &el : m->exports) {
		if (!el.name.empty()) continue;
		for (auto &f : el.funcs) {
			if (f.first == loader::NID_MODULE_START) m->moduleStart = f.second;
			if (f.first == loader::NID_MODULE_STOP) m->moduleStop = f.second;
		}
		for (auto &v : el.vars) {
			if (v.first == loader::NID_MODULE_START_THREAD_PARAMETER && mem::IsValidRange(v.second, 16)) {
				m->hasStartThreadParam = true;
				m->startThreadPrio = GRead32(v.second + 4);
				m->startThreadStack = GRead32(v.second + 8);
				m->startThreadAttr = GRead32(v.second + 12);
			}
		}
	}

	SceUID id = Register(m);
	g_modules.push_back(id);
	ResolveImports(m);
	RebindImportersOf(m);
	if (m->aot) RegisterAotFunctions(m);

	size_t nFuncImports = 0;
	for (auto &l : m->imports) nFuncImports += l.funcs.size();
	LOGI("LOADER", "Loaded %s (%s) at %08x-%08x, entry %08x, gp %08x, %zu import libs (%zu funcs), %zu export libs",
	     m->name.c_str(), path.c_str(), m->memAddr, m->memAddr + m->memSize, m->moduleStart, m->gp, m->imports.size(),
	     nFuncImports, m->exports.size());
	return id;
}

int StartModule(PspModule *m, u32 argSize, u32 argPtr, u32 statusPtr, bool waitForReturn) {
	if (m->status != MODULE_STATUS_LOADED) return (int)SCE_KERNEL_ERROR_ALREADY_STARTED;
	if (!m->moduleStart || m->moduleStart == 0xFFFFFFFF) {
		m->status = MODULE_STATUS_STARTED;
		return 0;
	}
	s32 prio = m->hasStartThreadParam && m->startThreadPrio ? (s32)m->startThreadPrio : 0x20;
	s32 stack = m->hasStartThreadParam && m->startThreadStack ? (s32)m->startThreadStack : 0x40000;
	u32 attr = m->hasStartThreadParam ? m->startThreadAttr : 0;
	if (m->kernelMode) attr |= 0x00001000;
	const char *threadName = waitForReturn ? "SceModmgrStart" : "user_main";
	SceUID tid = CreateThread(threadName, m->moduleStart, prio, stack, attr, m->gp, m->uid);
	if (tid < 0) return tid;
	PspThread *t = Get<PspThread>(tid);
	m->status = MODULE_STATUS_STARTING;
	PspThread *caller = sched::Current();
	if (waitForReturn && caller) t->endWaiters.push_back(caller->uid);
	StartThread(t, argSize, argPtr);
	if (!waitForReturn || !caller) {
		m->status = MODULE_STATUS_STARTED;
		return 0;
	}
	s32 result = sched::Wait(WAITTYPE_THREADEND, tid, 0, false);
	m->status = MODULE_STATUS_STARTED;
	if (mem::IsValidRange(statusPtr, 4)) GWrite32(statusPtr, (u32)result);
	return result;
}

int StopModule(PspModule *m, u32 argSize, u32 argPtr, u32 statusPtr) {
	if (m->status != MODULE_STATUS_STARTED) return (int)SCE_KERNEL_ERROR_ALREADY_STOPPED;
	if (m->moduleStop) {
		SceUID tid = CreateThread("SceModmgrStop", m->moduleStop, 0x20, 0x40000, 0, m->gp, m->uid);
		if (tid >= 0) {
			PspThread *t = Get<PspThread>(tid);
			PspThread *caller = sched::Current();
			if (caller) t->endWaiters.push_back(caller->uid);
			StartThread(t, argSize, argPtr);
			if (caller) {
				s32 r = sched::Wait(WAITTYPE_THREADEND, tid, 0, false);
				if (mem::IsValidRange(statusPtr, 4)) GWrite32(statusPtr, (u32)r);
			}
		}
	}
	m->status = MODULE_STATUS_STOPPED;
	return 0;
}

int UnloadModule(PspModule *m) {
	if (m->status == MODULE_STATUS_STARTED || m->status == MODULE_STATUS_STARTING) return (int)SCE_KERNEL_ERROR_MODULE_CANNOT_REMOVE;
	dispatch::UnregisterRange(m->memAddr, m->memSize);
	if (BlockAllocator *a = AllocatorForAddress(m->memAddr)) a->Free(m->memAddr);
	if (m->dll) FreeLibrary((HMODULE)m->dll);
	g_modules.erase(std::remove(g_modules.begin(), g_modules.end(), m->uid), g_modules.end());
	Destroy(m->uid);
	return 0;
}

PspModule *ModuleByAddress(u32 addr) {
	for (SceUID id : g_modules) {
		PspModule *m = Get<PspModule>(id);
		if (m && addr >= m->memAddr && addr < m->memAddr + m->memSize) return m;
	}
	return nullptr;
}

PspModule *ModuleByName(const std::string &name) {
	for (SceUID id : g_modules) {
		PspModule *m = Get<PspModule>(id);
		if (m && m->name == name) return m;
	}
	return nullptr;
}

}  // namespace kernel
