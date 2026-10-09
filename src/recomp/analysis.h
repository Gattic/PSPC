// SPDX-License-Identifier: GPL-2.0-or-later
//
// Function discovery for static recompilation.
//
// Strong entry points (module entry, exports, jal/bal targets, hints) bound functions. Each
// function's body is everything reachable from its entry through intra-procedural edges
// (fall-through, conditional branches, j inside its range, recovered jump tables). Bodies may
// overlap; shared code is simply compiled into every function that reaches it. Code pointers
// found in data or in lui/addiu pairs become (weak) entries too, so indirect calls land on
// native code. Anything still missing runs in the interpreter at runtime.
#pragma once

#include <map>
#include <set>
#include <vector>

#include "recomp/image.h"

namespace recomp {

struct JumpTable {
	u32 table = 0;              // address of the table (as loaded at the recompile base)
	std::vector<u32> targets;   // entries
};

struct Function {
	u32 entry = 0;
	u32 rangeEnd = 0;            // next strong entry (bounds tail-call decisions)
	std::set<u32> body;          // addresses executed as standalone instructions
	std::set<u32> labels;        // addresses that need a label (branch/jump targets)
	std::map<u32, JumpTable> jumpTables;  // keyed by the jr address
	std::set<u32> tailCalls;     // j/branch addresses that leave the function
};

struct AnalysisOptions {
	std::vector<u32> hints;      // extra entry points
	bool scanDataPointers = true;
};

struct Analysis {
	std::vector<Function> functions;  // sorted by entry
	std::set<u32> strongEntries;
	std::set<u32> weakEntries;
	size_t instructions = 0;
	size_t unresolvedIndirect = 0;
};

Analysis Analyze(const ModuleImage &img, const AnalysisOptions &opts);

}  // namespace recomp
