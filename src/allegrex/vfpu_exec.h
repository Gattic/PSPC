// SPDX-License-Identifier: GPL-2.0-or-later
//
// Out-of-line VFPU helpers, shared by the interpreter and recompiled code (both link pspc_sem).
// vfpu_sem.h handles the common prefix-free cases inline and calls these for everything else.
// Each Exec* function implements one instruction family completely, including the prefix rules
// and consuming the prefixes afterwards.
#pragma once

#include "allegrex/vfpu_regs.h"

namespace vfpu {

// ---- Bit-exact arithmetic (vfpu_math.cpp). Float bits in, float bits out. ----
u32 Sin(u32 x);  // x in quarter turns
u32 Cos(u32 x);
void SinCos(u32 x, u32 *s, u32 *c);
u32 Asin(u32 x);  // result in quarter turns
u32 Sqrt(u32 x);
u32 Rsqrt(u32 x);
u32 Rcp(u32 x);
u32 Exp2(u32 x);
u32 Rexp2(u32 x);
u32 Log2(u32 x);
// The hardware's four-term dot product (bit-exact model, see vfpu_math.cpp): denormals are zero,
// NaN results are 0x7F800001. DotBits is the SSE2 version, DotBitsReference the scalar one.
u32 DotBits(const u32 a[4], const u32 b[4]);
u32 DotBitsReference(const u32 a[4], const u32 b[4]);
void RndInit(u32 seed, u32 *rcx);
u32 RndGenerate(u32 *rcx);
u32 H2F(u16 h);
u16 F2H(u32 f);

// ---- Instruction families (vfpu_exec.cpp) ----
void ExecVecDo3(Ctx *c, u32 w);      // vadd vsub vdiv vmul
void ExecVsbn(Ctx *c, u32 w);
void ExecVDot(Ctx *c, u32 w);
void ExecVScl(Ctx *c, u32 w);
void ExecVHdp(Ctx *c, u32 w);
void ExecVcrs(Ctx *c, u32 w);
void ExecVdet(Ctx *c, u32 w);
void ExecVcmp(Ctx *c, u32 w);
void ExecVminmax(Ctx *c, u32 w);     // vmin vmax
void ExecVscmp(Ctx *c, u32 w);
void ExecVsge(Ctx *c, u32 w);
void ExecVslt(Ctx *c, u32 w);
void ExecVV2Op(Ctx *c, u32 w);       // vmov vabs vneg vsat0/1 vrcp vrsq vsin vcos vexp2 vlog2 vsqrt vasin vnrcp vnsin vrexp2
void ExecVidt(Ctx *c, u32 w);
void ExecVVectorInit(Ctx *c, u32 w); // vzero vone
void ExecVrnds(Ctx *c, u32 w);
void ExecVrndX(Ctx *c, u32 w);       // vrndi vrndf1 vrndf2
void ExecVf2h(Ctx *c, u32 w);
void ExecVh2f(Ctx *c, u32 w);
void ExecVsbz(Ctx *c, u32 w);
void ExecVlgb(Ctx *c, u32 w);
void ExecVx2i(Ctx *c, u32 w);        // vuc2i vc2i vus2i vs2i
void ExecVi2x(Ctx *c, u32 w);        // vi2uc vi2c vi2us vi2s
void ExecVsrt1(Ctx *c, u32 w);
void ExecVsrt2(Ctx *c, u32 w);
void ExecVsrt3(Ctx *c, u32 w);
void ExecVsrt4(Ctx *c, u32 w);
void ExecVbfy(Ctx *c, u32 w);        // vbfy1 vbfy2
void ExecVocp(Ctx *c, u32 w);
void ExecVsocp(Ctx *c, u32 w);
void ExecVfad(Ctx *c, u32 w);
void ExecVavg(Ctx *c, u32 w);
void ExecVsgn(Ctx *c, u32 w);
void ExecColorConv(Ctx *c, u32 w);   // vt4444 vt5551 vt5650
void ExecVcst(Ctx *c, u32 w);
void ExecVf2i(Ctx *c, u32 w);        // vf2in vf2iz vf2iu vf2id
void ExecVi2f(Ctx *c, u32 w);
void ExecVcmov(Ctx *c, u32 w);
void ExecVwbn(Ctx *c, u32 w);
void ExecViim(Ctx *c, u32 w);        // viim vfim
void ExecVmmul(Ctx *c, u32 w);
void ExecVtfm(Ctx *c, u32 w);        // vtfm2-4 vhtfm2-4
void ExecVmscl(Ctx *c, u32 w);
void ExecCrossQuat(Ctx *c, u32 w);   // vcrsp vqmul
void ExecVmmov(Ctx *c, u32 w);
void ExecVMatrixInit(Ctx *c, u32 w); // vmidt vmzero vmone
void ExecVrot(Ctx *c, u32 w);
// Loads and stores (they leave the prefixes alone, but lv.q/lvl.q/lvr.q honour the D write mask).
void ExecLvq(Ctx *c, u32 w);
void ExecSvq(Ctx *c, u32 w);
void ExecLvlq(Ctx *c, u32 w);
void ExecLvrq(Ctx *c, u32 w);
void ExecSvlq(Ctx *c, u32 w);
void ExecSvrq(Ctx *c, u32 w);

}  // namespace vfpu
