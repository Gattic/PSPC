// SPDX-License-Identifier: GPL-2.0-or-later
// Symbolized host backtraces for fatal errors and host stack overflows (uses the PDBs next to the
// executable; frames in AOT DLLs are named after the guest function they belong to).
#include "runtime/core/backtrace.h"

#include <windows.h>

#include <dbghelp.h>

#include <cstdio>

#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/core/dispatch.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"

#pragma comment(lib, "dbghelp.lib")

namespace backtrace {

namespace {

void PrintFrames(FILE *out, void *const *frames, USHORT n) {
	HANDLE proc = GetCurrentProcess();
	static bool symInit = false;
	if (!symInit) {
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
		symInit = SymInitialize(proc, nullptr, TRUE) != FALSE;
	}
	alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256];
	auto *sym = (SYMBOL_INFO *)symBuf;
	HMODULE exe = GetModuleHandleA(nullptr);
	for (USHORT i = 0; i < n; i++) {
		DWORD64 addr = (DWORD64)frames[i];
		char modName[MAX_PATH] = "?";
		HMODULE mod = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       (LPCSTR)frames[i], &mod)) {
			char path[MAX_PATH];
			if (GetModuleFileNameA(mod, path, MAX_PATH)) {
				const char *slash = strrchr(path, '\\');
				snprintf(modName, sizeof(modName), "%s", slash ? slash + 1 : path);
			}
		}
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		IMAGEHLP_LINE64 line{};
		line.SizeOfStruct = sizeof(line);
		DWORD lineDisp = 0;
		if (mod && mod != exe && strstr(modName, ".dll") && strlen(modName) == 44) {
			// <sha1>.dll: an AOT module without symbols.
			size_t off = 0;
			u32 guest = dispatch::GuestFunctionAt(frames[i], &off);
			fprintf(out, "  #%-2u %s guest fn %08x +0x%zx\n", i, modName, guest, off);
		} else if (symInit && SymFromAddr(proc, addr, &disp, sym)) {
			if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line))
				fprintf(out, "  #%-2u %s!%s+0x%llx (%s:%lu)\n", i, modName, sym->Name, (unsigned long long)disp,
				        line.FileName, line.LineNumber);
			else
				fprintf(out, "  #%-2u %s!%s+0x%llx\n", i, modName, sym->Name, (unsigned long long)disp);
		} else {
			fprintf(out, "  #%-2u %s!%p\n", i, modName, frames[i]);
		}
	}
	if (kernel::PspThread *t = sched::Current())
		fprintf(out, "  guest thread %s (uid %08x), pc=%08x ra=%08x sp=%08x\n", t->name.c_str(), t->uid, t->ctx.pc,
		        t->ctx.r[31], t->ctx.r[29]);
	fflush(out);
}

// A stack overflow leaves no room to symbolize: the handler grabs the frames and finishes the
// report on this fiber's stack.
void *g_crashFiber = nullptr;
void *g_crashFrames[62];
USHORT g_crashCount = 0;

void WINAPI CrashFiberProc(void *) {
	fprintf(stderr, "[FATAL] Host stack overflow (unbounded guest recursion through native calls?)\n");
	fprintf(stderr, "  innermost frames:\n");
	PrintFrames(stderr, g_crashFrames, g_crashCount);
	logging::Shutdown();
	TerminateProcess(GetCurrentProcess(), 0xC00000FD);
}

LONG CALLBACK CrashVeh(EXCEPTION_POINTERS *ep) {
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_ILLEGAL_INSTRUCTION) {
		// Host faults that are always fatal here (guest faults never reach the host this way).
		if (IsDebuggerPresent()) return EXCEPTION_CONTINUE_SEARCH;
		HMODULE mod = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &mod);
		fprintf(stderr, "[FATAL] Host exception %08lx at %p (module offset 0x%llx)\n", code,
		        ep->ExceptionRecord->ExceptionAddress,
		        (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)mod));
		Print(stderr);
		logging::Shutdown();
		TerminateProcess(GetCurrentProcess(), code);
	}
	if (code != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
	g_crashCount = RtlCaptureStackBackTrace(0, 62, g_crashFrames, nullptr);
	if (g_crashFiber && IsThreadAFiber()) SwitchToFiber(g_crashFiber);
	return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void Print(FILE *out) {
	void *frames[48];
	USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
	PrintFrames(out, frames, n);
}

void InstallFatalHook() {
	logging::SetFatalHook([]() { Print(stderr); });
	if (!g_crashFiber) {
		g_crashFiber = CreateFiber(256 * 1024, CrashFiberProc, nullptr);
		AddVectoredExceptionHandler(1, CrashVeh);
	}
}

}  // namespace backtrace
