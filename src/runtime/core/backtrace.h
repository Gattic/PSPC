// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdio>

namespace backtrace {

// Writes the current host call stack (symbolized when PDBs are found) and the running guest thread.
void Print(FILE *out);
// Makes Fatal() print a backtrace before exiting.
void InstallFatalHook();

}  // namespace backtrace
