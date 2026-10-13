// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest physical memory on Windows. All of [0, 0x20000000) is reserved as placeholders; the
// scratchpad, VRAM (+3 mirrors) and RAM are views of one page-file section, so mirrors alias.
// A fault anywhere else in the window maps a shared zero "junk" granule there and continues,
// which mirrors PPSSPP's default of ignoring bad accesses (and logs the guest address).
#include "runtime/core/memory.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#endif

#include <atomic>
#include <mutex>

#include "common/log.h"
#include "runtime/core/cpu.h"

u8 *g_membase = nullptr;

namespace mem {

bool g_edramMirrors = false;

namespace {

// Ported from PPSSPP (GPU/Common/DepthSwizzle.h), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later. With a 32-bit color buffer, address bits 5 to log2(T) - 1 rotate left by one
// (bits 9 and 10 swap at T 0), then (T << 3) | 0x40 is XORed in (0x600 at T 0); bits 0-4 never
// change.
struct EdramLayout {
	u32 rotMask = 0;
	int rotShift = 0;
	u32 xorBits = 0;
	u32 Stored(u32 offset) const {
		const u32 f = offset & rotMask;
		return ((offset & ~rotMask) | (((f << 1) | (f >> rotShift)) & rotMask)) ^ xorBits;
	}
};

EdramLayout LayoutFor(u32 translation, bool color32) {
	EdramLayout layout;
	if (translation == 0) {
		layout.xorBits = 0x600;
		if (color32) {
			layout.rotMask = 0x600;
			layout.rotShift = 1;
		}
		return layout;
	}
	layout.xorBits = (translation << 3) | 0x40;
	int top = 0;
	while ((2u << top) <= translation)
		top++;
	if (color32 && top - 5 > 1) {
		layout.rotMask = ((1u << (top - 5)) - 1) << 5;
		layout.rotShift = top - 5 - 1;
	}
	return layout;
}

EdramLayout g_layout16 = LayoutFor(0x400, false), g_layout32 = LayoutFor(0x400, true);

}  // namespace

void SetEdramTranslation(u32 translation) {
	g_layout16 = LayoutFor(translation, false);
	g_layout32 = LayoutFor(translation, true);
}

u32 EdramMirrorAddress(u32 addr) {
	if (!InSwizzledEdramMirror(addr)) return addr;
	const EdramLayout &layout = (addr & 0x00400000) ? g_layout32 : g_layout16;
	return (addr & ~0x003FFFFFu) | (layout.Stored(addr & 0x001FFFFF) & 0x001FFFFF);
}

}  // namespace mem

namespace mem {

u32 g_ramSize = 0;

#ifdef _WIN32
namespace {

constexpr size_t WINDOW_SIZE = 0x20000000;
constexpr size_t GRANULE = 0x10000;

typedef PVOID(WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID(WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                       MEM_EXTENDED_PARAMETER *, ULONG);

VirtualAlloc2Fn pVirtualAlloc2 = nullptr;
MapViewOfFile3Fn pMapViewOfFile3 = nullptr;
HANDLE g_section = nullptr;
HANDLE g_junkSection = nullptr;
bool g_placeholders = false;
std::mutex g_faultMutex;
std::atomic<u32> g_badAccesses{0};
PVOID g_vehHandle = nullptr;

// Physical layout inside the section.
constexpr u64 SEC_SCRATCH = 0;
constexpr u64 SEC_VRAM = GRANULE;
constexpr u64 SEC_RAM = SEC_VRAM + VRAM_SIZE;

bool SplitPlaceholder(size_t off, size_t size) {
	// Carve [off, off+size) out of whatever placeholder contains it.
	return VirtualFree(g_membase + off, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
}

bool MapView(HANDLE section, size_t off, u64 secOff, size_t size) {
	// Splitting fails when [off, off+size) already is a whole placeholder; mapping still works then.
	SplitPlaceholder(off, size);
	void *p = pMapViewOfFile3(section, GetCurrentProcess(), g_membase + off, secOff, size, MEM_REPLACE_PLACEHOLDER,
	                          PAGE_READWRITE, nullptr, 0);
	return p == g_membase + off;
}

LONG CALLBACK FaultHandler(EXCEPTION_POINTERS *ep) {
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
	uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
	uintptr_t base = (uintptr_t)g_membase;
	if (!g_membase || addr < base || addr >= base + WINDOW_SIZE) return EXCEPTION_CONTINUE_SEARCH;
	u32 guest = (u32)(addr - base);
	bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
	u32 n = ++g_badAccesses;
	if (n <= 64) {
		Ctx *c = cpu::CurrentCtx();
		LOGW("MEM", "Bad %s at guest %08x (pc~%08x ra=%08x)%s", write ? "write" : "read", guest, c ? c->pc : 0,
		     c ? c->r[31] : 0, n == 64 ? " - further bad accesses not logged" : "");
	}
	if (cpu::StrictMemory()) {
		Fatal("Bad guest memory access at %08x (strict mode)", guest);
	}
	std::lock_guard<std::mutex> lock(g_faultMutex);
	size_t off = (size_t)guest & ~(GRANULE - 1);
	if (g_placeholders) {
		if (!MapView(g_junkSection, off, 0, GRANULE)) {
			// Another thread may have mapped it already.
			MEMORY_BASIC_INFORMATION mbi;
			if (VirtualQuery(g_membase + off, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
				return EXCEPTION_CONTINUE_EXECUTION;
			return EXCEPTION_CONTINUE_SEARCH;
		}
	} else {
		if (!VirtualAlloc(g_membase + off, GRANULE, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

bool Init(u32 ramSize) {
	g_ramSize = ramSize;
	HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
	if (kb) {
		pVirtualAlloc2 = (VirtualAlloc2Fn)GetProcAddress(kb, "VirtualAlloc2");
		pMapViewOfFile3 = (MapViewOfFile3Fn)GetProcAddress(kb, "MapViewOfFile3");
	}
	const u64 secSize = SEC_RAM + ramSize;
	if (pVirtualAlloc2 && pMapViewOfFile3) {
		g_section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)(secSize >> 32),
		                               (DWORD)secSize, nullptr);
		g_junkSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, GRANULE, nullptr);
		void *base = pVirtualAlloc2(GetCurrentProcess(), nullptr, WINDOW_SIZE, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
		                            PAGE_NOACCESS, nullptr, 0);
		if (g_section && g_junkSection && base) {
			g_membase = (u8 *)base;
			bool ok = MapView(g_section, SCRATCHPAD_BASE, SEC_SCRATCH, GRANULE);
			for (u32 m = 0; m < 4 && ok; m++) ok = MapView(g_section, VRAM_BASE + m * VRAM_SIZE, SEC_VRAM, VRAM_SIZE);
			ok = ok && MapView(g_section, RAM_BASE, SEC_RAM, ramSize);
			if (ok) {
				g_placeholders = true;
			} else {
				LOGW("MEM", "Placeholder mapping failed (%lu), falling back to plain allocation", GetLastError());
				// Leak the partially mapped window; fall back below.
				g_membase = nullptr;
			}
		}
	}
	if (!g_membase) {
		void *base = VirtualAlloc(nullptr, WINDOW_SIZE, MEM_RESERVE, PAGE_NOACCESS);
		if (!base) return false;
		g_membase = (u8 *)base;
		VirtualAlloc(g_membase + SCRATCHPAD_BASE, GRANULE, MEM_COMMIT, PAGE_READWRITE);
		VirtualAlloc(g_membase + VRAM_BASE, VRAM_SIZE * 4, MEM_COMMIT, PAGE_READWRITE);
		VirtualAlloc(g_membase + RAM_BASE, ramSize, MEM_COMMIT, PAGE_READWRITE);
	}
	g_vehHandle = AddVectoredExceptionHandler(1, FaultHandler);
	LOGI("MEM", "Guest memory at %p (RAM %u MB, %s)", g_membase, ramSize >> 20,
	     g_placeholders ? "aliased mirrors" : "no mirrors");
	return true;
}

void Shutdown() {
	if (g_vehHandle) RemoveVectoredExceptionHandler(g_vehHandle);
	g_vehHandle = nullptr;
	// The window is released with the process.
}

#else
namespace {
constexpr size_t WINDOW_SIZE = 0x20000000;
constexpr size_t GRANULE = 0x10000;
constexpr size_t SEC_VRAM = GRANULE;
constexpr size_t SEC_RAM = SEC_VRAM + VRAM_SIZE;
int g_section = -1, g_junkSection = -1;
u8 *g_privateVram = nullptr;
std::atomic<u32> g_badAccesses{0};
struct sigaction g_previousFault{};
bool g_faultInstalled = false;

void FaultHandler(int signal, siginfo_t *info, void *context) {
	uintptr_t addr = (uintptr_t)info->si_addr;
	uintptr_t base = (uintptr_t)g_membase;
	if (g_membase && addr >= base && addr < base + WINDOW_SIZE) {
		g_badAccesses.fetch_add(1, std::memory_order_relaxed);
		if (!cpu::StrictMemory()) {
			size_t off = (addr - base) & ~(GRANULE - 1);
			// No allocation, locks or logging in the signal handler. Linux mmap replaces this
			// reserved granule atomically even if another renderer thread faults there too.
			if (mmap(g_membase + off, GRANULE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
			         g_junkSection, 0) != MAP_FAILED) return;
		}
		const char message[] = "PSPC: invalid guest memory access (strict mode or mapping failure)\n";
		(void)!write(STDERR_FILENO, message, sizeof(message) - 1);
	}
	if (g_previousFault.sa_flags & SA_SIGINFO) {
		if (g_previousFault.sa_sigaction) { g_previousFault.sa_sigaction(signal, info, context); return; }
	} else if (g_previousFault.sa_handler != SIG_DFL && g_previousFault.sa_handler != SIG_IGN) {
		g_previousFault.sa_handler(signal); return;
	}
	sigaction(signal, &g_previousFault, nullptr);
	raise(signal);
}
}
bool Init(u32 ramSize) {
	g_ramSize = ramSize;
	g_badAccesses = 0;
	g_section = memfd_create("pspc-guest", MFD_CLOEXEC);
	g_junkSection = memfd_create("pspc-junk", MFD_CLOEXEC);
	if (g_section < 0 || g_junkSection < 0 || ftruncate(g_section, SEC_RAM + ramSize) ||
	    ftruncate(g_junkSection, GRANULE)) { Shutdown(); return false; }
	void *base = mmap(nullptr, WINDOW_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) { Shutdown(); return false; }
	g_membase = (u8 *)base;
	auto map = [](size_t addr, size_t offset, size_t size) {
		return mmap(g_membase + addr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_section, offset) != MAP_FAILED;
	};
	bool ok = map(SCRATCHPAD_BASE, 0, GRANULE) && map(RAM_BASE, SEC_RAM, ramSize);
	for (size_t i = 0; i < 4 && ok; ++i) ok = map(VRAM_BASE + i * VRAM_SIZE, SEC_VRAM, VRAM_SIZE);
	if (!ok) { Shutdown(); return false; }
	struct sigaction action{};
	action.sa_sigaction = FaultHandler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, &g_previousFault)) { Shutdown(); return false; }
	g_faultInstalled = true;
	LOGI("MEM", "Guest memory at %p (RAM %u MB, aliased mirrors)", g_membase, ramSize >> 20);
	return true;
}
void Shutdown() {
	if (g_faultInstalled) sigaction(SIGSEGV, &g_previousFault, nullptr);
	g_faultInstalled = false;
	if (g_privateVram) munmap(g_privateVram, VRAM_SIZE);
	g_privateVram = nullptr;
	if (g_membase) munmap(g_membase, WINDOW_SIZE);
	g_membase = nullptr;
	if (g_section >= 0) close(g_section);
	if (g_junkSection >= 0) close(g_junkSection);
	g_section = g_junkSection = -1;
}
#endif

bool IsValidAddress(u32 addr) {
	u32 p = addr & GUEST_ADDR_MASK;
	if (p >= RAM_BASE && p < RAM_BASE + g_ramSize) return true;
	if (p >= VRAM_BASE && p < VRAM_BASE + VRAM_SIZE * 4) return true;
	if (p >= SCRATCHPAD_BASE && p < SCRATCHPAD_BASE + SCRATCHPAD_SIZE) return true;
	return false;
}

bool IsValidRange(u32 addr, u32 size) {
	if (size == 0) return IsValidAddress(addr);
	u32 p = addr & GUEST_ADDR_MASK;
	u64 end = (u64)p + size;
	if (p >= RAM_BASE && end <= (u64)RAM_BASE + g_ramSize) return true;
	if (p >= VRAM_BASE && end <= (u64)VRAM_BASE + VRAM_SIZE * 4) return true;
	if (p >= SCRATCHPAD_BASE && end <= (u64)SCRATCHPAD_BASE + SCRATCHPAD_SIZE) return true;
	return false;
}

u8 *GetPointer(u32 addr, u32 size) { return IsValidRange(addr, size) ? GPtr(addr) : nullptr; }

const char *GetCString(u32 addr, u32 maxLen) {
	if (!IsValidAddress(addr)) return nullptr;
	const char *s = (const char *)GPtr(addr);
	for (u32 i = 0; i < maxLen; i++) {
		if (!IsValidAddress(addr + i)) return nullptr;
		if (s[i] == 0) return s;
	}
	return nullptr;
}

std::string ReadString(u32 addr, u32 maxLen) {
	std::string out;
	for (u32 i = 0; i < maxLen; i++) {
		if (!IsValidAddress(addr + i)) break;
		char ch = (char)GRead8(addr + i);
		if (!ch) break;
		out.push_back(ch);
	}
	return out;
}

void Memset(u32 addr, u8 value, u32 size) {
	if (IsValidRange(addr, size)) memset(GPtr(addr), value, size);
}
void Memcpy(u32 dst, u32 src, u32 size) {
	if (IsValidRange(dst, size) && IsValidRange(src, size)) memmove(GPtr(dst), GPtr(src), size);
}
void WriteBytes(u32 dst, const void *src, u32 size) {
	if (IsValidRange(dst, size)) memcpy(GPtr(dst), src, size);
}
void ReadBytes(void *dst, u32 src, u32 size) {
	if (IsValidRange(src, size)) memcpy(dst, GPtr(src), size);
	else memset(dst, 0, size);
}

u32 BadAccessCount() { return g_badAccesses.load(); }

u8 *PrivateVramView() {
#ifdef _WIN32
	static u8 *view = nullptr;
	if (!view && g_placeholders && g_section)
		view = (u8 *)MapViewOfFile(g_section, FILE_MAP_ALL_ACCESS, (DWORD)(SEC_VRAM >> 32), (DWORD)SEC_VRAM, VRAM_SIZE);
	return view;
#else
	if (!g_privateVram && g_section >= 0) {
		void *p = mmap(nullptr, VRAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_section, SEC_VRAM);
		if (p != MAP_FAILED) g_privateVram = (u8 *)p;
	}
	return g_privateVram;
#endif
}

}  // namespace mem
