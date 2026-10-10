// SPDX-License-Identifier: GPL-2.0-or-later
//
// Kernel libraries that kernel-mode code (M33 plugins: cheat devices, screenshot and remote-play
// plugins) links against and the HLE kernel otherwise has no use for:
//   SysclibForKernel  the kernel's C library (string, memory, ctype, printf, 64-bit division)
//   StdioForKernel    printf/puts to the kernel's stdout (pspc's log)
//   LoadCoreForKernel module lookups returning the firmware's module descriptor (SceModule2)
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/m33/m33.h"

namespace hle {

// printf over guest varargs, as the firmware's prnt formats them: integer arguments from register
// `firstReg` on ($a0 = 4 ... $t3 = 11), then words on the stack; 64-bit values ("ll", doubles) in
// an even-aligned register pair or 8-byte aligned stack slot.
std::string FormatGuest(const Ctx *c, u32 fmtAddr, int firstReg) {
	std::string out;
	const char *fmt = mem::GetCString(fmtAddr);
	if (!fmt) return out;
	int reg = firstReg, stack = 0;
	auto word = [&]() -> u32 {
		if (reg <= 11) return c->r[reg++];
		return detail::StackArgWord(c, stack++);
	};
	auto dword = [&]() -> u64 {
		if (reg <= 10) {
			if (reg & 1) reg++;  // pairs start at an even register ($a0, $a2, $t0, $t2)
			if (reg <= 10) {
				u64 v = c->r[reg] | ((u64)c->r[reg + 1] << 32);
				reg += 2;
				return v;
			}
		}
		reg = 12;
		if (stack & 1) stack++;
		u64 v = detail::StackArgWord(c, stack) | ((u64)detail::StackArgWord(c, stack + 1) << 32);
		stack += 2;
		return v;
	};
	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			out.push_back(*p);
			continue;
		}
		p++;
		if (*p == '%') {
			out.push_back('%');
			continue;
		}
		std::string spec = "%";
		while (*p && strchr("-+ #0", *p)) spec.push_back(*p++);
		if (*p == '*') {
			spec += std::to_string((s32)word());
			p++;
		} else {
			while (*p >= '0' && *p <= '9') spec.push_back(*p++);
		}
		if (*p == '.') {
			spec.push_back(*p++);
			if (*p == '*') {
				spec += std::to_string((s32)word());
				p++;
			} else {
				while (*p >= '0' && *p <= '9') spec.push_back(*p++);
			}
		}
		int longs = 0;
		while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'L' || *p == 'q') {
			if (*p == 'l' || *p == 'L') longs++;
			if (*p == 'q') longs = 2;
			p++;
		}
		if (!*p) break;
		const char conv = *p;
		char buf[512];
		buf[0] = 0;
		switch (conv) {
		case 'd':
		case 'i':
			if (longs >= 2) snprintf(buf, sizeof(buf), (spec + "lld").c_str(), (long long)dword());
			else snprintf(buf, sizeof(buf), (spec + "d").c_str(), (int)word());
			break;
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			if (longs >= 2) snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)dword());
			else snprintf(buf, sizeof(buf), (spec + conv).c_str(), (unsigned)word());
			break;
		case 'p':
			snprintf(buf, sizeof(buf), (spec + "08X").c_str(), (unsigned)word());
			break;
		case 'c':
			snprintf(buf, sizeof(buf), (spec + "c").c_str(), (int)(u8)word());
			break;
		case 's': {
			const char *s = mem::GetCString(word());
			snprintf(buf, sizeof(buf), (spec + "s").c_str(), s ? s : "(null)");
			break;
		}
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G': {
			u64 bits = dword();
			double d;
			memcpy(&d, &bits, 8);
			snprintf(buf, sizeof(buf), (spec + conv).c_str(), d);
			break;
		}
		default:
			buf[0] = conv;
			buf[1] = 0;
			break;
		}
		out += buf;
	}
	return out;
}

}  // namespace hle

namespace {

// ---- SysclibForKernel ----

// Guest strings: bounded so a bad pointer can't run away.
std::string GStr(u32 a, u32 max = 0x100000) {
	const char *s = mem::GetCString(a, max);
	return s ? std::string(s) : std::string();
}
u32 GStrLen(u32 a) {
	u32 n = 0;
	while (mem::IsValidRange(a + n, 1) && GRead8(a + n)) n++;
	return n;
}

u32 sys_memset(u32 dst, int v, u32 n) {
	if (n && mem::IsValidRange(dst, n)) mem::Memset(dst, (u8)v, n);
	return dst;
}
u32 sys_memcpy(u32 dst, u32 src, u32 n) {
	if (n && mem::IsValidRange(dst, n) && mem::IsValidRange(src, n)) memmove(GPtr(dst), GPtr(src), n);
	return dst;
}
void sys_bcopy(u32 src, u32 dst, u32 n) { sys_memcpy(dst, src, n); }
void sys_bzero(u32 dst, u32 n) { sys_memset(dst, 0, n); }
int sys_memcmp(u32 a, u32 b, u32 n) {
	if (!n || !mem::IsValidRange(a, n) || !mem::IsValidRange(b, n)) return 0;
	const int r = memcmp(GPtr(a), GPtr(b), n);
	return r < 0 ? -1 : r > 0 ? 1 : 0;
}
u32 sys_memchr(u32 s, int ch, u32 n) {
	for (u32 i = 0; i < n && mem::IsValidRange(s + i, 1); i++)
		if (GRead8(s + i) == (u8)ch) return s + i;
	return 0;
}
u32 sys_strlen(u32 s) { return GStrLen(s); }
u32 sys_strnlen(u32 s, u32 max) {
	u32 n = 0;
	while (n < max && mem::IsValidRange(s + n, 1) && GRead8(s + n)) n++;
	return n;
}
u32 sys_strcpy(u32 dst, u32 src) {
	const u32 n = GStrLen(src) + 1;
	return sys_memcpy(dst, src, n);
}
u32 sys_strncpy(u32 dst, u32 src, u32 n) {
	u32 i = 0;
	for (; i < n && mem::IsValidRange(src + i, 1) && mem::IsValidRange(dst + i, 1); i++) {
		const u8 ch = GRead8(src + i);
		GWrite8(dst + i, ch);
		if (!ch) break;
	}
	for (; i < n && mem::IsValidRange(dst + i, 1); i++) GWrite8(dst + i, 0);
	return dst;
}
u32 sys_strcat(u32 dst, u32 src) {
	sys_strcpy(dst + GStrLen(dst), src);
	return dst;
}
u32 sys_strncat(u32 dst, u32 src, u32 n) {
	u32 d = dst + GStrLen(dst), i = 0;
	for (; i < n && mem::IsValidRange(src + i, 1) && GRead8(src + i); i++) GWrite8(d + i, GRead8(src + i));
	if (mem::IsValidRange(d + i, 1)) GWrite8(d + i, 0);
	return dst;
}
int Sign(int r) { return r < 0 ? -1 : r > 0 ? 1 : 0; }
int sys_strcmp(u32 a, u32 b) { return Sign(strcmp(GStr(a).c_str(), GStr(b).c_str())); }
int sys_strncmp(u32 a, u32 b, u32 n) { return Sign(strncmp(GStr(a, n + 1).c_str(), GStr(b, n + 1).c_str(), n)); }
u32 sys_strchr(u32 s, int ch) {
	for (u32 i = 0; mem::IsValidRange(s + i, 1); i++) {
		const u8 c = GRead8(s + i);
		if (c == (u8)ch) return s + i;
		if (!c) return 0;
	}
	return 0;
}
u32 sys_strrchr(u32 s, int ch) {
	u32 found = 0;
	for (u32 i = 0; mem::IsValidRange(s + i, 1); i++) {
		const u8 c = GRead8(s + i);
		if (c == (u8)ch) found = s + i;
		if (!c) break;
	}
	return found;
}
u32 sys_strstr(u32 hay, u32 needle) {
	const std::string h = GStr(hay), n = GStr(needle);
	const size_t at = h.find(n);
	return at == std::string::npos ? 0 : hay + (u32)at;
}
u32 sys_strpbrk(u32 s, u32 set) {
	const std::string h = GStr(s), k = GStr(set);
	const size_t at = h.find_first_of(k);
	return at == std::string::npos ? 0 : s + (u32)at;
}
u32 sys_strspn(u32 s, u32 set) {
	const std::string h = GStr(s), k = GStr(set);
	const size_t at = h.find_first_not_of(k);
	return at == std::string::npos ? (u32)h.size() : (u32)at;
}
u32 sys_strcspn(u32 s, u32 set) {
	const std::string h = GStr(s), k = GStr(set);
	const size_t at = h.find_first_of(k);
	return at == std::string::npos ? (u32)h.size() : (u32)at;
}
int sys_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int sys_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

u32 StrToNum(u32 s, u32 endPtr, int base, bool isSigned) {
	const std::string str = GStr(s);
	char *end = nullptr;
	u32 v = isSigned ? (u32)strtol(str.c_str(), &end, base) : (u32)strtoul(str.c_str(), &end, base);
	if (endPtr && mem::IsValidRange(endPtr, 4)) GWrite32(endPtr, s + (u32)(end - str.c_str()));
	return v;
}
u32 sys_strtol(u32 s, u32 endPtr, int base) { return StrToNum(s, endPtr, base, true); }
u32 sys_strtoul(u32 s, u32 endPtr, int base) { return StrToNum(s, endPtr, base, false); }
// atob(str, &value): the firmware's number parser (decimal, 0x hex); returns the end.
u32 sys_atob(u32 s, u32 valuePtr) {
	const std::string str = GStr(s);
	char *end = nullptr;
	const u32 v = (u32)strtoul(str.c_str(), &end, 0);
	if (mem::IsValidRange(valuePtr, 4)) GWrite32(valuePtr, v);
	return s + (u32)(end - str.c_str());
}

// strtok keeps its position in a word of kernel memory per caller-visible state: the classic
// static pointer (strtok) or the caller's (strtok_r).
u32 g_strtokSave = 0;
u32 StrTok(u32 s, u32 delim, u32 *save) {
	u32 p = s ? s : *save;
	if (!p) return 0;
	const std::string d = GStr(delim);
	while (mem::IsValidRange(p, 1) && GRead8(p) && d.find((char)GRead8(p)) != std::string::npos) p++;
	if (!mem::IsValidRange(p, 1) || !GRead8(p)) {
		*save = 0;
		return 0;
	}
	const u32 tok = p;
	while (mem::IsValidRange(p, 1) && GRead8(p) && d.find((char)GRead8(p)) == std::string::npos) p++;
	if (mem::IsValidRange(p, 1) && GRead8(p)) {
		GWrite8(p, 0);
		*save = p + 1;
	} else {
		*save = 0;
	}
	return tok;
}
u32 sys_strtok(u32 s, u32 delim) { return StrTok(s, delim, &g_strtokSave); }
u32 sys_strtok_r(u32 s, u32 delim, u32 savePtr) {
	if (!mem::IsValidRange(savePtr, 4)) return 0;
	u32 save = GRead32(savePtr);
	const u32 r = StrTok(s, delim, &save);
	GWrite32(savePtr, save);
	return r;
}

// The ctype table (look_ctype_table/get_ctype_table): newlib's _ctype_ flags, offset by one so
// index -1 (EOF) works, in kernel memory on first use.
u32 g_ctypeTable = 0;
u32 CtypeTable() {
	if (g_ctypeTable) return g_ctypeTable;
	const u32 a = kernel::AllocKernelMemory(0x104, "ctype");
	if (!a) return 0;
	mem::Memset(a, 0, 0x104);
	for (int ch = 0; ch < 128; ch++) {
		u8 f = 0;
		if (isupper(ch)) f |= 0x01;
		if (islower(ch)) f |= 0x02;
		if (isdigit(ch)) f |= 0x04;
		if (isspace(ch)) f |= 0x08;
		if (ispunct(ch)) f |= 0x10;
		if (iscntrl(ch)) f |= 0x20;
		if (isxdigit(ch)) f |= 0x40;
		if (ch == ' ') f |= 0x80;
		GWrite8(a + 1 + ch, f);
	}
	g_ctypeTable = a;
	return a;
}
u32 sys_get_ctype_table() { return CtypeTable() + 1; }
int sys_look_ctype_table(int ch) {
	const u32 t = CtypeTable();
	return t && ch >= -1 && ch < 256 ? GRead8(t + 1 + ch) : 0;
}

void sys_sprintf(Ctx *c) {
	const std::string s = hle::FormatGuest(c, c->r[5], 6);
	const u32 dst = c->r[4];
	if (mem::IsValidRange(dst, (u32)s.size() + 1)) mem::WriteBytes(dst, s.c_str(), (u32)s.size() + 1);
	c->r[2] = (u32)s.size();
}
void sys_snprintf(Ctx *c) {
	const std::string s = hle::FormatGuest(c, c->r[6], 7);
	const u32 dst = c->r[4], size = c->r[5];
	if (size && mem::IsValidRange(dst, size)) {
		const u32 n = std::min<u32>((u32)s.size(), size - 1);
		mem::WriteBytes(dst, s.data(), n);
		GWrite8(dst + n, 0);
	}
	c->r[2] = (u32)s.size();
}

// 64-bit division helpers (libgcc): operands in register pairs, the result in $v0:$v1.
void Set64(Ctx *c, u64 v) {
	c->r[2] = (u32)v;
	c->r[3] = (u32)(v >> 32);
}
u64 Arg64(const Ctx *c, int reg) { return c->r[reg] | ((u64)c->r[reg + 1] << 32); }
void sys_udivdi3(Ctx *c) {
	const u64 d = Arg64(c, 6);
	Set64(c, d ? Arg64(c, 4) / d : 0);
}
void sys_umoddi3(Ctx *c) {
	const u64 d = Arg64(c, 6);
	Set64(c, d ? Arg64(c, 4) % d : 0);
}
// __udivmoddi4(n, d, &rem)
void sys_udivmoddi4(Ctx *c) {
	const u64 n = Arg64(c, 4), d = Arg64(c, 6);
	const u32 remPtr = c->r[8];
	const u64 q = d ? n / d : 0, r = d ? n % d : n;
	if (remPtr && mem::IsValidRange(remPtr, 8)) {
		GWrite32(remPtr, (u32)r);
		GWrite32(remPtr + 4, (u32)(r >> 32));
	}
	Set64(c, q);
}

// ---- StdioForKernel ----

void Out(const std::string &s) {
	std::string t = s;
	while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
	if (!t.empty()) LOGI("KPRINTF", "%s", t.c_str());
}
void stdio_printf(Ctx *c) {
	const std::string s = hle::FormatGuest(c, c->r[4], 5);
	Out(s);
	c->r[2] = (u32)s.size();
}
int stdio_puts(u32 s) {
	Out(GStr(s));
	return 0;
}
int stdio_putchar(int ch) { return ch & 0xFF; }
int stdio_stdout() { return 1; }
int stdio_stderr() { return 2; }
int stdio_stdin() { return 0; }

// ---- LoadCoreForKernel ----

u32 lc_FindModuleByName(const char *name) {
	kernel::PspModule *m = name ? kernel::ModuleByName(name) : nullptr;
	return m ? m33::ModuleDescriptor(m->uid) : 0;
}
u32 lc_FindModuleByUID(SceUID uid) { return m33::ModuleDescriptor(uid); }
u32 lc_FindModuleByAddress(u32 addr) {
	kernel::PspModule *m = kernel::ModuleByAddress(addr);
	return m ? m33::ModuleDescriptor(m->uid) : 0;
}
u32 lc_GetModuleGPByAddress(u32 addr) {
	kernel::PspModule *m = kernel::ModuleByAddress(addr);
	return m ? m->gp : 0;
}
int lc_GetModuleIdListForKernel(u32 listPtr, u32 maxCount, u32 countPtr) {
	const std::vector<SceUID> &mods = kernel::LoadedModules();
	u32 n = 0;
	for (SceUID id : mods) {
		if (n < maxCount && mem::IsValidRange(listPtr + 4 * n, 4)) GWrite32(listPtr + 4 * n, (u32)id);
		n++;
	}
	if (countPtr && mem::IsValidRange(countPtr, 4)) GWrite32(countPtr, n);
	return 0;
}
int lc_ModuleCount() { return (int)kernel::LoadedModules().size(); }
void lc_IcacheClearAll() { kernel::InvalidateNativeCode(0x08000000, 0x02000000); }

}  // namespace

void Register_KernelClib() {
	static const hle::HleFunction sysclib[] = {
		HLEFN(0x10F3BB61, sys_memset),
		HLEFN(0xAB7592FF, sys_memcpy),
		HLEFN(0xA48D2592, sys_memcpy),  // memmove
		HLEFN(0x097049BD, sys_bcopy),
		HLEFN(0x86FEFCE9, sys_bzero),
		HLEFN(0x81D0D1F7, sys_memcmp),
		HLEFN(0x7F8A6F23, sys_memcmp),  // bcmp
		HLEFN(0x68A78817, sys_memchr),
		HLEFN(0x52DF196C, sys_strlen),
		HLEFN(0x90C5573D, sys_strnlen),
		HLEFN(0xEC6F1CF2, sys_strcpy),
		HLEFN(0xB49A7697, sys_strncpy),
		HLEFN(0x476FD94A, sys_strcat),
		HLEFN(0xD3D1A3B9, sys_strncat),
		HLEFN(0xC0AB8932, sys_strcmp),
		HLEFN(0x7AB35214, sys_strncmp),
		HLEFN(0xB1DC2AE8, sys_strchr),
		HLEFN(0xD1CD40E5, sys_strchr),  // index
		HLEFN(0x4C0E0274, sys_strrchr),
		HLEFN(0x243665ED, sys_strrchr),  // rindex
		HLEFN(0x0D188658, sys_strstr),
		HLEFN(0x0DFB7B6C, sys_strpbrk),
		HLEFN(0x62AE052F, sys_strspn),
		HLEFN(0x89B79CB1, sys_strcspn),
		HLEFN(0x87F8D2DA, sys_strtok),
		HLEFN(0x1AB53A58, sys_strtok_r),
		HLEFN(0x47DD934D, sys_strtol),
		HLEFN(0x6A7900E1, sys_strtoul),
		HLEFN(0x1D83F344, sys_atob),
		HLEFN(0xCE2F7487, sys_toupper),
		HLEFN(0x3EC5BBF6, sys_tolower),
		HLEFN(0xD887CACD, sys_get_ctype_table),
		HLEFN(0x32C767F2, sys_look_ctype_table),
		HLERAW(0x7661E728, sys_sprintf),
		HLERAW(0xC2145E80, sys_snprintf),
		HLERAW(0x7DEE14DE, sys_udivdi3),
		HLERAW(0x5E8E5F42, sys_umoddi3),
		HLERAW(0xDF17F4A2, sys_udivmoddi4),
	};
	hle::RegisterModule("SysclibForKernel", sysclib);

	static const hle::HleFunction stdio[] = {
		HLERAW(0xCAB439DF, stdio_printf),
		HLEFN(0xD97C8CB9, stdio_puts),
		HLEFN(0xD768752A, stdio_putchar),
		HLEFN(0xA6BAB2E9, stdio_stdout),
		HLEFN(0xF78BA90A, stdio_stderr),
		HLEFN(0x172D316E, stdio_stdin),
	};
	hle::RegisterModule("StdioForKernel", stdio);

	static const hle::HleFunction loadcore[] = {
		HLEFN(0xCF8A41B1, lc_FindModuleByName),
		HLEFN(0xF6B1BF0F, lc_FindModuleByName),
		HLEFN(0xCCE4A157, lc_FindModuleByUID),
		HLEFN(0x40972E6E, lc_FindModuleByUID),
		HLEFN(0xF32A2940, lc_FindModuleByUID),  // sceKernelModuleFromUID
		HLEFN(0xCD26E0CA, lc_FindModuleByUID),  // sceKernelGetModuleFromUID
		HLEFN(0xFB8AE27D, lc_FindModuleByAddress),
		HLEFN(0xBC99C625, lc_FindModuleByAddress),
		HLEFN(0x410084F9, lc_GetModuleGPByAddress),
		HLEFN(0x05D915DB, lc_GetModuleIdListForKernel),
		HLEFN(0x37E6F41B, lc_GetModuleIdListForKernel),
		HLEFN(0x82CE54ED, lc_ModuleCount),
		HLEFN(0xD8779AC6, lc_IcacheClearAll),
		HLESTUB(0x2952F5AC, "sceKernelDcacheWBinvAll"),
	};
	hle::RegisterModule("LoadCoreForKernel", loadcore);
}
