// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "recomp/analysis.h"
#include "recomp/image.h"

namespace recomp {

struct CodegenOptions {
	std::string outDir;
	size_t maxBytesPerFile = 1u << 20;  // split generated code into ~1 MB translation units
	bool comments = true;               // disassembly comments
};

struct CodegenStats {
	size_t files = 0;
	size_t functions = 0;
	size_t bytes = 0;
};

bool Generate(const ModuleImage &img, const Analysis &an, const CodegenOptions &opts, CodegenStats *stats,
              std::string *err);

}  // namespace recomp
