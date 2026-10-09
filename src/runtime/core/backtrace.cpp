// SPDX-License-Identifier: GPL-2.0-or-later
// Symbolized host backtraces for fatal errors (uses the PDBs next to the executable and the AOT
// DLLs, when present).
#include "runtime/core/backtrace.h"

#include <windows.h>

#include <dbghelp.h>

#include <cstdio>

#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"

#pragma comment(lib, "dbghelp.lib")

namespace backtrace {

void Print(FILE *out) {
	HANDLE proc = GetCurrentProcess();
	static bool symInit = false;
	if (!symInit) {
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
		symInit = SymInitialize(proc, nullptr, TRUE) != FALSE;
	}
	void *frames[48];
	USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
	alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256];
	auto *sym = (SYMBOL_INFO *)symBuf;
	for (USHORT i = 0; i < n; i++) {
		DWORD64 addr = (DWORD64)frames[i];
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		IMAGEHLP_LINE64 line{};
		line.SizeOfStruct = sizeof(line);
		DWORD lineDisp = 0;
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
		if (symInit && SymFromAddr(proc, addr, &disp, sym)) {
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

void InstallFatalHook() {
	logging::SetFatalHook([]() { Print(stderr); });
}

}  // namespace backtrace
