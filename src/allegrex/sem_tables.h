// SPDX-License-Identifier: GPL-2.0-or-later
// Function-pointer views of the semantics, for the interpreter.
#pragma once

#include "allegrex/ctx.h"
#include "allegrex/decode.h"

namespace ag {

using SemFn = void (*)(Ctx *, u32);
using CondFn = bool (*)(Ctx *, u32);

// Indexed by Op. Non-null for Kind::ALU.
extern const SemFn g_semTable[(size_t)Op::COUNT];
// Indexed by Op. Non-null for branch kinds.
extern const CondFn g_condTable[(size_t)Op::COUNT];

}  // namespace ag
