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

// Where unlinked imports go: lui v0, 0x8002; jr ra; ori v0, v0, 0x013A.
u32 NotLinkedStub() {
	static u32 stub = 0;
	if (!stub || GRead32(stub) != 0x3C028002) {
		stub = AllocKernelMemory(16, "unlinked import");
		GWrite32(stub, 0x3C028002);
		GWrite32(stub + 4, 0x03E00008);
		GWrite32(stub + 8, 0x3442013A);
		GWrite32(stub + 12, 0);
	}
	return stub;
}

// Points the references of a variable import at the exported variable (once per import).
void BindVariable(PspModule *m, const loader::ImportLib &lib, const loader::ImportVar &v, u32 exportAddr) {
	if (!m->boundVars.insert(lib.name + ":" + std::to_string(v.nid)).second) return;
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
	// Native code reads these words at run time; they aren't code changes.
	if (!m->pristine.empty()) {
		for (const loader::VarRef &ref : v.refs) {
			if (ref.addr >= m->memAddr && ref.addr + 4 <= m->memAddr + m->memSize) {
				u32 w = GRead32(ref.addr);
				memcpy(&m->pristine[ref.addr - m->memAddr], &w, 4);
			}
		}
	}
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
			} else if (!hle::FindFunction(lib.name, f.nid) && hle::NidName(lib.name, f.nid).rfind("unknown_", 0) == 0) {
				// No firmware exports this: the loader leaves it unlinked, and calling it returns
				// LIBRARY_NOT_YET_LINKED (modules/unresolved).
				LOGW("LOADER", "%s: unresolved import %s::%08x", m->name.c_str(), lib.name.c_str(), f.nid);
				hle::PatchImportStubToGuest(f.stubAddr, lib.name, f.nid, NotLinkedStub());
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
				LOGW("LOADER", "%s: variable import %s::%08x: no loaded module exports it (yet)", m->name.c_str(), lib.name.c_str(), v.nid);
				continue;
			}
			BindVariable(m, lib, v, exportAddr);
		}
	}
}

// A new module exports libraries: rebind earlier modules that imported them but got HLE stubs
// with no implementation, and bind their variable imports from it (the XMB's plugins load
// before the libraries whose variables they use, e.g. music_main_plugin before music_browser).
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
				for (const loader::ImportVar &v : lib.vars) {
					for (auto &ev : el.vars)
						if (ev.first == v.nid) BindVariable(m, lib, v, ev.second);
				}
			}
		}
	}
}

bool TryLoadAot(PspModule *m, const loader::ElfFile &elf, u32 *requiredBase) {
	if (!g_aotEnabled || g_aotDir.empty()) return false;
	// A DLL is one process-wide instance with one relocation delta: a second copy of a module
	// that is already running natively (a game loading the same PRX twice) is interpreted.
	for (SceUID id : g_modules) {
		PspModule *other = Get<PspModule>(id);
		if (other && other->aot && other->sha1 == m->sha1) {
			LOGI("AOT", "%s is already loaded natively; this copy runs in the interpreter", m->name.c_str());
			return false;
		}
	}
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
	m->aotEntries.clear();
	for (u32 i = 0; i < desc->numFunctions; i++) {
		u32 addr = desc->recompileBase + delta + desc->functions[i].offset;
		dispatch::RegisterNative(addr, desc->functions[i].fn, desc->functions[i].body);
		m->aotEntries.push_back(addr);
	}
	std::sort(m->aotEntries.begin(), m->aotEntries.end());
	m->aotPatched = desc->patched;
	// What the native code was compiled from (relocated, imports bound): later writes to the code
	// show up as differences against this.
	if (const u8 *p = mem::GetPointer(m->memAddr, m->memSize)) m->pristine.assign(p, p + m->memSize);
	LOGI("AOT", "%s: %u native functions (delta %08x)", m->name.c_str(), desc->numFunctions, delta);
}

}  // namespace

void SetAotCacheDir(const std::string &dir) { g_aotDir = dir; }
void SetAotEnabled(bool enabled) { g_aotEnabled = enabled; }
void SetDecryptTool(const std::string &exe) { g_decryptTool = exe; }
bool DecryptWithTool(const std::vector<u8> &in, std::vector<u8> &out, std::string *err) { return RunDecryptTool(in, out, err); }
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
			if (v.first == loader::NID_MODULE_STOP_THREAD_PARAMETER && mem::IsValidRange(v.second, 16)) {
				m->stopThreadPrio = GRead32(v.second + 4);
				m->stopThreadStack = GRead32(v.second + 8);
				m->stopThreadAttr = GRead32(v.second + 12);
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

// SceKernelSMOption: size, mpidstack, stacksize, priority, attribute.
constexpr u32 kSMOptionSize = 20;
constexpr u32 kSMOptionAttrMask = 0x00F06000;

int ValidateSMOption(u32 opt) {
	if (!opt || !mem::IsValidRange(opt, kSMOptionSize)) return 0;
	if ((CompiledSdkVersion() & 0xFFFF0000) > 0x0207FFFF && GRead32(opt) != kSMOptionSize)
		return (int)SCE_KERNEL_ERROR_ILLEGAL_SIZE;
	if ((GRead32(opt + 16) & ~kSMOptionAttrMask) != 0) return (int)SCE_KERNEL_ERROR_ERROR;
	return 0;
}

namespace {

struct ModuleThreadParams {
	s32 priority;
	s32 stackSize;
	u32 attr;
	BlockAllocator *stackAlloc;  // nullptr: where the attr puts it
};

// A module_start/module_stop thread's parameters: a nonzero option field wins, then the module's
// thread parameter export, then the defaults (stack 0x40000 for a user module, 0x1000 for a
// kernel one). The option's attribute bits add to the module's; its stack partition must be one
// the module could use itself. Follows PPSSPP's ResolveModuleThreadParams.
int ResolveModuleThreadParams(PspModule *m, u32 prio, u32 stack, u32 attr, u32 opt, ModuleThreadParams &p) {
	p.priority = prio ? (s32)prio : 0x20;
	p.stackSize = stack ? (s32)stack : (m->kernelMode ? 0x1000 : 0x40000);
	p.attr = attr;
	p.stackAlloc = nullptr;
	if (opt && mem::IsValidRange(opt, kSMOptionSize)) {
		u32 mpid = GRead32(opt + 4), ostack = GRead32(opt + 8), oprio = GRead32(opt + 12), oattr = GRead32(opt + 16);
		if (oprio) p.priority = (s32)oprio;
		if (ostack) p.stackSize = (s32)ostack;
		p.attr |= oattr & kSMOptionAttrMask;
		switch (mpid) {
		case 0: break;
		case 1:
		case 3:
		case 4:
			if (!m->kernelMode) return (int)SCE_KERNEL_ERROR_PARTITION_MISMATCH;
			p.stackAlloc = &g_kernelMem;
			break;
		case 2:
		case 5:
		case 6:
			if (m->kernelMode) return (int)SCE_KERNEL_ERROR_PARTITION_MISMATCH;
			p.stackAlloc = mpid == 5 ? &g_volatileMem : &g_userMem;
			break;
		default: return (int)SCE_KERNEL_ERROR_PARTITION_MISMATCH;
		}
	}
	// The top four bits (user mode and its level) are cleared; threadman sets them.
	p.attr &= 0x0FFFFFFF;
	return 0;
}

// Runs a module function in a new thread, which is deleted when it returns. With a waiting
// caller, blocks until then and returns its exit status in *exitStatus.
void (*g_moduleStartHook)(SceUID) = nullptr;

int RunModuleThread(PspModule *m, u32 entry, const ModuleThreadParams &p, u32 argSize, u32 argPtr, bool wait,
                    s32 *exitStatus, std::function<void()> onFirstRun = nullptr) {
	SceUID tid = CreateThread(m->name.c_str(), entry, p.priority, p.stackSize, p.attr, m->gp, m->uid, m->kernelMode,
	                          nullptr, p.stackAlloc);
	if (tid < 0) return tid;
	PspThread *t = Get<PspThread>(tid);
	t->deleteOnReturn = true;
	t->onFirstRun = std::move(onFirstRun);
	PspThread *caller = sched::Current();
	if (wait && caller) t->endWaiters.push_back(caller->uid);
	StartThread(t, argSize, argPtr);
	if (wait && caller) {
		s32 r = sched::Wait(WAITTYPE_THREADEND, tid, 0, false);
		if (exitStatus) *exitStatus = r;
	}
	return 0;
}

}  // namespace

int StartModule(PspModule *m, u32 argSize, u32 argPtr, u32 statusPtr, bool waitForReturn, u32 smOptionPtr) {
	if (m->status != MODULE_STATUS_LOADED) return (int)SCE_KERNEL_ERROR_ERROR;
	if (!m->moduleStart || m->moduleStart == 0xFFFFFFFF) {
		if (g_moduleStartHook && sched::Current()) g_moduleStartHook(m->uid);
		m->status = MODULE_STATUS_STARTED;
		return 0;
	}
	ModuleThreadParams p;
	int r = ResolveModuleThreadParams(m, m->startThreadPrio, m->startThreadStack, m->startThreadAttr, smOptionPtr, p);
	if (r < 0) return r;
	m->status = MODULE_STATUS_STARTING;
	s32 status = 0;
	// The start hook runs on the start thread just before module_start: by then the plugins
	// started earlier (their threads were queued first) have installed their handlers, as on M33
	// where they load before the game.
	SceUID uid = m->uid;
	r = RunModuleThread(m, m->moduleStart, p, argSize, argPtr, waitForReturn, &status, [uid]() {
		if (g_moduleStartHook) g_moduleStartHook(uid);
	});
	if (r < 0) {
		m->status = MODULE_STATUS_LOADED;
		return r;
	}
	m->status = MODULE_STATUS_STARTED;
	if (waitForReturn && mem::IsValidRange(statusPtr, 4)) GWrite32(statusPtr, (u32)status);
	return 0;
}

int StopModule(PspModule *m, u32 argSize, u32 argPtr, u32 statusPtr, u32 smOptionPtr) {
	if (m->status != MODULE_STATUS_STARTED) return (int)SCE_KERNEL_ERROR_ALREADY_STOPPED;
	if (m->moduleStop && (m->moduleStop & 3) == 0 && mem::IsValidAddress(m->moduleStop)) {
		ModuleThreadParams p;
		int r = ResolveModuleThreadParams(m, m->stopThreadPrio, m->stopThreadStack, m->stopThreadAttr, smOptionPtr, p);
		if (r < 0) return r;
		m->status = MODULE_STATUS_STOPPING;
		s32 status = 0;
		r = RunModuleThread(m, m->moduleStop, p, argSize, argPtr, true, &status);
		if (r < 0) {
			m->status = MODULE_STATUS_STARTED;
			return r;
		}
		if (mem::IsValidRange(statusPtr, 4)) GWrite32(statusPtr, (u32)status);
	}
	m->status = MODULE_STATUS_STOPPED;
	return 0;
}

int UnloadModule(PspModule *m) {
	if (m->status == MODULE_STATUS_STARTING || m->status == MODULE_STATUS_STOPPING) return (int)SCE_KERNEL_ERROR_MODULE_CANNOT_REMOVE;
	dispatch::UnregisterRange(m->memAddr, m->memSize);
	if (BlockAllocator *a = AllocatorForAddress(m->memAddr)) a->Free(m->memAddr);
	if (m->dll) FreeLibrary((HMODULE)m->dll);
	g_modules.erase(std::remove(g_modules.begin(), g_modules.end(), m->uid), g_modules.end());
	Destroy(m->uid);
	return 0;
}

void SetModuleStartHook(void (*hook)(SceUID module)) { g_moduleStartHook = hook; }

int InvalidateNativeCode(u32 addr, u32 size) {
	int dropped = 0;
	for (SceUID id : g_modules) {
		PspModule *m = Get<PspModule>(id);
		if (!m || !m->aot || m->pristine.empty()) continue;
		u32 lo = std::max(addr & ~3u, m->memAddr), hi = std::min(addr + size, m->memAddr + m->memSize);
		if (lo >= hi) continue;
		const u8 *cur = mem::GetPointer(m->memAddr, m->memSize);
		if (!cur) continue;
		auto inCode = [&](u32 a) {
			for (const auto &r : m->image.codeRanges)
				if (a >= r.first && a < r.second) return true;
			return false;
		};
		for (u32 a = lo; a + 4 <= hi; a += 4) {
			if (!inCode(a)) continue;  // data changes all the time; only code matters
			u32 off = a - m->memAddr;
			if (memcmp(cur + off, &m->pristine[off], 4) == 0) continue;
			// The function holding the word is the nearest entry at or below it (bodies that
			// reach past the next entry are rare; their callers still see the change there).
			auto it = std::upper_bound(m->aotEntries.begin(), m->aotEntries.end(), a);
			if (it != m->aotEntries.begin()) {
				u32 entry = *std::prev(it);
				size_t idx = (size_t)(std::prev(it) - m->aotEntries.begin());
				if (m->aotPatched && !m->aotPatched[idx]) {
					m->aotPatched[idx] = 1;  // direct calls from other native code check this
					dispatch::Register(entry, nullptr);
					dropped++;
					LOGI("AOT", "%s: code at %08x changed; %08x now runs in the interpreter", m->name.c_str(), a,
					     entry);
				}
			}
			memcpy(&m->pristine[off], cur + off, 4);
		}
	}
	return dropped;
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
