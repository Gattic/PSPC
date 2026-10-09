// SPDX-License-Identifier: GPL-2.0-or-later
// Instruction field extraction. Everything is constexpr so that recompiled code, which passes
// the instruction word as a literal, folds the fields away completely.
#pragma once

#include "common/common.h"

namespace insn {

constexpr u32 Opc(u32 w) { return w >> 26; }
constexpr u32 Rs(u32 w) { return (w >> 21) & 31; }
constexpr u32 Rt(u32 w) { return (w >> 16) & 31; }
constexpr u32 Rd(u32 w) { return (w >> 11) & 31; }
constexpr u32 Sa(u32 w) { return (w >> 6) & 31; }
constexpr u32 Funct(u32 w) { return w & 63; }
constexpr u32 Imm16(u32 w) { return w & 0xFFFF; }
constexpr s32 Simm16(u32 w) { return (s32)(s16)(u16)(w & 0xFFFF); }
constexpr u32 Target26(u32 w) { return w & 0x03FFFFFF; }

// FPU register fields (COP1).
constexpr u32 Ft(u32 w) { return (w >> 16) & 31; }
constexpr u32 Fs(u32 w) { return (w >> 11) & 31; }
constexpr u32 Fd(u32 w) { return (w >> 6) & 31; }

// SPECIAL3 ext/ins fields.
constexpr u32 Pos(u32 w) { return (w >> 6) & 31; }
constexpr u32 Size(u32 w) { return (w >> 11) & 31; }

constexpr u32 BranchTarget(u32 pc, u32 w) { return pc + 4 + ((u32)Simm16(w) << 2); }
constexpr u32 JumpTarget(u32 pc, u32 w) { return ((pc + 4) & 0xF0000000u) | (Target26(w) << 2); }

}  // namespace insn
