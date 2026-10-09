// SPDX-License-Identifier: GPL-2.0-or-later
//
// VFPU tests: decoding, register layout, semantics with and without prefixes, disassembly, and
// replays of hardware-recorded pspautotests results (cpu/vfpu exact, prefix_consume, vrnd) when
// reference/pspautotests is present.
#include <xmmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "allegrex/decode.h"
#include "allegrex/sem.h"
#include "allegrex/sem_tables.h"
#include "allegrex/vfpu_exec.h"
#include "testing.h"

using ag::Op;

namespace {

// ---- Encoding helpers ----
constexpr u32 SZ_S = 0, SZ_P = 0x80, SZ_T = 0x8000, SZ_Q = 0x8080;
constexpr u32 VD(u32 r) { return r; }
constexpr u32 VS(u32 r) { return r << 8; }
constexpr u32 VT(u32 r) { return r << 16; }
constexpr u32 RS(u32 r) { return r << 21; }
constexpr u32 RT(u32 r) { return r << 16; }

// Register numbers: <m><c><r> for column vectors / singles.
constexpr u32 Reg(u32 m, u32 c, u32 r) { return (m << 2) | c | (r << 5); }

constexpr u32 OP_VADD = 0x60000000, OP_VSUB = 0x60800000, OP_VSBN = 0x61000000, OP_VDIV = 0x63800000;
constexpr u32 OP_VMUL = 0x64000000, OP_VDOT = 0x64800000, OP_VSCL = 0x65000000, OP_VHDP = 0x66000000;
constexpr u32 OP_VCRS = 0x66800000, OP_VDET = 0x67000000;
constexpr u32 OP_VCMP = 0x6C000000, OP_VMIN = 0x6D000000, OP_VMAX = 0x6D800000, OP_VSCMP = 0x6E800000;
constexpr u32 OP_VSGE = 0x6F000000, OP_VSLT = 0x6F800000;
constexpr u32 OP_V4 = 0xD0000000;  // | sub << 16
constexpr u32 OP_V7 = 0xD0200000;
constexpr u32 OP_V9 = 0xD0400000;
constexpr u32 OP_VCST = 0xD0600000, OP_VF2IN = 0xD2000000, OP_VF2IZ = 0xD2200000, OP_VF2IU = 0xD2400000;
constexpr u32 OP_VF2ID = 0xD2600000, OP_VI2F = 0xD2800000, OP_VCMOVT = 0xD2A00000, OP_VCMOVF = 0xD2A80000;
constexpr u32 OP_VWBN = 0xD3000000;
constexpr u32 OP_VPFXS = 0xDC000000, OP_VPFXT = 0xDD000000, OP_VPFXD = 0xDE000000;
constexpr u32 OP_VIIM = 0xDF000000, OP_VFIM = 0xDF800000;
constexpr u32 OP_VMMUL = 0xF0000000, OP_VTFM2 = 0xF0800000, OP_VTFM3 = 0xF1000000, OP_VTFM4 = 0xF1800000;
constexpr u32 OP_VMSCL = 0xF2000000, OP_VCRSP = 0xF2800000, OP_VMMOV = 0xF3800000, OP_VMIDT = 0xF3830000;
constexpr u32 OP_VMZERO = 0xF3860000, OP_VMONE = 0xF3870000, OP_VROT = 0xF3A00000;
constexpr u32 OP_LVS = 0xC8000000, OP_LVLQ = 0xD4000000, OP_LVRQ = 0xD4000002, OP_LVQ = 0xD8000000;
constexpr u32 OP_SVS = 0xE8000000, OP_SVLQ = 0xF4000000, OP_SVRQ = 0xF4000002, OP_SVQ = 0xF8000000;
constexpr u32 OP_MFV = 0x48600000, OP_MTV = 0x48E00000, OP_BVF = 0x49000000, OP_BVT = 0x49010000;
constexpr u32 OP_BVFL = 0x49020000, OP_BVTL = 0x49030000;

constexpr u32 V4(u32 sub) { return OP_V4 | (sub << 16); }
constexpr u32 V7(u32 sub) { return OP_V7 | (sub << 16); }
constexpr u32 V9(u32 sub) { return OP_V9 | (sub << 16); }

u32 Bits(float f) { return std::bit_cast<u32>(f); }
float Flt(u32 u) { return std::bit_cast<float>(u); }

// A thread-initialized context with the register file zeroed (InitCtx fills it with NaN, which
// the tests below don't want).
Ctx NewCtx() {
	Ctx c;
	memset(&c, 0, sizeof(c));
	vfpu::InitCtx(&c);
	memset(c.v, 0, sizeof(c.v));
	return c;
}

// Runs one instruction through the interpreter tables (decode + sem/cond tables).
bool Exec(Ctx *c, u32 w) {
	const Op op = ag::Decode(w);
	if (op == Op::INVALID) {
		printf("  invalid instruction %08x\n", w);
		g_testFailures++;
		return false;
	}
	if (ag::g_semTable[(size_t)op]) {
		ag::g_semTable[(size_t)op](c, w);
		return true;
	}
	if (ag::g_condTable[(size_t)op]) return ag::g_condTable[(size_t)op](c, w);
	printf("  no semantics for %08x\n", w);
	g_testFailures++;
	return false;
}

void SetVec(Ctx *c, u32 reg, int n, const float *vals) {
	for (int i = 0; i < n; i++) c->v[vfpu::VecElem(reg, n, i)] = vals[i];
}
void SetVecBits(Ctx *c, u32 reg, int n, const u32 *vals) {
	for (int i = 0; i < n; i++) c->vi[vfpu::VecElem(reg, n, i)] = vals[i];
}
float GetV(const Ctx *c, u32 reg, int n, int i) { return c->v[vfpu::VecElem(reg, n, i)]; }
u32 GetVI(const Ctx *c, u32 reg, int n, int i) { return c->vi[vfpu::VecElem(reg, n, i)]; }

// Locates a file under the repository by walking up from the working directory.
std::filesystem::path FindRef(const char *rel) {
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path p = fs::current_path(ec);
	for (int i = 0; i < 8 && !p.empty(); i++) {
		fs::path cand = p / rel;
		if (fs::exists(cand, ec)) return cand;
		if (p.parent_path() == p) break;
		p = p.parent_path();
	}
	return {};
}

std::vector<std::string> ReadLines(const std::filesystem::path &path) {
	std::vector<std::string> lines;
	std::ifstream f(path, std::ios::binary);
	std::string line;
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		lines.push_back(line);
	}
	return lines;
}

// Compares generated lines with an .expected file, reporting the first few differences.
void CompareLines(const char *what, const std::vector<std::string> &got, const std::vector<std::string> &want) {
	int bad = 0;
	const size_t n = std::max(got.size(), want.size());
	for (size_t i = 0; i < n; i++) {
		const std::string g = i < got.size() ? got[i] : "<missing>";
		const std::string w = i < want.size() ? want[i] : "<missing>";
		if (g != w) {
			if (bad < 12) printf("  %s line %zu:\n    got:  %s\n    want: %s\n", what, i + 1, g.c_str(), w.c_str());
			bad++;
		}
	}
	if (bad) {
		printf("  %s: %d of %zu lines differ\n", what, bad, n);
		g_testFailures++;
	}
}

std::string Fmt(const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

constexpr bool IsVfpuPrimary(u32 opc) {
	switch (opc) {
	case 18: case 24: case 25: case 27: case 50: case 52: case 53: case 54: case 55: case 58: case 60: case 61:
	case 62: case 63: return true;
	default: return false;
	}
}

// ---- Canonical encodings: every VFPU op decodes from at least one ----
struct Canon {
	Op op;
	u32 w;
};

const Canon kCanon[] = {
	{Op::MFV, OP_MFV | RT(8) | Reg(1, 2, 3)},
	{Op::MFVC, OP_MFV | RT(8) | 0x83},
	{Op::MTV, OP_MTV | RT(8) | Reg(1, 2, 3)},
	{Op::MTVC, OP_MTV | RT(8) | 0x80},
	{Op::BVF, OP_BVF | (5 << 18) | 0x10},
	{Op::BVT, OP_BVT | 0x10},
	{Op::BVFL, OP_BVFL | 0x10},
	{Op::BVTL, OP_BVTL | 0x10},
	{Op::LV_S, OP_LVS | RS(4) | RT(1) | 2 | 0x10},
	{Op::SV_S, OP_SVS | RS(4) | RT(1) | 0x10},
	{Op::LV_Q, OP_LVQ | RS(4) | RT(1) | 0x10},
	{Op::SV_Q, OP_SVQ | RS(4) | RT(1) | 2},
	{Op::LVL_Q, OP_LVLQ | RS(4)},
	{Op::LVR_Q, OP_LVRQ | RS(4)},
	{Op::SVL_Q, OP_SVLQ | RS(4)},
	{Op::SVR_Q, OP_SVRQ | RS(4)},
	{Op::VADD, OP_VADD | SZ_Q | VD(0) | VS(4) | VT(8)},
	{Op::VSUB, OP_VSUB | SZ_T},
	{Op::VSBN, OP_VSBN},
	{Op::VDIV, OP_VDIV | SZ_P},
	{Op::VMUL, OP_VMUL | SZ_Q},
	{Op::VDOT, OP_VDOT | SZ_Q},
	{Op::VSCL, OP_VSCL | SZ_Q},
	{Op::VHDP, OP_VHDP | SZ_Q},
	{Op::VCRS, OP_VCRS | SZ_T},
	{Op::VDET, OP_VDET | SZ_P},
	{Op::VCMP, OP_VCMP | SZ_Q | 2},
	{Op::VMIN, OP_VMIN | SZ_Q},
	{Op::VMAX, OP_VMAX | SZ_Q},
	{Op::VSCMP, OP_VSCMP | SZ_Q},
	{Op::VSGE, OP_VSGE | SZ_Q},
	{Op::VSLT, OP_VSLT | SZ_Q},
	{Op::VMOV, V4(0) | SZ_Q},
	{Op::VABS, V4(1) | SZ_Q},
	{Op::VNEG, V4(2) | SZ_Q},
	{Op::VIDT, V4(3) | SZ_Q},
	{Op::VSAT0, V4(4) | SZ_Q},
	{Op::VSAT1, V4(5) | SZ_Q},
	{Op::VZERO, V4(6) | SZ_Q},
	{Op::VONE, V4(7) | SZ_Q},
	{Op::VRCP, V4(16) | SZ_Q},
	{Op::VRSQ, V4(17) | SZ_Q},
	{Op::VSIN, V4(18) | SZ_Q},
	{Op::VCOS, V4(19) | SZ_Q},
	{Op::VEXP2, V4(20) | SZ_Q},
	{Op::VLOG2, V4(21) | SZ_Q},
	{Op::VSQRT, V4(22) | SZ_Q},
	{Op::VASIN, V4(23) | SZ_Q},
	{Op::VNRCP, V4(24) | SZ_Q},
	{Op::VNSIN, V4(26) | SZ_Q},
	{Op::VREXP2, V4(28) | SZ_Q},
	{Op::VRNDS, V7(0) | VS(5)},
	{Op::VRNDI, V7(1) | SZ_Q},
	{Op::VRNDF1, V7(2) | SZ_Q},
	{Op::VRNDF2, V7(3) | SZ_Q},
	{Op::VF2H, V7(18) | SZ_Q},
	{Op::VH2F, V7(19) | SZ_P},
	{Op::VSBZ, V7(22)},
	{Op::VLGB, V7(23)},
	{Op::VUC2I, V7(24)},
	{Op::VC2I, V7(25)},
	{Op::VUS2I, V7(26) | SZ_P},
	{Op::VS2I, V7(27) | SZ_P},
	{Op::VI2UC, V7(28) | SZ_Q},
	{Op::VI2C, V7(29) | SZ_Q},
	{Op::VI2US, V7(30) | SZ_Q},
	{Op::VI2S, V7(31) | SZ_Q},
	{Op::VSRT1, V9(0) | SZ_Q},
	{Op::VSRT2, V9(1) | SZ_Q},
	{Op::VBFY1, V9(2) | SZ_Q},
	{Op::VBFY2, V9(3) | SZ_Q},
	{Op::VOCP, V9(4) | SZ_Q},
	{Op::VSOCP, V9(5) | SZ_P},
	{Op::VFAD, V9(6) | SZ_Q},
	{Op::VAVG, V9(7) | SZ_Q},
	{Op::VSRT3, V9(8) | SZ_Q},
	{Op::VSRT4, V9(9) | SZ_Q},
	{Op::VSGN, V9(10) | SZ_Q},
	{Op::VMFVC, V9(16) | VS(3)},
	{Op::VMTVC, V9(17) | 3},
	{Op::VT4444, V9(25) | SZ_Q},
	{Op::VT5551, V9(26) | SZ_Q},
	{Op::VT5650, V9(27) | SZ_Q},
	{Op::VCST, OP_VCST | (9 << 16) | SZ_Q},
	{Op::VF2IN, OP_VF2IN | SZ_Q},
	{Op::VF2IZ, OP_VF2IZ | SZ_Q},
	{Op::VF2IU, OP_VF2IU | SZ_Q},
	{Op::VF2ID, OP_VF2ID | SZ_Q},
	{Op::VI2F, OP_VI2F | SZ_Q},
	{Op::VCMOVT, OP_VCMOVT | SZ_Q},
	{Op::VCMOVF, OP_VCMOVF | SZ_Q},
	{Op::VWBN, OP_VWBN | (10 << 16)},
	{Op::VPFXS, OP_VPFXS | 0xE4},
	{Op::VPFXT, OP_VPFXT | 0xE4},
	{Op::VPFXD, OP_VPFXD},
	{Op::VIIM, OP_VIIM | 5},
	{Op::VFIM, OP_VFIM | 0x3C00},
	{Op::VMMUL, OP_VMMUL | SZ_Q | VS(0x24) | VT(4)},
	{Op::VTFM, OP_VTFM4 | SZ_Q},
	{Op::VHTFM, OP_VTFM4 | SZ_T},
	{Op::VMSCL, OP_VMSCL | SZ_Q},
	{Op::VCRSP, OP_VCRSP | SZ_T},
	{Op::VQMUL, OP_VCRSP | SZ_Q},
	{Op::VMMOV, OP_VMMOV | SZ_Q},
	{Op::VMIDT, OP_VMIDT | SZ_Q},
	{Op::VMZERO, OP_VMZERO | SZ_Q},
	{Op::VMONE, OP_VMONE | SZ_Q},
	{Op::VROT, OP_VROT | (1 << 16) | SZ_Q},
	{Op::VNOP, 0xFFFF0000},
	{Op::VSYNC, 0xFFFF0320},
	{Op::VFLUSH, 0xFFFF040D},
	{Op::VNOPX, 0xFC000000},
};

}  // namespace

// ---------------------------------------------------------------------------------------------

TEST(vfpu_decode_roundtrip) {
	int counts[(size_t)Op::COUNT] = {};
	for (const Canon &e : kCanon) {
		const Op got = ag::Decode(e.w);
		if (got != e.op) {
			printf("  %08x decodes as %s, expected %s\n", e.w, ag::OpName(got), ag::OpName(e.op));
			g_testFailures++;
		}
		CHECK(IsVfpuPrimary(e.w >> 26));
		counts[(size_t)e.op]++;
	}
	// Every VFPU op (MFV .. the last before INVALID) has exactly one canonical entry and a unique name.
	std::map<std::string, Op> names;
	for (size_t i = (size_t)Op::MFV; i < (size_t)Op::INVALID; i++) {
		if (counts[i] != 1) {
			printf("  op %s has %d canonical encodings\n", ag::OpName((Op)i), counts[i]);
			g_testFailures++;
		}
		CHECK(names.emplace(ag::OpName((Op)i), (Op)i).second);
	}
	CHECK_EQ(ArraySize(kCanon), (size_t)Op::INVALID - (size_t)Op::MFV);
	// Kinds: the branches are conditional (likely) branches, everything else straight-line.
	CHECK(ag::OpKind(Op::BVF) == ag::Kind::BR);
	CHECK(ag::OpKind(Op::BVT) == ag::Kind::BR);
	CHECK(ag::OpKind(Op::BVFL) == ag::Kind::BRL);
	CHECK(ag::OpKind(Op::BVTL) == ag::Kind::BRL);
	CHECK(ag::OpKind(Op::VMMUL) == ag::Kind::ALU);
	CHECK(ag::g_semTable[(size_t)Op::VMMUL] != nullptr);
	CHECK(ag::g_condTable[(size_t)Op::BVT] != nullptr);
}

TEST(vfpu_decode_invalid) {
	const u32 invalid[] = {
		0x48000000,  // mfc2
		0x48400000,  // cfc2
		0x48800000,  // mtc2
		0x48C00000,  // ctc2
		0x49200000,  // cop2 rs=9
		0x61800000,  // VFPU0 subop 3
		0x65800000,  // VFPU1 subop 3
		0x67800000,  // VFPU1 subop 7
		0x6C800000,  // VFPU3 subop 1
		0x6E000000,  // VFPU3 subop 4
		0xD0080000,  // VFPU4 subop 8
		0xD0190000,  // VFPU4 subop 25
		0xD01B0000,  // VFPU4 subop 27
		0xD01D0000,  // VFPU4 subop 29
		0xD0240000,  // VFPU7 subop 4
		0xD0300000,  // VFPU7 subop 16
		0xD0340000,  // VFPU7 subop 20
		0xD04B0000,  // VFPU9 subop 11
		0xD0520000,  // VFPU9 subop 18
		0xD0580000,  // VFPU9 subop 24
		0xD05C0000,  // VFPU9 subop 28
		0xD0800000,  // opcode 52 rs=4
		0xD1E00000,  // opcode 52 rs=15
		0xD2C00000,  // opcode 52 rs=22
		0xD2E00000,  // opcode 52 rs=23
		0xF3000000,  // VFPU6 rs=24
		0xF3600000,  // VFPU6 rs=27
		0xF3810000,  // matrix set subop 1
		0xF3840000,  // matrix set subop 4
		0xF3880000,  // matrix set subop 8
		0xF3C00000,  // VFPU6 rs=30
		0xF3E00000,  // VFPU6 rs=31
	};
	for (u32 w : invalid) {
		if (ag::Decode(w) != Op::INVALID) {
			printf("  %08x should be invalid, decodes as %s\n", w, ag::OpName(ag::Decode(w)));
			g_testFailures++;
		}
	}
	// Opcodes 51 and 59 are not VFPU.
	CHECK(ag::Decode(0xCC000000) == Op::INVALID);
	CHECK(ag::Decode(0xEC000000) == Op::INVALID);
}

// Cross-check against pspdecompiler's independent Allegrex table (masks and opcodes).
TEST(vfpu_decode_vs_pspdecompiler) {
	const auto path = FindRef("reference/pspdecompiler/allegrex.c");
	if (path.empty()) {
		printf("  (skipped: reference/pspdecompiler not found)\n");
		return;
	}
	std::ifstream f(path);
	std::stringstream ss;
	ss << f.rdbuf();
	const std::string text = ss.str();
	const std::regex re("\\{\\s*I_\\w+,\\s*\"([^\"]+)\",\\s*0x([0-9A-Fa-f]+),\\s*0x([0-9A-Fa-f]+),[^}]*_C2");
	int checked = 0;
	u32 rng = 12345;
	for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
		std::string name = (*it)[1];
		const u32 opcode = (u32)std::stoul((*it)[2], nullptr, 16);
		const u32 mask = (u32)std::stoul((*it)[3], nullptr, 16);
		if (!IsVfpuPrimary(opcode >> 26)) continue;  // mfvme/mtvme are Media Engine ops
		// Normalize to our mnemonic family.
		std::string want = name;
		if (want == "vwb.q") want = "sv.q";
		if (want.rfind("lv", 0) != 0 && want.rfind("sv", 0) != 0) {
			const size_t dot = want.find('.');
			if (dot != std::string::npos) want = want.substr(0, dot);
			if ((want.rfind("vtfm", 0) == 0 || want.rfind("vhtfm", 0) == 0) && isdigit((unsigned char)want.back()))
				want.pop_back();
		}
		// pspdecompiler lists mfvc/mtvc with the same opcode as mfv/mtv (first match wins there).
		const u32 force = (want == "mfvc" || want == "mtvc") ? 0x80 : 0;
		for (int k = 0; k < 17; k++) {
			u32 w = opcode | force;
			if (k > 0 && mask != 0xFFFFFFFF) {
				rng = rng * 1103515245u + 12345u;
				const u32 r = (rng >> 1) ^ (rng << 17);
				w = opcode | (r & ~mask) | force;
			}
			// pspdecompiler's generic "vsync imm" also covers the vnop and vflush words.
			if (want == "vsync" && (w == 0xFFFF0000 || w == 0xFFFF040D)) continue;
			const Op op = ag::Decode(w);
			if (want != ag::OpName(op)) {
				printf("  %s %08x (mask %08x): decodes as %s\n", name.c_str(), w, mask, ag::OpName(op));
				g_testFailures++;
				break;
			}
			checked++;
		}
	}
	CHECK(checked > 2000);
}

TEST(vfpu_init_ctx) {
	Ctx c;
	memset(&c, 0xCC, sizeof(c));
	vfpu::InitCtx(&c);
	for (int i = 0; i < 128; i++) CHECK_EQ(c.vi[i], 0x7F800001u);  // cpu/vfpu/vavg reads these
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_SPREFIX], 0xE4u);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_TPREFIX], 0xE4u);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_DPREFIX], 0u);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_CC], 0x3Fu);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_REV], 0x7772CEABu);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_RCX0], 0x3F800001u);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_RCX0 + 7], 0x3F800000u);
	CHECK(vfpu::HasDefaultPrefixes(&c));
}

TEST(vfpu_register_layout) {
	// PPSSPP's voffset table: voffset[m * 4 + x * 32 + y] = m * 16 + y * 4 + x.
	int i = 0;
	for (u32 m = 0; m < 8; m++) {
		for (u32 y = 0; y < 4; y++) {
			for (u32 x = 0; x < 4; x++) {
				CHECK_EQ(vfpu::VOffset(m * 4 + x * 32 + y), (u32)i);
				CHECK_EQ(vfpu::FromVOffset((u32)i), m * 4 + x * 32 + y);
				i++;
			}
		}
	}
	// Columns are contiguous, rows stride 4.
	for (int k = 0; k < 4; k++) {
		CHECK_EQ(vfpu::VecElem(Reg(0, 0, 0), 4, k), (u32)k);           // C000.q
		CHECK_EQ(vfpu::VecElem(Reg(2, 1, 0), 4, k), 32u + 4 + k);       // C210.q
		CHECK_EQ(vfpu::VecElem(0x20, 4, k), (u32)(k * 4));              // R000.q
		CHECK_EQ(vfpu::VecElem(0x20 | Reg(1, 3, 0), 4, k), 16u + 3 + k * 4);  // R103.q
	}
	// C002.p (row 2) and wrapping, triples start at row 0 or 1.
	CHECK_EQ(vfpu::VecElem(0x40, 2, 0), 2u);
	CHECK_EQ(vfpu::VecElem(0x40, 2, 1), 3u);
	CHECK_EQ(vfpu::VecElem(0x40, 4, 2), 0u);  // C002.q wraps
	CHECK_EQ(vfpu::VecElem(0x40, 3, 0), 1u);  // C001.t
	CHECK_EQ(vfpu::VecElem(0x40, 3, 2), 3u);
	CHECK_EQ(vfpu::VecElem(0x60, 3, 0), 4u);  // R001.t
	// Singles: S<m><c><r>.
	CHECK_EQ(vfpu::VOffset(Reg(5, 2, 3)), 5u * 16 + 2 * 4 + 3);
	CHECK_EQ(vfpu::VecElem(Reg(5, 2, 3), 1, 0), 5u * 16 + 2 * 4 + 3);
	// Matrices: M100 is the identity layout, E100 its transpose.
	for (int j = 0; j < 4; j++) {
		for (int k = 0; k < 4; k++) {
			CHECK_EQ(vfpu::MtxElem(4, 4, j, k), 16u + j * 4 + k);
			CHECK_EQ(vfpu::MtxElem(0x24, 4, j, k), 16u + k * 4 + j);
		}
	}
	CHECK_EQ(vfpu::MtxElem(Reg(0, 2, 2), 2, 1, 1), 15u);  // M022.p, last element
	CHECK_EQ(vfpu::MtxElem(0x40 | 1, 3, 0, 0), 5u);       // M011.t
	// Instruction fields.
	CHECK_EQ(vfpu::VecSize(SZ_S), 1);
	CHECK_EQ(vfpu::VecSize(SZ_P), 2);
	CHECK_EQ(vfpu::VecSize(SZ_T), 3);
	CHECK_EQ(vfpu::VecSize(SZ_Q), 4);
	CHECK_EQ(vfpu::LsRegS(OP_LVS | RT(9) | 3), 9u | 0x60);
	CHECK_EQ(vfpu::LsRegQ(OP_LVQ | RT(9) | 1), 9u | 0x20);
	CHECK_EQ(vfpu::LsOffset(OP_LVQ | 0xFFF0), -16);
}

TEST(vfpu_arith_basic) {
	Ctx c = NewCtx();
	const float a[4] = {1.5f, -2.0f, 3.25f, 100.0f};
	const float b[4] = {0.5f, 4.0f, -1.25f, 0.125f};
	SetVec(&c, Reg(1, 0, 0), 4, a);
	SetVec(&c, Reg(2, 0, 0), 4, b);
	Exec(&c, OP_VADD | SZ_Q | VD(Reg(0, 0, 0)) | VS(Reg(1, 0, 0)) | VT(Reg(2, 0, 0)));
	for (int i = 0; i < 4; i++) CHECK(GetV(&c, 0, 4, i) == a[i] + b[i]);
	Exec(&c, OP_VSUB | SZ_T | VD(Reg(0, 1, 0)) | VS(Reg(1, 0, 0)) | VT(Reg(2, 0, 0)));
	for (int i = 0; i < 3; i++) CHECK(GetV(&c, Reg(0, 1, 0), 3, i) == a[i] - b[i]);
	CHECK(GetV(&c, Reg(0, 1, 0), 4, 3) == 0.0f);  // .t leaves w alone
	Exec(&c, OP_VMUL | SZ_P | VD(Reg(0, 2, 0)) | VS(Reg(1, 0, 0)) | VT(Reg(2, 0, 0)));
	CHECK(GetV(&c, Reg(0, 2, 0), 2, 0) == 0.75f);
	CHECK(GetV(&c, Reg(0, 2, 0), 2, 1) == -8.0f);
	Exec(&c, OP_VDIV | SZ_S | VD(Reg(0, 3, 0)) | VS(Reg(1, 0, 0)) | VT(Reg(2, 0, 0)));
	CHECK(GetV(&c, Reg(0, 3, 0), 1, 0) == 3.0f);
	// vdot.q S300, C100, C200 and vscl.q C310, C100, S203.
	Exec(&c, OP_VDOT | SZ_Q | VD(Reg(3, 0, 0)) | VS(Reg(1, 0, 0)) | VT(Reg(2, 0, 0)));
	CHECK(c.v[vfpu::VOffset(Reg(3, 0, 0))] == 0.75f - 8.0f - 4.0625f + 12.5f);
	Exec(&c, OP_VSCL | SZ_Q | VD(Reg(3, 1, 0)) | VS(Reg(1, 0, 0)) | VT(Reg(2, 0, 3)));
	for (int i = 0; i < 4; i++) CHECK(GetV(&c, Reg(3, 1, 0), 4, i) == a[i] * 0.125f);
	// Row vectors: vadd.q R000, R100, R100 doubles the row.
	Exec(&c, OP_VADD | SZ_Q | VD(0x20) | VS(0x20 | 4) | VT(0x20 | 4));
	for (int i = 0; i < 4; i++) CHECK(c.v[i * 4] == 2.0f * c.v[16 + i * 4]);
	// The same encoding works when called directly, as recompiled code does.
	Ctx d = NewCtx();
	SetVec(&d, Reg(1, 0, 0), 4, a);
	SetVec(&d, Reg(2, 0, 0), 4, b);
	sem_VADD(&d, 0x60000000u | 0x8080u | (8u << 16) | (4u << 8));
	for (int i = 0; i < 4; i++) CHECK(d.v[i] == a[i] + b[i]);
	// vmov / vneg / vabs keep NaN payloads.
	d.vi[vfpu::VOffset(4)] = 0x7F800001;
	Exec(&d, V4(2) | SZ_S | VD(0) | VS(4));
	CHECK_EQ(d.vi[0], 0xFF800001u);
	Exec(&d, V4(1) | SZ_S | VD(1) | VS(0));
	CHECK_EQ(d.vi[vfpu::VOffset(1)], 0x7F800001u);
	// vzero / vone / vcst / viim / vfim.
	Exec(&d, V4(7) | SZ_Q | VD(Reg(4, 0, 0)));
	for (int i = 0; i < 4; i++) CHECK(GetV(&d, Reg(4, 0, 0), 4, i) == 1.0f);
	Exec(&d, V4(6) | SZ_P | VD(Reg(4, 0, 0)));
	CHECK(GetV(&d, Reg(4, 0, 0), 4, 1) == 0.0f && GetV(&d, Reg(4, 0, 0), 4, 2) == 1.0f);
	Exec(&d, OP_VCST | (9 << 16) | SZ_S | VD(Reg(4, 1, 0)));
	CHECK_EQ(d.vi[vfpu::VOffset(Reg(4, 1, 0))], 0x40490FDBu);
	Exec(&d, OP_VIIM | VT(Reg(4, 1, 1)) | 0xFF9C);
	CHECK(d.v[vfpu::VOffset(Reg(4, 1, 1))] == -100.0f);
	Exec(&d, OP_VFIM | VT(Reg(4, 1, 2)) | 0x3E00);
	CHECK(d.v[vfpu::VOffset(Reg(4, 1, 2))] == 1.5f);
	// Half denormals are expanded by PPSSPP's Float16ToFloat32, which lands one binade low
	// (0x0001 gives 2^-25, not 2^-24). Kept for parity; not verified on hardware.
	Exec(&d, OP_VFIM | VT(Reg(4, 1, 2)) | 0x0001);
	CHECK(d.v[vfpu::VOffset(Reg(4, 1, 2))] == std::ldexp(1.0f, -25));
}

TEST(vfpu_matrix_ops) {
	Ctx c = NewCtx();
	// M100 and M200 with small integers so every sum is exact.
	for (int i = 0; i < 16; i++) {
		c.v[16 + i] = (float)(i + 1);
		c.v[32 + i] = (float)((i * 7) % 5 - 2);
	}
	// With element (row r, col k) of a non-transposed matrix at v[base + k * 4 + r], the word with
	// vs = E100 computes D = M100 * M200. Assemblers write it "vmmul.q M000, M100, M200" and flip
	// the transpose bit of vs when encoding.
	Exec(&c, OP_VMMUL | SZ_Q | VD(0) | VS(4 ^ 0x20) | VT(8));
	for (int r = 0; r < 4; r++) {
		for (int k = 0; k < 4; k++) {
			float sum = 0;
			for (int x = 0; x < 4; x++) sum += c.v[16 + x * 4 + r] * c.v[32 + k * 4 + x];
			CHECK(c.v[k * 4 + r] == sum);
		}
	}
	// With vs = M100 it is M100^T * M200.
	Exec(&c, OP_VMMUL | SZ_Q | VD(Reg(3, 0, 0)) | VS(4) | VT(8));
	for (int a = 0; a < 4; a++) {
		for (int b = 0; b < 4; b++) {
			float sum = 0;
			for (int x = 0; x < 4; x++) sum += c.v[16 + b * 4 + x] * c.v[32 + a * 4 + x];
			CHECK(c.v[48 + a * 4 + b] == sum);
		}
	}
	// vtfm4.q C400, M100, C200: d[i] = sum_k M100[col i][row k] * t[k].
	Exec(&c, OP_VTFM4 | SZ_Q | VD(Reg(4, 0, 0)) | VS(4) | VT(8));
	for (int i = 0; i < 4; i++) {
		float sum = 0;
		for (int k = 0; k < 4; k++) sum += c.v[16 + i * 4 + k] * c.v[32 + k];
		CHECK(c.v[64 + i] == sum);
	}
	// vhtfm4.q: the vector is a triple with an implied w = 1.
	Exec(&c, OP_VTFM4 | SZ_T | VD(Reg(4, 1, 0)) | VS(4) | VT(8));
	for (int i = 0; i < 4; i++) {
		float sum = 0;
		for (int k = 0; k < 3; k++) sum += c.v[16 + i * 4 + k] * c.v[32 + k];
		sum += c.v[16 + i * 4 + 3];
		CHECK(c.v[68 + i] == sum);
	}
	// vtfm2.p C420, M100, C200.
	Exec(&c, OP_VTFM2 | SZ_P | VD(Reg(4, 2, 0)) | VS(4) | VT(8));
	for (int i = 0; i < 2; i++) CHECK(c.v[72 + i] == c.v[16 + i * 4] * c.v[32] + c.v[16 + i * 4 + 1] * c.v[33]);
	// vmidt.q M500, vmzero.t, vmone.p, vmmov.q, vmscl.q.
	Exec(&c, OP_VMIDT | SZ_Q | VD(Reg(5, 0, 0)));
	for (int j = 0; j < 16; j++) CHECK(c.v[80 + j] == ((j % 5) == 0 ? 1.0f : 0.0f));
	Exec(&c, OP_VMZERO | SZ_T | VD(Reg(5, 0, 0)));
	CHECK(c.v[80] == 0.0f && c.v[85] == 0.0f && c.v[95] == 1.0f);
	Exec(&c, OP_VMONE | SZ_P | VD(Reg(5, 0, 0)));
	CHECK(c.v[80] == 1.0f && c.v[81] == 1.0f && c.v[82] == 0.0f && c.v[84] == 1.0f && c.v[90] == 0.0f);
	Exec(&c, OP_VMMOV | SZ_Q | VD(Reg(6, 0, 0) | 0x20) | VS(4));  // E600 = M100: transpose copy
	for (int j = 0; j < 4; j++) {
		for (int k = 0; k < 4; k++) CHECK(c.v[96 + k * 4 + j] == c.v[16 + j * 4 + k]);
	}
	c.v[vfpu::VOffset(Reg(7, 0, 0))] = 0.5f;
	Exec(&c, OP_VMSCL | SZ_Q | VD(Reg(6, 0, 0)) | VS(4) | VT(Reg(7, 0, 0)));
	for (int j = 0; j < 16; j++) CHECK(c.v[96 + j] == c.v[16 + j] * 0.5f);
	// vcrsp.t: cross product; vqmul.q: quaternion product.
	const float x[3] = {1, 2, 3}, y[3] = {4, 5, 6};
	SetVec(&c, Reg(7, 1, 0), 3, x);
	SetVec(&c, Reg(7, 2, 0), 3, y);
	Exec(&c, OP_VCRSP | SZ_T | VD(Reg(7, 3, 0)) | VS(Reg(7, 1, 0)) | VT(Reg(7, 2, 0)));
	CHECK(GetV(&c, Reg(7, 3, 0), 3, 0) == -3.0f);
	CHECK(GetV(&c, Reg(7, 3, 0), 3, 1) == 6.0f);
	CHECK(GetV(&c, Reg(7, 3, 0), 3, 2) == -3.0f);
	const float q1[4] = {1, 2, 3, 4}, q2[4] = {5, 6, 7, 8};
	SetVec(&c, Reg(7, 1, 0), 4, q1);
	SetVec(&c, Reg(7, 2, 0), 4, q2);
	Exec(&c, OP_VCRSP | SZ_Q | VD(Reg(7, 3, 0)) | VS(Reg(7, 1, 0)) | VT(Reg(7, 2, 0)));
	// (x, y, z, w) quaternions: q1 * q2.
	CHECK(GetV(&c, Reg(7, 3, 0), 4, 0) == 1 * 8 + 2 * 7 - 3 * 6 + 4 * 5);
	CHECK(GetV(&c, Reg(7, 3, 0), 4, 1) == -1 * 7 + 2 * 8 + 3 * 5 + 4 * 6);
	CHECK(GetV(&c, Reg(7, 3, 0), 4, 2) == 1 * 6 - 2 * 5 + 3 * 8 + 4 * 7);
	CHECK(GetV(&c, Reg(7, 3, 0), 4, 3) == -1 * 5 - 2 * 6 - 3 * 7 + 4 * 8);
}

TEST(vfpu_prefixes) {
	Ctx c = NewCtx();
	const float a[4] = {1.0f, -2.0f, 3.0f, -4.0f};
	const float b[4] = {10.0f, 20.0f, 30.0f, 40.0f};
	SetVec(&c, Reg(1, 0, 0), 4, a);
	SetVec(&c, Reg(2, 0, 0), 4, b);
	const u32 vadd = OP_VADD | SZ_Q | VD(0) | VS(4) | VT(8);
	// S: swizzle wzyx with |abs| on lane 1 and negate on lane 0.
	Exec(&c, OP_VPFXS | vfpu::Swizzle(3, 2, 1, 0) | vfpu::PfxAbs(0, 1, 0, 0) | vfpu::PfxNeg(1, 0, 0, 0));
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_SPREFIX], 0x0001021Bu);
	Exec(&c, vadd);
	CHECK(c.v[0] == 4.0f + 10.0f);   // -(-4)
	CHECK(c.v[1] == 3.0f + 20.0f);   // |3|
	CHECK(c.v[2] == -2.0f + 30.0f);
	CHECK(c.v[3] == 1.0f + 40.0f);
	// Prefixes were consumed.
	CHECK(vfpu::HasDefaultPrefixes(&c));
	// T: constants 0, 1, 3 (slot 0 with abs) and a negated 2.
	Exec(&c, OP_VPFXT | vfpu::MakeConstants(vfpu::C_ZERO, vfpu::C_ONE, vfpu::C_THREE, vfpu::C_TWO) | vfpu::PfxNeg(0, 0, 0, 1));
	Exec(&c, vadd);
	CHECK(c.v[0] == 1.0f);
	CHECK(c.v[1] == -1.0f);
	CHECK(c.v[2] == 6.0f);
	CHECK(c.v[3] == -6.0f);
	// D: saturate 0:1 on x, -1:1 on y, write mask on z.
	c.v[2] = 123.0f;
	Exec(&c, OP_VPFXD | 0x1 | (0x3 << 2) | (1 << 10));
	Exec(&c, vadd);
	CHECK(c.v[0] == 1.0f);
	CHECK(c.v[1] == 1.0f);
	CHECK(c.v[2] == 123.0f);  // masked
	CHECK(c.v[3] == 36.0f);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_DPREFIX], 0u);
	// 0:1 turns -0 into +0 and keeps NaN.
	c.vi[vfpu::VOffset(4)] = 0x80000000;
	Exec(&c, OP_VPFXD | 0x1);
	Exec(&c, V4(0) | SZ_S | VD(1) | VS(4));
	CHECK_EQ(c.vi[vfpu::VOffset(1)], 0u);
	c.vi[vfpu::VOffset(4)] = 0x7FC00000;
	Exec(&c, OP_VPFXD | 0x1);
	Exec(&c, V4(0) | SZ_S | VD(1) | VS(4));
	CHECK_EQ(c.vi[vfpu::VOffset(1)], 0x7FC00000u);
	// A swizzle past the length of a .p op reads zero (vadd zeroes such lanes).
	SetVec(&c, Reg(1, 0, 0), 4, a);
	Exec(&c, OP_VPFXS | vfpu::Swizzle(3, 0, 2, 3));
	Exec(&c, OP_VADD | SZ_P | VD(0) | VS(4) | VT(8));
	CHECK(c.v[0] == 0.0f);
	CHECK(c.v[1] == 1.0f + 20.0f);
	// vpfx*, loads, stores and transfers leave prefixes pending; vnop/vsync/vflush too.
	Exec(&c, OP_VPFXS | 0x1B);
	Exec(&c, OP_VPFXT | 0x1B);
	Exec(&c, OP_VPFXD | 0x55);
	Exec(&c, OP_LVS | RS(0) | RT(0) | 0x100);
	Exec(&c, OP_MTV | RT(0) | 3);
	Exec(&c, 0xFFFF0000);
	Exec(&c, 0xFFFF0320);
	Exec(&c, 0xFFFF040D);
	Exec(&c, V9(16) | VD(3) | VS(2));  // vmfvc
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_SPREFIX], 0x1Bu);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_TPREFIX], 0x1Bu);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_DPREFIX], 0x55u);
	CHECK_EQ(c.vi[vfpu::VOffset(3)], 0x55u);
	// Any other opcode-63 word consumes them.
	Exec(&c, 0xFC000000);
	CHECK(vfpu::HasDefaultPrefixes(&c));
	// vpfxd keeps 12 bits, vpfxs/t 20.
	Exec(&c, OP_VPFXD | 0xFFFFFF);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_DPREFIX], 0xFFFu);
	Exec(&c, OP_VPFXS | 0xFFFFFF);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_SPREFIX], 0xFFFFFu);
}

TEST(vfpu_compare_branch) {
	Ctx c = NewCtx();
	const float a[4] = {1.0f, 5.0f, -3.0f, 0.0f};
	const float b[4] = {2.0f, 5.0f, -4.0f, 0.0f};
	SetVec(&c, 4, 4, a);
	SetVec(&c, 8, 4, b);
	auto cmp = [&](u32 cond, u32 size) { Exec(&c, OP_VCMP | size | VS(4) | VT(8) | cond); };
	cmp(2, SZ_Q);  // LT: 1 0 0 0
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_CC], 0x11u);
	CHECK(Exec(&c, OP_BVT | (0 << 18)));   // x set
	CHECK(!Exec(&c, OP_BVT | (1 << 18)));  // y clear
	CHECK(Exec(&c, OP_BVF | (1 << 18)));
	CHECK(Exec(&c, OP_BVT | (4 << 18)));   // any
	CHECK(!Exec(&c, OP_BVT | (5 << 18)));  // all
	CHECK(Exec(&c, OP_BVFL | (5 << 18)));
	CHECK(!Exec(&c, OP_BVTL | (5 << 18)));
	cmp(3, SZ_P);  // LE on a pair: 1 1, upper lanes keep their old bits
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_CC], 0x33u);
	cmp(4, SZ_Q);  // TR
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_CC], 0x3Fu);
	cmp(0, SZ_S);  // FL on a single clears x, any, all
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_CC], 0x0Eu);
	cmp(8, SZ_Q);  // EZ
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_CC], 0x18u);
	c.vi[vfpu::VOffset(4)] = 0x7F800000;
	c.vi[vfpu::VOffset(4 | 0x20)] = 0x7FC00000;
	cmp(10, SZ_P);  // EI: inf, not NaN
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_CC] & 0x33u, 0x11u);
	cmp(9, SZ_P);  // EN
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_CC] & 0x33u, 0x12u);
	cmp(11, SZ_P);  // ES
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_CC] & 0x33u, 0x33u);
	// vcmovt / vcmovf on a single condition bit and per lane.
	const float d0[4] = {-1, -1, -1, -1};
	SetVec(&c, Reg(3, 0, 0), 4, d0);
	SetVec(&c, 8, 4, b);
	c.vfpuCtrl[VFPU_CTRL_CC] = 0x05;  // x and z
	Exec(&c, OP_VCMOVT | (6 << 16) | SZ_Q | VD(Reg(3, 0, 0)) | VS(8));
	CHECK(c.v[48] == 2.0f && c.v[49] == -1.0f && c.v[50] == -4.0f && c.v[51] == -1.0f);
	Exec(&c, OP_VCMOVF | (1 << 16) | SZ_Q | VD(Reg(3, 0, 0)) | VS(4));  // y clear: move all
	CHECK(c.v[48] == c.v[16] && c.v[51] == c.v[19]);
	// Branch semantics through the interpreter table, as decoded kinds.
	CHECK(ag::OpKind(ag::Decode(OP_BVTL)) == ag::Kind::BRL);
}

TEST(vfpu_convert) {
	Ctx c = NewCtx();
	const float in[4] = {2.5f, -2.5f, 3.5f, 1e10f};
	SetVec(&c, 4, 4, in);
	Exec(&c, OP_VF2IN | SZ_Q | VD(0) | VS(4));  // round to nearest even
	CHECK_EQ(c.vi[0], 2u);
	CHECK_EQ(c.vi[1], (u32)-2);
	CHECK_EQ(c.vi[2], 4u);
	CHECK_EQ(c.vi[3], 0x7FFFFFFFu);
	Exec(&c, OP_VF2IZ | SZ_Q | VD(0) | VS(4));
	CHECK_EQ(c.vi[0], 2u);
	CHECK_EQ(c.vi[1], (u32)-2);
	Exec(&c, OP_VF2IU | SZ_P | VD(0) | VS(4));
	CHECK_EQ(c.vi[0], 3u);
	CHECK_EQ(c.vi[1], (u32)-2);
	Exec(&c, OP_VF2ID | SZ_P | VD(0) | VS(4));
	CHECK_EQ(c.vi[0], 2u);
	CHECK_EQ(c.vi[1], (u32)-3);
	// Scaled: vf2in.s with 2^8, NaN gives INT_MAX, -huge gives INT_MIN.
	Exec(&c, OP_VF2IN | (8 << 16) | SZ_S | VD(0) | VS(4));
	CHECK_EQ(c.vi[0], 640u);
	c.vi[vfpu::VOffset(4)] = 0xFFC00000;
	Exec(&c, OP_VF2IN | SZ_S | VD(0) | VS(4));
	CHECK_EQ(c.vi[0], 0x7FFFFFFFu);
	c.v[vfpu::VOffset(4)] = -1e20f;
	Exec(&c, OP_VF2IZ | SZ_S | VD(0) | VS(4));
	CHECK_EQ(c.vi[0], 0x80000000u);
	// vi2f with a scale of 2^-16.
	const u32 ints[4] = {65536u, (u32)-32768, 3, 0x7FFFFFFF};
	SetVecBits(&c, 8, 4, ints);
	Exec(&c, OP_VI2F | (16 << 16) | SZ_Q | VD(Reg(3, 0, 0)) | VS(8));
	CHECK(c.v[48] == 1.0f);
	CHECK(c.v[49] == -0.5f);
	CHECK(c.v[50] == 3.0f / 65536.0f);
	CHECK(c.v[51] == 32768.0f);
	// Through the prefix path: negate flips the integer's sign bit, not its value.
	Exec(&c, OP_VPFXS | 0xE4 | vfpu::PfxNeg(1, 0, 0, 0));
	Exec(&c, OP_VI2F | SZ_S | VD(Reg(3, 0, 0)) | VS(8));
	CHECK(c.v[48] == (float)(s32)0x80010000u);
	// Half floats: vf2h.p / vh2f.s.
	const float h[2] = {1.0f, -2.0f};
	SetVec(&c, 4, 2, h);
	Exec(&c, V7(18) | SZ_P | VD(0) | VS(4));
	CHECK_EQ(c.vi[0], 0xC0003C00u);
	Exec(&c, V7(19) | SZ_S | VD(Reg(0, 1, 0)) | VS(0));
	CHECK(c.v[4] == 1.0f && c.v[5] == -2.0f);
	// Color conversion: vt5650.q.
	const u32 rgba[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};
	SetVecBits(&c, 4, 4, rgba);
	Exec(&c, V9(27) | SZ_Q | VD(0) | VS(4));
	CHECK_EQ(c.vi[0], 0x07E0001Fu);
	CHECK_EQ(c.vi[1], 0xFFFFF800u);
	// vi2uc / vuc2i.
	const u32 pos[4] = {0x7FFFFFFF, 0x40000000, 0, 0x80000000};
	SetVecBits(&c, 4, 4, pos);
	Exec(&c, V7(28) | SZ_Q | VD(0) | VS(4));
	CHECK_EQ(c.vi[0], 0x000080FFu);
	c.vi[vfpu::VOffset(4)] = 0x80FF0001;
	Exec(&c, V7(24) | SZ_S | VD(0) | VS(4));
	CHECK_EQ(c.vi[0], 0x00808080u);
	CHECK_EQ(c.vi[2], 0x7FFFFFFFu);
	CHECK_EQ(c.vi[3], 0x40404040u);
}

TEST(vfpu_transcendental) {
	// Spot values recorded on hardware (pspautotests cpu/vfpu/exact.expected), which the ported
	// PPSSPP interpolator reproduces bit for bit.
	struct Case {
		u32 in, rcp, rsq, sqrt, sin, cos, exp2, log2;
	};
	const Case cases[] = {
		{0x3F000000, 0x40000000, 0x3FB504F0, 0x3F3504F0, 0x3F3504F0, 0x3F3504F0, 0x3FB504F0, 0xBF800000},
		{0x40490FDB, 0x3EA2F980, 0x3F106EB8, 0x3FE2DFC0, 0xBF79B1B4, 0x3E61E0D0, 0x410D331C, 0x3FD36438},
		{0x3EAAAAAB, 0x403FFFFC, 0x3FDDB3D8, 0x3F13CD38, 0x3EFFFFF8, 0x3F5DB3D4, 0x3FA14514, 0xBFCAE000},
		{0x3B03907D, 0x43F91074, 0x41B28CE4, 0x3D3785BC, 0x3B4EA880, 0x3F7FFFAC, 0x3F802DA0, 0xC10F5DC0},
		{0x7F800000, 0x00000000, 0x00000000, 0x7F800000, 0x7F800001, 0x7F800001, 0x7F800000, 0x7F800000},
		{0x80000000, 0xFF800000, 0xFF800000, 0x00000000, 0x80000000, 0x3F800000, 0x3F800000, 0xFF800000},
	};
	for (const Case &k : cases) {
		CHECK_EQ(vfpu::Rcp(k.in), k.rcp);
		CHECK_EQ(vfpu::Rsqrt(k.in), k.rsq);
		CHECK_EQ(vfpu::Sqrt(k.in), k.sqrt);
		CHECK_EQ(vfpu::Sin(k.in), k.sin);
		CHECK_EQ(vfpu::Cos(k.in), k.cos);
		CHECK_EQ(vfpu::Exp2(k.in), k.exp2);
		CHECK_EQ(vfpu::Log2(k.in), k.log2);
	}
	// Through the instructions: vsin.q / vcos.q / vrsq.q / vexp2.q on four lanes at once.
	Ctx c = NewCtx();
	const u32 in[4] = {cases[0].in, cases[1].in, cases[2].in, cases[3].in};
	SetVecBits(&c, 4, 4, in);
	struct {
		u32 sub;
		u32 Case::*field;
	} ops[] = {{18, &Case::sin}, {19, &Case::cos}, {17, &Case::rsq}, {20, &Case::exp2}, {16, &Case::rcp},
	           {21, &Case::log2}, {22, &Case::sqrt}};
	for (auto &o : ops) {
		Exec(&c, V4(o.sub) | SZ_Q | VD(0) | VS(4));
		for (int i = 0; i < 4; i++) CHECK_EQ(c.vi[i], cases[i].*o.field);
	}
	// vrot.q C000, S100, [c, s, 0, 0] equals vcos/vsin of the same input.
	Exec(&c, OP_VROT | ((1 << 2) | 0) << 16 | SZ_Q | VD(0) | VS(4));
	CHECK_EQ(c.vi[0], cases[0].cos);
	CHECK_EQ(c.vi[1], cases[0].sin);
	CHECK_EQ(c.vi[2], 0u);
}

// The hardware datapath model (kHardwareFloat): values from the recorded .expected files.
TEST(vfpu_hw_float_model) {
	if (!vfpu::kHardwareFloat) return;
	Ctx c = NewCtx();
	const u32 specials[4] = {0x7FBFFFFF, 0xFFBFFFFF, 0x7F800000, 0xFF800000};  // psp-gcc NAN, -NAN, inf, -inf
	const u32 zeros[4] = {0x00000000, 0x80000000, 0x000116C2, 0x800116C2};     // 0, -0, denormals
	const u32 vadd = OP_VADD | SZ_Q | VD(0) | VS(4) | VT(5);
	auto run = [&](const u32 *a, u32 d, u32 op) {
		SetVecBits(&c, 4, 4, a);
		for (int i = 0; i < 4; i++) c.vi[20 + i] = 0;  // C110 = 0
		if (d) Exec(&c, OP_VPFXD | d);
		Exec(&c, op);
		return Fmt("%08x,%08x,%08x,%08x", c.vi[0], c.vi[1], c.vi[2], c.vi[3]);
	};
	// prefix_sat: vadd.q x + 0 with d[0:1] and d[-1:1].
	CHECK(run(specials, 0x55, vadd) == "7f800001,7f800001,3f800000,00000000");
	CHECK(run(zeros, 0x55, vadd) == "00000000,00000000,00000000,00000000");
	CHECK(run(specials, 0xFF, vadd) == "7f800001,7f800001,3f800000,bf800000");
	CHECK(run(zeros, 0xFF, vadd) == "00000000,00000000,00000000,00000000");
	// Without saturation: denormals flush to a signed zero (x + x keeps the sign).
	CHECK(run(zeros, 0, OP_VADD | SZ_Q | VD(0) | VS(4) | VT(4)) == "00000000,80000000,00000000,80000000");
	// prefix_sat: vdiv.q of nan,-nan,inf,-inf by 1.5,-2.5,3.5,-4.5.
	const float divisors[4] = {1.5f, -2.5f, 3.5f, -4.5f};
	SetVec(&c, 5, 4, divisors);
	SetVecBits(&c, 4, 4, specials);
	Exec(&c, OP_VDIV | SZ_Q | VD(0) | VS(4) | VT(5));
	CHECK_EQ(c.vi[0], 0x7F800001u);
	CHECK_EQ(c.vi[1], 0x7F800001u);  // the divider's NaN has the sign of x ^ y
	CHECK_EQ(c.vi[2], 0x7F800000u);
	CHECK_EQ(c.vi[3], 0x7F800000u);
	// prefixes checkOps: vmul's NaN keeps the sign of x ^ y, vadd's is positive.
	c.vi[vfpu::VOffset(4)] = 0xFFBFFFFF;
	c.v[vfpu::VOffset(5)] = 1.0f / 3.0f;
	Exec(&c, OP_VMUL | SZ_S | VD(0) | VS(4) | VT(5));
	CHECK_EQ(c.vi[0], 0xFF800001u);
	Exec(&c, OP_VADD | SZ_S | VD(0) | VS(4) | VT(5));
	CHECK_EQ(c.vi[0], 0x7F800001u);

	// specials: vcmp reads denormals as zero. s = 0,-0,den,-den against t = 1,nan,0,-inf.
	SetVecBits(&c, 4, 4, zeros);
	const u32 t1[4] = {0x3F800000, 0x7FBFFFFF, 0x00000000, 0xFF800000};
	SetVecBits(&c, 5, 4, t1);
	const struct {
		u32 cond, cc;
	} cmps[] = {{1, 0x14}, {3, 0x15}, {5, 0x1B}, {7, 0x18}, {8, 0x3F}, {12, 0x00}};
	for (const auto &k : cmps) {
		Exec(&c, OP_VCMP | SZ_Q | VS(4) | VT(5) | k.cond);
		CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_CC], k.cc);
	}

	// minmax_zero: denormals tie with zero and a tie returns t; a denormal that wins is kept.
	const u32 dz[4] = {0x000116C2, 0x800116C2, 0x000116C2, 0x800116C2};
	const u32 z4[4] = {0x00000000, 0x00000000, 0x80000000, 0x80000000};
	const u32 one[4] = {0x3F800000, 0x3F800000, 0xBF800000, 0xBF800000};
	SetVecBits(&c, 4, 4, dz);
	SetVecBits(&c, 5, 4, z4);
	Exec(&c, OP_VMIN | SZ_Q | VD(0) | VS(4) | VT(5));
	CHECK(Fmt("%08x,%08x,%08x,%08x", c.vi[0], c.vi[1], c.vi[2], c.vi[3]) == "00000000,00000000,80000000,80000000");
	SetVecBits(&c, 5, 4, one);
	Exec(&c, OP_VMAX | SZ_Q | VD(0) | VS(4) | VT(5));
	CHECK(Fmt("%08x,%08x,%08x,%08x", c.vi[0], c.vi[1], c.vi[2], c.vi[3]) == "3f800000,3f800000,000116c2,800116c2");

	// overlap_vcrsp: lanes run z, y, x, each reading the registers the previous one wrote.
	auto crsp = [&](u32 w) {
		for (int i = 0; i < 16; i++) {
			c.v[i] = (float)(i + 1);
			c.v[16 + i] = (float)(101 + i);
		}
		Exec(&c, w);
		return Fmt("%08x,%08x,%08x,%08x", c.vi[0], c.vi[1], c.vi[2], c.vi[3]);
	};
	CHECK(crsp(0xF2848000) == "c97e1450,c61f6c00,c2c80000,40800000");  // vcrsp.t C000, C000, C100
	CHECK(crsp(0xF2848040) == "3f800000,c69dc400,43480000,c2c80000");  // vcrsp.t C001, C000, C100
	CHECK(crsp(0xF284C000) == "c625b400,43460000,c2c60000,40800000");  // vcrsp.t C000, C001, C100
	// The VFPU rounds to nearest even when the FPU's mode (mirrored into MXCSR) says otherwise,
	// and leaves MXCSR as it found it.
	const unsigned csr = _mm_getcsr();
	_mm_setcsr((csr & ~(unsigned)_MM_ROUND_MASK) | _MM_ROUND_TOWARD_ZERO);
	c.fcr31 = 1;
	c.v[vfpu::VOffset(4)] = 1.0f;
	c.vi[vfpu::VOffset(5)] = 0x33C00000;  // 1.5 * 2^-24
	Exec(&c, OP_VADD | SZ_S | VD(0) | VS(4) | VT(5));
	const u32 rounded = c.vi[0];
	const unsigned after = _mm_getcsr();
	_mm_setcsr(csr);
	c.fcr31 = 0;
	CHECK_EQ(rounded, 0x3F800001u);
	CHECK_EQ(after & _MM_ROUND_MASK, (unsigned)_MM_ROUND_TOWARD_ZERO);
	// Without overlap it's the cross product.
	for (int i = 0; i < 16; i++) {
		c.v[i] = (float)(i + 1);
		c.v[16 + i] = (float)(101 + i);
	}
	Exec(&c, OP_VCRSP | SZ_T | VD(8) | VS(0) | VT(4));
	CHECK(c.v[32] == 2.0f * 103 - 3.0f * 102 && c.v[33] == 3.0f * 101 - 1.0f * 103 && c.v[34] == 1.0f * 102 - 2.0f * 101);
}

// Every input in cpu/vfpu/exact.expected (specials, a sweep and random bit patterns), all 11 ops.
TEST(vfpu_hw_exact) {
	const auto path = FindRef("reference/pspautotests/tests/cpu/vfpu/exact.expected");
	if (path.empty()) {
		printf("  (skipped: reference/pspautotests not found)\n");
		return;
	}
	struct OpCol {
		const char *name;
		u32 sub;
	};
	const OpCol cols[] = {{"rcp", 16}, {"rsq", 17}, {"sqrt", 22}, {"sin", 18}, {"cos", 19}, {"nsin", 26},
	                      {"asin", 23}, {"exp2", 20}, {"rexp2", 28}, {"log2", 21}, {"nrcp", 24}};
	int checked = 0, bad = 0;
	for (const std::string &line : ReadLines(path)) {
		if (line.size() < 10 || line[8] != ':') continue;
		const u32 in = (u32)std::stoul(line.substr(0, 8), nullptr, 16);
		std::istringstream ss(line.substr(9));
		std::string name, val;
		while (ss >> name >> val) {
			const u32 want = (u32)std::stoul(val, nullptr, 16);
			for (const OpCol &col : cols) {
				if (name != col.name) continue;
				Ctx c = NewCtx();
				c.vi[vfpu::VOffset(4)] = in;
				Exec(&c, V4(col.sub) | SZ_S | VD(0) | VS(4));
				if (c.vi[0] != want) {
					if (bad < 10) printf("  %s(%08x) = %08x, hardware %08x\n", col.name, in, c.vi[0], want);
					bad++;
				}
				checked++;
			}
		}
	}
	CHECK(checked > 3000);
	CHECK_EQ(bad, 0);
}

// Replays cpu/vfpu/prefix_consume: which ops consume pending prefixes, and their results with an
// S swizzle+negate, T constants and D saturation pending.
TEST(vfpu_hw_prefix_consume) {
	const auto path = FindRef("reference/pspautotests/tests/cpu/vfpu/prefix_consume.expected");
	if (path.empty()) {
		printf("  (skipped: reference/pspautotests not found)\n");
		return;
	}
	constexpr u32 C000 = 0, C010 = 1, C100 = 4, C110 = 5, C120 = 6, C130 = 7, S000 = 0, S100 = 4, S110 = 5;
	constexpr u32 M000 = 0, M100 = 4, T3 = 11, ADDR_A = 0x1000, ADDR_SCRATCH = 0x2000;
	const auto three = [](u32 op, u32 sz, u32 d, u32 s, u32 t) { return op | sz | VD(d) | VS(s) | VT(t); };
	const auto two = [](u32 op, u32 sz, u32 d, u32 s) { return op | sz | VD(d) | VS(s); };
	const u32 vcmpLt = OP_VCMP | SZ_Q | 2 | VS(C100) | VT(C110);
	const std::vector<std::pair<std::string, std::vector<u32>>> tests = {
		{"(nothing)", {}},
		{"vadd.q", {three(OP_VADD, SZ_Q, C000, C100, C110)}},
		{"vadd.s", {three(OP_VADD, SZ_S, S000, S100, S110)}},
		{"vadd.p", {three(OP_VADD, SZ_P, C000, C100, C110)}},
		{"vsub.q", {three(OP_VSUB, SZ_Q, C000, C100, C110)}},
		{"vmul.q", {three(OP_VMUL, SZ_Q, C000, C100, C110)}},
		{"vdiv.q", {three(OP_VDIV, SZ_Q, C000, C100, C110)}},
		{"vmin.q", {three(OP_VMIN, SZ_Q, C000, C100, C110)}},
		{"vmax.q", {three(OP_VMAX, SZ_Q, C000, C100, C110)}},
		{"vscl.q", {three(OP_VSCL, SZ_Q, C000, C100, S110)}},
		{"vdot.q", {three(OP_VDOT, SZ_Q, S000, C100, C110)}},
		{"vhdp.q", {three(OP_VHDP, SZ_Q, S000, C100, C110)}},
		{"vcrs.t", {three(OP_VCRS, SZ_T, C000, C100, C110)}},
		{"vcrsp.t", {three(OP_VCRSP, SZ_T, C000, C100, C110)}},
		{"vqmul.q", {three(OP_VCRSP, SZ_Q, C000, C100, C110)}},
		{"vmov.q", {two(V4(0), SZ_Q, C000, C100)}},
		{"vneg.q", {two(V4(2), SZ_Q, C000, C100)}},
		{"vabs.q", {two(V4(1), SZ_Q, C000, C100)}},
		{"vsat0.q", {two(V4(4), SZ_Q, C000, C100)}},
		{"vsat1.q", {two(V4(5), SZ_Q, C000, C100)}},
		{"vzero.q", {two(V4(6), SZ_Q, C000, 0)}},
		{"vone.q", {two(V4(7), SZ_Q, C000, 0)}},
		{"vidt.q", {two(V4(3), SZ_Q, C000, 0)}},
		{"vcst.q", {OP_VCST | (9 << 16) | SZ_Q | VD(C000)}},
		{"vrcp.q", {two(V4(16), SZ_Q, C000, C100)}},
		{"vrsq.q", {two(V4(17), SZ_Q, C000, C100)}},
		{"vsin.q", {two(V4(18), SZ_Q, C000, C100)}},
		{"vcos.q", {two(V4(19), SZ_Q, C000, C100)}},
		{"vexp2.q", {two(V4(20), SZ_Q, C000, C100)}},
		{"vlog2.q", {two(V4(21), SZ_Q, C000, C100)}},
		{"vsqrt.q", {two(V4(22), SZ_Q, C000, C100)}},
		{"vasin.q", {two(V4(23), SZ_Q, C000, C100)}},
		{"vnrcp.q", {two(V4(24), SZ_Q, C000, C100)}},
		{"vnsin.q", {two(V4(26), SZ_Q, C000, C100)}},
		{"vrexp2.q", {two(V4(28), SZ_Q, C000, C100)}},
		{"vsgn.q", {two(V9(10), SZ_Q, C000, C100)}},
		{"vocp.q", {two(V9(4), SZ_Q, C000, C100)}},
		{"vsocp.p", {two(V9(5), SZ_P, C000, C100)}},
		{"vbfy1.q", {two(V9(2), SZ_Q, C000, C100)}},
		{"vbfy2.q", {two(V9(3), SZ_Q, C000, C100)}},
		{"vavg.q", {two(V9(7), SZ_Q, S000, C100)}},
		{"vfad.q", {two(V9(6), SZ_Q, S000, C100)}},
		{"vf2iz.q", {two(OP_VF2IZ, SZ_Q, C000, C100)}},
		{"vf2in.q 2", {two(OP_VF2IN | (2 << 16), SZ_Q, C000, C100)}},
		{"vi2f.q", {two(OP_VI2F, SZ_Q, C000, C100)}},
		{"vf2h.q", {two(V7(18), SZ_Q, C000, C100)}},
		{"vh2f.p", {two(V7(19), SZ_P, C000, C100)}},
		{"vi2c.q", {two(V7(29), SZ_Q, S000, C100)}},
		{"vi2s.q", {two(V7(31), SZ_Q, C000, C100)}},
		{"vc2i.s", {two(V7(25), SZ_S, C000, S100)}},
		{"vs2i.p", {two(V7(27), SZ_P, C000, C100)}},
		{"vi2uc.q", {two(V7(28), SZ_Q, S000, C100)}},
		{"vi2us.q", {two(V7(30), SZ_Q, C000, C100)}},
		{"vuc2ifs.s", {two(V7(24), SZ_S, C000, S100)}},
		{"vus2i.p", {two(V7(26), SZ_P, C000, C100)}},
		{"vsge.q", {three(OP_VSGE, SZ_Q, C000, C100, C110)}},
		{"vslt.q", {three(OP_VSLT, SZ_Q, C000, C100, C110)}},
		{"vcmp.q", {vcmpLt}},
		{"vcmp+vcmovt.q", {vcmpLt, two(OP_VCMOVT, SZ_Q, C000, C100)}},
		{"vcmp+vcmovf.q", {vcmpLt, two(OP_VCMOVF, SZ_Q, C000, C100)}},
		{"vrot.q", {two(OP_VROT, SZ_Q, C000, S100)}},  // [c, s, s, s]
		{"vmmov.q", {two(OP_VMMOV, SZ_Q, M000, M100)}},
		{"vmzero.q", {two(OP_VMZERO, SZ_Q, M000, 0)}},
		{"vmidt.q", {two(OP_VMIDT, SZ_Q, M000, 0)}},
		{"vmscl.q", {three(OP_VMSCL, SZ_Q, M000, M100, S110)}},
		{"vmmul.q", {three(OP_VMMUL, SZ_Q, M000, M100 ^ 0x20, M100)}},  // assembler flips vs
		{"vtfm4.q", {three(OP_VTFM4, SZ_Q, C000, M100, C110)}},
		{"vhtfm4.q", {three(OP_VTFM4, SZ_T, C000, M100, C110)}},
		{"vtfm3.t", {three(OP_VTFM3, SZ_T, C000, M100, C110)}},
		{"vwbn.s", {two(OP_VWBN | (10 << 16), SZ_S, S000, S100)}},
		{"vsbn.s", {three(OP_VSBN, SZ_S, S000, S100, S110)}},
		{"vsbz.s", {two(V7(22), SZ_S, S000, S100)}},
		{"vlgb.s", {two(V7(23), SZ_S, S000, S100)}},
		{"lv.q", {OP_LVQ | RS(T3) | RT(C000)}},
		{"lv.s", {OP_LVS | RS(T3) | RT(S000)}},
		{"sv.q", {OP_SVQ | RS(T3) | RT(C100)}},
		{"mtv", {OP_MTV | RT(0) | S000}},
		{"mfv", {OP_MFV | RT(T3) | S100}},
		{"mfvc", {OP_MFV | RT(T3) | 131}},
		{"vnop", {0xFFFF0000}},
		{"vsync", {0xFFFF0320}},
		{"vflush", {0xFFFF040D}},
		{"nop", {0x00000000}},
		{"addiu", {0x240B0001}},
	};
	const float inS[4] = {1.5f, -2.5f, 3.5f, -4.5f}, inT[4] = {10.0f, 20.0f, 30.0f, 40.0f};
	const float inA[4] = {0.25f, 0.5f, 0.75f, 1.25f}, inB[4] = {100.0f, 200.0f, 300.0f, 400.0f};
	std::vector<std::string> got;
	for (const auto &t : tests) {
		Ctx c = NewCtx();
		for (int i = 0; i < 4; i++) GWrite32(ADDR_A + 4 * i, Bits(inA[i]));
		c.r[T3] = t.first == "sv.q" ? ADDR_SCRATCH : ADDR_A;
		SetVec(&c, C100, 4, inS);
		SetVec(&c, C110, 4, inT);
		SetVec(&c, C120, 4, inA);
		SetVec(&c, C130, 4, inB);
		Exec(&c, OP_VPFXS | 0x000F00B1);  // -y, -x, -w, -z
		Exec(&c, OP_VPFXT | 0x0000F40D);  // 1, 1/2, 3, 0
		Exec(&c, OP_VPFXD | 0x00000055);  // 0:1 x4
		for (u32 w : t.second) Exec(&c, w);
		const u32 sp = c.vfpuCtrl[0], tp = c.vfpuCtrl[1], dp = c.vfpuCtrl[2];
		Exec(&c, OP_VADD | SZ_Q | VD(C010) | VS(C120) | VT(C130));
		std::string line = Fmt("%-14s op=%08x,%08x,%08x,%08x", t.first.c_str(), GetVI(&c, C000, 4, 0),
		                       GetVI(&c, C000, 4, 1), GetVI(&c, C000, 4, 2), GetVI(&c, C000, 4, 3));
		line += Fmt(" then=%08x,%08x,%08x,%08x", GetVI(&c, C010, 4, 0), GetVI(&c, C010, 4, 1), GetVI(&c, C010, 4, 2),
		            GetVI(&c, C010, 4, 3));
		line += Fmt(" pfx=%08x,%08x,%08x", sp, tp, dp);
		got.push_back(line);
	}
	// Lines where PPSSPP's interpreter (which we follow) differs from the hardware; PPSSPP lists
	// this test in tests_next. vuc2ifs.s: the S prefix's lane-0 swizzle (y) is out of range for a
	// single, PPSSPP negates the zero it reads, and the 0x80 top byte becomes 0x40404040, which
	// D 0:1 saturates to 1.0. The hardware doesn't negate, so every lane is 0.
	std::vector<std::string> want = ReadLines(path);
	const std::map<std::string, std::string> ppssppDivergence = {
		{"vuc2ifs.s", "vuc2ifs.s      op=00000000,00000000,00000000,3f800000"},
	};
	for (std::string &w : want) {
		for (const auto &[name, ours] : ppssppDivergence) {
			if (w.rfind(name + " ", 0) == 0) w = ours + w.substr(ours.size());
		}
	}
	CompareLines("prefix_consume", got, want);
}

// Replays cpu/vfpu/vrnd: the generator's sequence and state for several seeds, the float forms,
// vector draws (with the last-lane-only D prefix) and directly written state.
TEST(vfpu_hw_vrnd) {
	const auto path = FindRef("reference/pspautotests/tests/cpu/vfpu/vrnd.expected");
	Ctx c = NewCtx();
	std::vector<std::string> out;
	auto state = [&](const std::string &label) {
		u32 r[8];
		for (int i = 0; i < 8; i++) {
			Exec(&c, OP_MFV | RT(8) | (136 + i));  // mfvc t0, $136+i
			r[i] = c.r[8];
		}
		out.push_back(Fmt("%s RCX: %08x %08x %08x %08x %08x %08x %08x %08x", label.c_str(), r[0], r[1], r[2], r[3], r[4],
		                  r[5], r[6], r[7]));
	};
	auto seed = [&](u32 s) {
		c.r[9] = s;
		Exec(&c, OP_MTV | RT(9) | 0);  // mtv t1, S000
		Exec(&c, V7(0) | VS(0));       // vrnds.s S000
	};
	auto draw = [&](u32 sub) {
		Exec(&c, V7(sub) | SZ_S | VD(0));
		return c.vi[0];
	};
	auto quad = [&](u32 w, bool zero) {
		if (zero) {
			for (int i = 0; i < 4; i++) c.vi[i] = 0;
		}
		Exec(&c, w);
		return Fmt("%08x %08x %08x %08x", c.vi[0], c.vi[1], c.vi[2], c.vi[3]);
	};
	const u32 seeds[] = {0, 1, 0x12345678, 0xFFFFFFFF, 0x80000000, 0xDEADBEEF};
	for (u32 s : seeds) {
		out.push_back(Fmt("-- seed %08x --", s));
		seed(s);
		state("after vrnds");
		for (int i = 0; i < 6; i++) state(Fmt("vrndi=%08x", draw(1)));
		std::string line = "next 40:";
		for (int i = 0; i < 40; i++) {
			line += Fmt(" %08x", draw(1));
			if (i % 8 == 7 && i != 39) {
				out.push_back(line);
				line = "        ";
			}
		}
		out.push_back(line);
	}
	out.push_back("-- float forms, seed 1 --");
	seed(1);
	for (int i = 0; i < 6; i++) {
		const u32 f1 = draw(2), f2 = draw(3);
		out.push_back(Fmt("vrndf1=%08x vrndf2=%08x", f1, f2));
	}
	state("after those");
	out.push_back("-- vector draws, seed 1 --");
	seed(1);
	out.push_back("vrndi.q: " + quad(V7(1) | SZ_Q, false));
	state("  state");
	out.push_back("vrndf1.q: " + quad(V7(2) | SZ_Q, false));
	Exec(&c, OP_VPFXS | 0x1C214);  // -x, |y|, 1, 0
	Exec(&c, OP_VPFXD | 0x4C5);    // 0:1, 0:1, m, -1:1
	out.push_back("vrndi.q s[-x,|y|,1,0] d[0:1,0:1,m,-1:1]: " + quad(V7(1) | SZ_Q, false));
	state("  state");
	out.push_back("-- state written directly --");
	const u32 st[4][8] = {
		{0x00000001, 0x00000002, 0x00000004, 0x00000008, 0, 0, 0, 0},
		{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
		{0x3F80ABCD, 0x3F810000, 0x3F82FFFF, 0x3F830001, 0x3F840002, 0x3F850003, 0x3F860004, 0x3F870005},
		{0x12345678, 0x9ABCDEF0, 0x0F0F0F0F, 0xF0F0F0F0, 0x00FF00FF, 0xFF00FF00, 0x55AA55AA, 0xAA55AA55},
	};
	for (const auto &s : st) {
		for (int i = 0; i < 8; i++) {
			c.r[8] = s[i];
			Exec(&c, OP_MTV | RT(8) | (136 + i));  // mtvc t0, $136+i
		}
		out.push_back(Fmt("wrote %08x %08x %08x %08x %08x %08x %08x %08x", s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]));
		state("  reads back");
		for (int i = 0; i < 4; i++) state(Fmt("  vrndi=%08x", draw(1)));
	}
	out.push_back("-- vrndi.p and vrndi.t --");
	seed(1);
	out.push_back("vrndi.p: " + quad(V7(1) | SZ_P, true));
	state("  state");
	out.push_back("vrndi.t: " + quad(V7(1) | SZ_T, true));
	state("  state");

	// Determinism without the reference: seed 0 starts 00000001, 00010dce, 1c5983f7 on hardware.
	CHECK(out[2] == "vrndi=00000001 RCX: 3f800001 3f800000 3f800000 3f800000 3f800000 3f800000 3f800000 3f800000");
	CHECK(out[3].rfind("vrndi=00010dce", 0) == 0);
	if (path.empty()) {
		printf("  (hardware comparison skipped: reference/pspautotests not found)\n");
		return;
	}
	CompareLines("vrnd", out, ReadLines(path));
}

TEST(vfpu_load_store) {
	Ctx c = NewCtx();
	for (u32 i = 0; i < 16; i++) GWrite32(0x3000 + 4 * i, 0x40000000 + i);
	c.r[4] = 0x3000;
	// lv.q C100, 16(a0) and the transposed R100.
	Exec(&c, OP_LVQ | RS(4) | RT(4) | 16);
	for (int i = 0; i < 4; i++) CHECK_EQ(c.vi[16 + i], 0x40000004u + i);
	Exec(&c, OP_LVQ | RS(4) | RT(4) | 1 | 32);  // R100
	for (int i = 0; i < 4; i++) CHECK_EQ(c.vi[16 + i * 4], 0x40000008u + i);
	// sv.q C100, -16(a0+32) and sv.s / lv.s with the row in bits 0-1.
	c.r[5] = 0x3100;
	Exec(&c, OP_SVQ | RS(5) | RT(4) | 0xFFF0 | 2);  // with the wb flag
	for (int i = 0; i < 4; i++) CHECK_EQ(GRead32(0x30F0 + 4 * i), c.vi[16 + i]);
	Exec(&c, OP_LVS | RS(4) | RT(Reg(2, 1, 0)) | 3 | 8);  // lv.s S213, 8(a0)
	CHECK_EQ(c.vi[vfpu::VOffset(Reg(2, 1, 3))], 0x40000002u);
	Exec(&c, OP_SVS | RS(4) | RT(Reg(2, 1, 0)) | 3 | 60);
	CHECK_EQ(GRead32(0x303C), 0x40000002u);
	// The D write mask applies to lv.q.
	for (int i = 0; i < 4; i++) c.vi[i] = 0;
	Exec(&c, OP_VPFXD | (0x5 << 8));
	Exec(&c, OP_LVQ | RS(4) | RT(0));
	CHECK(c.vi[0] == 0 && c.vi[1] == 0x40000001u && c.vi[2] == 0 && c.vi[3] == 0x40000003u);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_DPREFIX], 0x500u);  // still pending
	c.vfpuCtrl[VFPU_CTRL_DPREFIX] = 0;
	// lvl.q / lvr.q at 0x3008 build the unaligned quad 0x3008..0x3017 from two halves.
	for (int i = 0; i < 4; i++) c.vi[32 + i] = 0;
	c.r[6] = 0x3008;
	Exec(&c, OP_LVLQ | RS(6) | RT(8) | 12);  // lvl.q C200, 12(a2): words up to 0x3014
	Exec(&c, OP_LVRQ | RS(6) | RT(8));       // lvr.q C200, 0(a2): words from 0x3008
	CHECK_EQ(c.vi[32], 0x40000002u);
	CHECK_EQ(c.vi[33], 0x40000003u);
	CHECK_EQ(c.vi[34], 0x40000004u);
	CHECK_EQ(c.vi[35], 0x40000005u);
	// svl.q / svr.q write it back elsewhere.
	c.r[7] = 0x3208;
	Exec(&c, OP_SVLQ | RS(7) | RT(8) | 12);
	Exec(&c, OP_SVRQ | RS(7) | RT(8));
	for (int i = 0; i < 4; i++) CHECK_EQ(GRead32(0x3208 + 4 * i), 0x40000002u + i);
	CHECK_EQ(GRead32(0x3204), 0u);
	CHECK_EQ(GRead32(0x3218), 0u);
}

TEST(vfpu_transfers) {
	Ctx c = NewCtx();
	c.r[8] = 0x12345678;
	Exec(&c, OP_MTV | RT(8) | Reg(3, 2, 1));
	CHECK_EQ(c.vi[vfpu::VOffset(Reg(3, 2, 1))], 0x12345678u);
	Exec(&c, OP_MFV | RT(9) | Reg(3, 2, 1));
	CHECK_EQ(c.r[9], 0x12345678u);
	Exec(&c, OP_MFV | RT(0) | Reg(3, 2, 1));  // $zero stays zero
	CHECK_EQ(c.r[0], 0u);
	// mtvc masks per register; RCX keep 0x3F800000; RSV5/6 and REV are read-only.
	c.r[8] = 0xFFFFFFFF;
	Exec(&c, OP_MTV | RT(8) | 0x80);  // SPFX
	CHECK_EQ(c.vfpuCtrl[0], 0xFFFFFu);
	Exec(&c, OP_MTV | RT(8) | 0x82);  // DPFX
	CHECK_EQ(c.vfpuCtrl[2], 0xFFFu);
	Exec(&c, OP_MTV | RT(8) | 0x83);  // CC
	CHECK_EQ(c.vfpuCtrl[3], 0x3Fu);
	Exec(&c, OP_MTV | RT(8) | 0x84);  // INF4
	CHECK_EQ(c.vfpuCtrl[4], 0xFFFFFFFFu);
	Exec(&c, OP_MTV | RT(8) | 0x87);  // REV
	CHECK_EQ(c.vfpuCtrl[7], 0x7772CEABu);
	c.r[8] = 0;
	Exec(&c, OP_MTV | RT(8) | 0x8A);  // RCX2
	CHECK_EQ(c.vfpuCtrl[10], 0x3F800000u);
	Exec(&c, OP_MFV | RT(10) | 0x87);  // mfvc t2, REV
	CHECK_EQ(c.r[10], 0x7772CEABu);
	c.r[10] = 5;
	Exec(&c, OP_MFV | RT(10) | 0xFF);  // past the control registers: unchanged
	CHECK_EQ(c.r[10], 5u);
	Exec(&c, OP_MFV | RT(0) | 0xFF);  // the interlock idiom
	// vmfvc / vmtvc.
	Exec(&c, V9(16) | VD(Reg(1, 1, 1)) | VS(7));
	CHECK_EQ(c.vi[vfpu::VOffset(Reg(1, 1, 1))], 0x7772CEABu);
	c.vi[vfpu::VOffset(Reg(1, 1, 1))] = 0x15;
	Exec(&c, V9(17) | VS(Reg(1, 1, 1)) | 3);
	CHECK_EQ(c.vfpuCtrl[VFPU_CTRL_CC], 0x15u);
}

TEST(vfpu_misc_ops) {
	Ctx c = NewCtx();
	// vmin/vmax: NaN and inf order by bits, zeros tie to t.
	const u32 s[4] = {0x3F800000, 0x7FC00000, 0x80000000, 0xFF800000};
	const u32 t[4] = {0x40000000, 0x7F800000, 0x00000000, 0xFFC00000};
	SetVecBits(&c, 4, 4, s);
	SetVecBits(&c, 8, 4, t);
	Exec(&c, OP_VMIN | SZ_Q | VD(0) | VS(4) | VT(8));
	CHECK_EQ(c.vi[0], 0x3F800000u);
	CHECK_EQ(c.vi[1], 0x7F800000u);
	CHECK_EQ(c.vi[2], 0x00000000u);
	CHECK_EQ(c.vi[3], 0xFFC00000u);
	Exec(&c, OP_VMAX | SZ_Q | VD(0) | VS(4) | VT(8));
	CHECK_EQ(c.vi[0], 0x40000000u);
	CHECK_EQ(c.vi[1], 0x7FC00000u);
	CHECK_EQ(c.vi[3], 0xFF800000u);
	// vsgn, vsat0/1, vocp, vfad, vavg, vhdp, vdet.
	const float a[4] = {-3.0f, 0.0f, 0.25f, 7.0f};
	SetVec(&c, 4, 4, a);
	Exec(&c, V9(10) | SZ_Q | VD(0) | VS(4));
	CHECK(c.v[0] == -1.0f && c.v[1] == 0.0f && c.v[2] == 1.0f && c.v[3] == 1.0f);
	Exec(&c, V4(4) | SZ_Q | VD(0) | VS(4));
	CHECK(c.v[0] == 0.0f && c.v[2] == 0.25f && c.v[3] == 1.0f);
	Exec(&c, V4(5) | SZ_Q | VD(0) | VS(4));
	CHECK(c.v[0] == -1.0f && c.v[3] == 1.0f);
	Exec(&c, V9(4) | SZ_Q | VD(0) | VS(4));
	CHECK(c.v[0] == 4.0f && c.v[1] == 1.0f && c.v[2] == 0.75f && c.v[3] == -6.0f);
	Exec(&c, V9(6) | SZ_Q | VD(0) | VS(4));
	CHECK(c.v[0] == 4.25f);
	Exec(&c, V9(7) | SZ_Q | VD(0) | VS(4));
	CHECK(c.v[0] == 4.25f * 0.25f);
	const float h[4] = {1.0f, 2.0f, 3.0f, 4.0f};
	SetVec(&c, 8, 4, h);
	Exec(&c, OP_VHDP | SZ_Q | VD(0) | VS(4) | VT(8));  // w of vs forced to 1
	CHECK(c.v[0] == -3.0f + 0.0f + 0.75f + 4.0f);
	Exec(&c, OP_VDET | SZ_P | VD(0) | VS(4) | VT(8));  // -3 * 2 - 0 * 1
	CHECK(c.v[0] == -6.0f);
	// vbfy1/2 and vsrt1.
	Exec(&c, V9(2) | SZ_Q | VD(0) | VS(8));
	CHECK(c.v[0] == 3.0f && c.v[1] == -1.0f && c.v[2] == 7.0f && c.v[3] == -1.0f);
	Exec(&c, V9(3) | SZ_Q | VD(0) | VS(8));
	CHECK(c.v[0] == 4.0f && c.v[1] == 6.0f && c.v[2] == -2.0f && c.v[3] == -2.0f);
	const float u[4] = {4.0f, 1.0f, 2.0f, 3.0f};
	SetVec(&c, 8, 4, u);
	Exec(&c, V9(0) | SZ_Q | VD(0) | VS(8));
	CHECK(c.v[0] == 1.0f && c.v[1] == 4.0f && c.v[2] == 2.0f && c.v[3] == 3.0f);
	// vsbz / vlgb / vwbn / vsbn on 12.0 = 1.5 * 2^3.
	c.v[vfpu::VOffset(4)] = 12.0f;
	Exec(&c, V7(22) | VD(0) | VS(4));
	CHECK(c.v[0] == 1.5f);
	Exec(&c, V7(23) | VD(0) | VS(4));
	CHECK(c.v[0] == 3.0f);
	Exec(&c, OP_VWBN | (130 << 16) | VD(0) | VS(4));  // same exponent: unchanged
	CHECK(c.v[0] == 12.0f);
	c.vi[vfpu::VOffset(8)] = (u32)-1;
	Exec(&c, OP_VSBN | VD(0) | VS(4) | VT(8));
	CHECK(c.v[0] == 0.75f);
	// vidt.q on column 2.
	Exec(&c, V4(3) | SZ_Q | VD(Reg(3, 2, 0)));
	CHECK(GetV(&c, Reg(3, 2, 0), 4, 0) == 0.0f && GetV(&c, Reg(3, 2, 0), 4, 2) == 1.0f);
	// vcrs.t: half a cross product.
	const float x[3] = {1, 2, 3}, y[3] = {4, 5, 6};
	SetVec(&c, 4, 3, x);
	SetVec(&c, 8, 3, y);
	Exec(&c, OP_VCRS | SZ_T | VD(0) | VS(4) | VT(8));
	CHECK(c.v[0] == 2.0f * 6.0f && c.v[1] == 3.0f * 4.0f && c.v[2] == 1.0f * 5.0f);
}

TEST(vfpu_disasm) {
	struct Case {
		u32 w;
		const char *text;
	};
	const Case cases[] = {
		{OP_VADD | SZ_Q | VD(0) | VS(4) | VT(8), "vadd.q C000, C100, C200"},
		{OP_VADD | SZ_T | VD(0x20) | VS(0x41) | VT(8), "vadd.t R000, C011, C200"},
		{OP_VDOT | SZ_Q | VD(Reg(3, 1, 2)) | VS(4) | VT(0x24), "vdot.q S312, C100, R100"},
		{OP_VSCL | SZ_P | VD(0) | VS(4) | VT(Reg(2, 0, 3)), "vscl.p C000, C100, S203"},
		{OP_VMMUL | SZ_Q | VD(0) | VS(0x24) | VT(8), "vmmul.q M000, M100, M200"},
		{OP_VTFM4 | SZ_Q | VD(0) | VS(4) | VT(8), "vtfm4.q C000, M100, C200"},
		{OP_VTFM4 | SZ_T | VD(0) | VS(0x24) | VT(8), "vhtfm4.q C000, E100, C200"},
		{OP_VCMP | SZ_Q | 2 | VS(4) | VT(8), "vcmp.q LT, C100, C200"},
		{OP_LVQ | RS(4) | RT(1) | 1 | 16, "lv.q R001, 16(a0)"},
		{OP_SVQ | RS(29) | RT(4) | 2 | 0xFFF0, "sv.q C100, -16(sp), wb"},
		{OP_LVS | RS(4) | RT(Reg(2, 1, 0)) | 3, "lv.s S213, 0(a0)"},
		{OP_MFV | RT(8) | 0x83, "mfvc t0, CC"},
		{OP_MFV | RT(0) | 0xFF, "mfvc zero, (interlock)"},
		{OP_MTV | RT(9) | Reg(0, 1, 2), "mtv t1, S012"},
		{OP_VPFXS | vfpu::Swizzle(1, 0, 3, 2) | vfpu::PfxNeg(1, 1, 1, 1), "vpfxs [-y, -x, -w, -z]"},
		{OP_VPFXT | 0x0000F40D, "vpfxt [1, 1/2, 3, 0]"},
		{OP_VPFXD | 0x55, "vpfxd [0:1, 0:1, 0:1, 0:1]"},
		{OP_VCST | (9 << 16) | SZ_Q, "vcst.q C000, VFPU_PI"},
		{OP_VF2IZ | (16 << 16) | SZ_Q | VS(4), "vf2iz.q C000, C100, 16"},
		{OP_VIIM | VT(4) | 0xFFFF, "viim.s S100, -1"},
		{OP_VROT | (1 << 16) | SZ_Q | VS(4), "vrot.q C000, S100, [S,C,0,0]"},
		{OP_VMIDT | SZ_Q | VD(0x24), "vmidt.q E100"},
		{0xFFFF0000, "vnop"},
		{0xFFFF0320, "vsync"},
		{0xFFFF040D, "vflush"},
	};
	char buf[128];
	for (const Case &k : cases) {
		ag::Disassemble(0x08800000, k.w, buf, sizeof(buf));
		if (strcmp(buf, k.text) != 0) {
			printf("  %08x: \"%s\", expected \"%s\"\n", k.w, buf, k.text);
			g_testFailures++;
		}
	}
	ag::Disassemble(0x08800000, OP_BVT | (2 << 18) | 3, buf, sizeof(buf));
	CHECK(strcmp(buf, "bvt 2, 0x08800010") == 0);
	// Every VFPU op disassembles to something starting with its mnemonic.
	for (const Canon &e : kCanon) {
		ag::Disassemble(0x08800000, e.w, buf, sizeof(buf));
		CHECK(strncmp(buf, ag::OpName(e.op), strlen(ag::OpName(e.op))) == 0 ||
		      (e.op == Op::VIIM || e.op == Op::VFIM));
	}
}

// Decodes every VFPU-opcode word in the executable sections of the pspautotests cpu/vfpu PRXs.
TEST(vfpu_prx_scan) {
	namespace fs = std::filesystem;
	const auto dir = FindRef("reference/pspautotests/tests/cpu/vfpu");
	if (dir.empty()) {
		printf("  (skipped: reference/pspautotests not found)\n");
		return;
	}
	int files = 0, words = 0, invalid = 0;
	std::map<Op, int> seen;
	for (const auto &entry : fs::directory_iterator(dir)) {
		if (entry.path().extension() != ".prx") continue;
		std::ifstream f(entry.path(), std::ios::binary);
		std::vector<u8> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (d.size() < 52 || memcmp(d.data(), "\x7F" "ELF", 4) != 0) continue;
		auto rd32 = [&](size_t o) { u32 v; memcpy(&v, &d[o], 4); return v; };
		auto rd16 = [&](size_t o) { u16 v; memcpy(&v, &d[o], 2); return v; };
		const u32 shoff = rd32(0x20);
		const u16 shentsize = rd16(0x2E), shnum = rd16(0x30);
		files++;
		for (u32 i = 0; i < shnum; i++) {
			const size_t sh = shoff + (size_t)i * shentsize;
			if (sh + 40 > d.size()) break;
			const u32 type = rd32(sh + 4), flags = rd32(sh + 8), off = rd32(sh + 16), size = rd32(sh + 20);
			if (type != 1 || !(flags & 4) || (size_t)off + size > d.size()) continue;  // PROGBITS + EXECINSTR
			for (u32 o = 0; o + 4 <= size; o += 4) {
				const u32 w = rd32(off + o);
				if (!IsVfpuPrimary(w >> 26)) continue;
				words++;
				const Op op = ag::Decode(w);
				seen[op]++;
				if (op == Op::INVALID) {
					char buf[64];
					ag::Disassemble(0, w, buf, sizeof(buf));
					printf("  %s +%06x: %08x does not decode\n", entry.path().filename().string().c_str(), off + o, w);
					invalid++;
				}
			}
		}
	}
	printf("  %d PRX files, %d VFPU-opcode words, %zu distinct ops, %d undecodable\n", files, words, seen.size(), invalid);
	CHECK(files >= 20);
	CHECK(words > 1000);
	CHECK_EQ(invalid, 0);
}
