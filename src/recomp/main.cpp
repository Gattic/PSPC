// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc-recomp: statically recompile one PSP module (plain ELF/PRX) to C++.
//
//   pspc-recomp <module.elf|prx> -o <outdir> [--base 0x08804000] [--hints file] [--no-comments]
//
// The output directory gets module.h, glue.cpp, code_NNN.cpp and a CMakeLists.txt that builds
// <sha1>.dll against PSPC's headers (-DPSPC_SRC_DIR=...) and libraries (-DPSPC_LIB_DIR=...).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "common/fileutil.h"
#include "loader/psp_header.h"
#include "recomp/analysis.h"
#include "recomp/codegen.h"
#include "recomp/image.h"

static int Usage() {
	fprintf(stderr,
	        "usage: pspc-recomp <module.elf|prx> -o <outdir> [--base <hex>] [--hints <file>] [--no-comments] [--info]\n");
	return 1;
}

int main(int argc, char **argv) {
	std::string input, outDir, hintsFile;
	u32 base = 0x08804000;
	bool comments = true, infoOnly = false;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "-o" && i + 1 < argc) outDir = argv[++i];
		else if (a == "--base" && i + 1 < argc) base = (u32)strtoul(argv[++i], nullptr, 16);
		else if (a == "--hints" && i + 1 < argc) hintsFile = argv[++i];
		else if (a == "--no-comments") comments = false;
		else if (a == "--info") infoOnly = true;
		else if (!a.empty() && a[0] != '-') input = a;
		else return Usage();
	}
	if (input.empty() || (outDir.empty() && !infoOnly)) return Usage();

	std::vector<u8> data;
	if (!ReadWholeFile(fs::u8path(input), data)) {
		fprintf(stderr, "cannot read %s\n", input.c_str());
		return 2;
	}
	if (loader::Classify(data.data(), data.size()) != loader::ContainerKind::PlainElf) {
		fprintf(stderr, "%s is not a plain ELF/PRX (decrypt it with pspc-decrypt first)\n", input.c_str());
		return 2;
	}
	recomp::ModuleImage img;
	std::string err;
	if (!img.Load(data, base, &err)) {
		fprintf(stderr, "%s: %s\n", input.c_str(), err.c_str());
		return 2;
	}
	recomp::AnalysisOptions aopts;
	if (!hintsFile.empty()) {
		std::ifstream in(hintsFile);
		std::string line;
		while (std::getline(in, line)) {
			if (line.empty() || line[0] == '#') continue;
			aopts.hints.push_back((u32)strtoul(line.c_str(), nullptr, 16));
		}
	}
	recomp::Analysis an = recomp::Analyze(img, aopts);
	printf("module %s sha1 %s\n", img.name.c_str(), HexString(img.sha1.data(), 20).c_str());
	printf("  image %08x-%08x (%s), %zu code ranges, %zu relocations\n", img.low, img.high,
	       img.relocatable ? "relocatable" : "fixed", img.code.size(), img.image.relocs.size());
	printf("  %zu functions (%zu strong, %zu weak entries), %zu instructions, %zu unresolved indirect jumps\n",
	       an.functions.size(), an.strongEntries.size(), an.weakEntries.size(), an.instructions,
	       an.unresolvedIndirect);
	if (infoOnly) return 0;

	recomp::CodegenOptions copts;
	copts.outDir = outDir;
	copts.comments = comments;
	recomp::CodegenStats st;
	if (!recomp::Generate(img, an, copts, &st, &err)) {
		fprintf(stderr, "codegen failed: %s\n", err.c_str());
		return 2;
	}
	printf("  wrote %zu files, %.1f MB of C++ to %s\n", st.files, st.bytes / 1048576.0, outDir.c_str());
	return 0;
}
