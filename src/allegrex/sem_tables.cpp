// SPDX-License-Identifier: GPL-2.0-or-later
#include "allegrex/sem_tables.h"

#include "allegrex/sem.h"

namespace ag {

// Per-kind expansion: ALU ops get a semantic wrapper, branches a condition wrapper.
#define K_ALU(id) static void semw_##id(Ctx *c, u32 w) { sem_##id(c, w); }
#define K_BR(id) static bool condw_##id(Ctx *c, u32 w) { return cond_##id(c, w); }
#define K_BRL(id) K_BR(id)
#define K_BRAL(id) K_BR(id)
#define K_BRALL(id) K_BR(id)
#define K_J(id)
#define K_JAL(id)
#define K_JR(id)
#define K_JALR(id)
#define K_SYSCALL(id)
#define K_BREAK(id)
#define K_ERET(id)
#define K_INVALID(id)

#define OP(id, mn, kind) K_##kind(id)
#include "allegrex/ops.inc"
#include "allegrex/vfpu_ops.inc"
#undef OP

#define S_ALU(id) &semw_##id
#define S_BR(id) nullptr
#define S_BRL(id) nullptr
#define S_BRAL(id) nullptr
#define S_BRALL(id) nullptr
#define S_J(id) nullptr
#define S_JAL(id) nullptr
#define S_JR(id) nullptr
#define S_JALR(id) nullptr
#define S_SYSCALL(id) nullptr
#define S_BREAK(id) nullptr
#define S_ERET(id) nullptr
#define S_INVALID(id) nullptr

const SemFn g_semTable[(size_t)Op::COUNT] = {
#define OP(id, mn, kind) S_##kind(id),
#include "allegrex/ops.inc"
#include "allegrex/vfpu_ops.inc"
#undef OP
	nullptr,
};

#define C_ALU(id) nullptr
#define C_BR(id) &condw_##id
#define C_BRL(id) &condw_##id
#define C_BRAL(id) &condw_##id
#define C_BRALL(id) &condw_##id
#define C_J(id) nullptr
#define C_JAL(id) nullptr
#define C_JR(id) nullptr
#define C_JALR(id) nullptr
#define C_SYSCALL(id) nullptr
#define C_BREAK(id) nullptr
#define C_ERET(id) nullptr
#define C_INVALID(id) nullptr

const CondFn g_condTable[(size_t)Op::COUNT] = {
#define OP(id, mn, kind) C_##kind(id),
#include "allegrex/ops.inc"
#include "allegrex/vfpu_ops.inc"
#undef OP
	nullptr,
};

}  // namespace ag
