// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/hle/hle.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/core/dispatch.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/scheduler.h"

namespace hle {

namespace {

struct Library {
	std::string name;
	std::unordered_map<u32, HleFunction> funcs;
};

struct SyscallEntry {
	std::string library;
	u32 nid = 0;
	const HleFunction *func = nullptr;  // nullptr => unimplemented
	std::string name;
	u64 calls = 0;
	bool warned = false;
};

std::map<std::string, Library> g_libs;
std::deque<SyscallEntry> g_syscalls;  // index = syscall code (deque: stable references)
std::map<std::pair<std::string, u32>, u32> g_codeByImport;
std::unordered_map<u64, std::string> g_nidNames;  // (libhash<<32|nid) and (0<<32|nid)
const SyscallEntry *g_current = nullptr;
u32 g_currentCallerPc = 0;

u64 LibKey(const std::string &lib, u32 nid) {
	u32 h = 2166136261u;
	for (char ch : lib) h = (h ^ (u8)ch) * 16777619u;
	return ((u64)h << 32) | nid;
}

u32 CodeFor(const std::string &library, u32 nid) {
	auto key = std::make_pair(library, nid);
	auto it = g_codeByImport.find(key);
	if (it != g_codeByImport.end()) return it->second;
	SyscallEntry e;
	e.library = library;
	e.nid = nid;
	e.func = FindFunction(library, nid);
	e.name = e.func ? e.func->name : NidName(library, nid);
	u32 code = (u32)g_syscalls.size();
	if (code >= cpu::SYSCALL_MAGIC_RETURN) Fatal("Out of syscall codes");
	g_syscalls.push_back(e);
	g_codeByImport[key] = code;
	return code;
}

void StubThunk(Ctx *c) {
	// The stub's second word is "syscall CODE"; run it as if executed in the jr delay slot.
	u32 stub = c->pc;
	u32 w = GRead32(stub + 4);
	Syscall(c, w, stub + 4);
}

}  // namespace

void Init() {
	g_syscalls.clear();
	g_codeByImport.clear();
	g_syscalls.push_back(SyscallEntry{"(reserved)", 0, nullptr, "(reserved)"});
}

void RegisterModule(const char *library, const HleFunction *funcs, size_t count) {
	Library &lib = g_libs[library];
	lib.name = library;
	for (size_t i = 0; i < count; i++) {
		if (lib.funcs.count(funcs[i].nid)) LOGW("HLE", "Duplicate NID %08x in %s", funcs[i].nid, library);
		lib.funcs[funcs[i].nid] = funcs[i];
	}
}

const HleFunction *FindFunction(const std::string &library, u32 nid) {
	auto it = g_libs.find(library);
	if (it == g_libs.end()) return nullptr;
	auto f = it->second.funcs.find(nid);
	if (f == it->second.funcs.end()) return nullptr;
	return &f->second;
}

bool IsLibraryKnown(const std::string &library) { return g_libs.count(library) != 0; }

u32 GetSyscallCode(const std::string &library, u32 nid) { return CodeFor(library, nid); }

void PatchImportStub(u32 stubAddr, const std::string &library, u32 nid) {
	u32 code = CodeFor(library, nid);
	GWrite32(stubAddr, 0x03E00008);               // jr ra
	GWrite32(stubAddr + 4, (code << 6) | 0x0C);   // syscall code
	dispatch::Register(stubAddr, &StubThunk);
}

void PatchImportStubToGuest(u32 stubAddr, const std::string &library, u32 nid, u32 target) {
	GWrite32(stubAddr, 0x08000000 | ((target >> 2) & 0x03FFFFFF));  // j target
	GWrite32(stubAddr + 4, 0);
	if (HostFn fn = dispatch::Lookup(target)) {
		dispatch::Register(stubAddr, fn);
	} else {
		dispatch::UnregisterRange(stubAddr, 8);
	}
	(void)library;
	(void)nid;
}

void Syscall(Ctx *c, u32 insn, u32 pc) {
	u32 code = (insn >> 6) & 0xFFFFF;
	if (code == cpu::SYSCALL_MAGIC_RETURN) {
		Fatal("Executed the host-call return stub (pc=%08x ra=%08x): guest code returned past its caller", pc,
		      c->r[31]);
	}
	if (code >= g_syscalls.size()) {
		LOGE("HLE", "Unknown syscall code %05x at %08x", code, pc);
		c->r[2] = 0x80020001;
		return;
	}
	SyscallEntry &e = g_syscalls[code];
	e.calls++;
	const SyscallEntry *prev = g_current;
	u32 prevPc = g_currentCallerPc;
	g_current = &e;
	g_currentCallerPc = c->r[31] - 8;
	if (e.func && e.func->fn) {
		if (logging::g_level <= LogLevel::Trace) {
			LOGT("HLE", "%s(%08x, %08x, %08x, %08x, %08x, %08x) from %08x", e.name.c_str(), c->r[4], c->r[5], c->r[6],
			     c->r[7], c->r[8], c->r[9], c->r[31]);
		}
		e.func->fn(c);
		if (logging::g_level <= LogLevel::Trace) LOGT("HLE", "  %s -> %08x", e.name.c_str(), c->r[2]);
	} else {
		if (!e.warned) {
			e.warned = true;
			LOGW("HLE", "Unimplemented %s::%s (NID %08x) called from %08x (a0=%08x a1=%08x a2=%08x)", e.library.c_str(),
			     e.name.c_str(), e.nid, c->r[31] - 8, c->r[4], c->r[5], c->r[6]);
		}
		c->r[2] = 0;
	}
	g_current = prev;
	g_currentCallerPc = prevPc;
}

void LoadNidDatabase(const std::filesystem::path &csv) {
	std::ifstream in(csv);
	if (!in) return;
	std::string line;
	bool first = true;
	size_t n = 0;
	while (std::getline(in, line)) {
		if (first) {
			first = false;
			if (line.rfind("library", 0) == 0) continue;
		}
		std::stringstream ss(line);
		std::string lib, nidStr, name;
		if (!std::getline(ss, lib, ',') || !std::getline(ss, nidStr, ',') || !std::getline(ss, name, ',')) continue;
		u32 nid = (u32)strtoul(nidStr.c_str(), nullptr, 16);
		g_nidNames[LibKey(lib, nid)] = name;
		g_nidNames.emplace((u64)nid, name);
		n++;
	}
	LOGI("HLE", "Loaded %zu NID names from %s", n, csv.string().c_str());
}

std::string NidName(const std::string &library, u32 nid) {
	auto it = g_nidNames.find(LibKey(library, nid));
	if (it != g_nidNames.end()) return it->second;
	it = g_nidNames.find((u64)nid);
	if (it != g_nidNames.end()) return it->second;
	char buf[32];
	snprintf(buf, sizeof(buf), "unknown_%08x", nid);
	return buf;
}

const char *CurrentFunctionName() { return g_current ? g_current->name.c_str() : "(none)"; }
u32 CurrentCallerPc() { return g_currentCallerPc; }

void SetReturnU64(Ctx *c, u64 v) {
	c->r[2] = (u32)v;
	c->r[3] = (u32)(v >> 32);
}

void DumpUnimplementedStats() {
	std::vector<const SyscallEntry *> v;
	for (const auto &e : g_syscalls)
		if (!(e.func && e.func->fn) && e.calls) v.push_back(&e);
	std::sort(v.begin(), v.end(), [](auto a, auto b) { return a->calls > b->calls; });
	for (auto e : v) LOGI("HLE", "unimplemented: %8llu x %s::%s (%08x)", (unsigned long long)e->calls, e->library.c_str(),
	                      e->name.c_str(), e->nid);
}

}  // namespace hle
