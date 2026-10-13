// SPDX-License-Identifier: GPL-2.0-or-later
//
// C++ emission. Conventions (see allegrex/aot_abi.h):
//   * every guest function F at address A becomes `void F_A(Ctx *c)`;
//   * ALU instructions become sem_ID(c, word) calls - the same semantics as the interpreter;
//   * HI16-relocated lui instructions add g_delta (load base - recompile base);
//   * calls inside the module are direct C calls, other calls go through g_api->callIndirect;
//   * after each call, a pending non-local return either stops here or keeps propagating.
#include "recomp/codegen.h"

#include <cstdarg>
#include <filesystem>
#include <fstream>

#include "allegrex/decode.h"
#include "allegrex/insn.h"
#include "common/fileutil.h"

namespace recomp {

using ag::Kind;
using ag::Op;
using namespace insn;

namespace {

std::string Fmt(const char *fmt, ...) {
	char buf[1024];
	va_list a;
	va_start(a, fmt);
	vsnprintf(buf, sizeof(buf), fmt, a);
	va_end(a);
	return buf;
}

class FunctionEmitter {
public:
	FunctionEmitter(const ModuleImage &img, const std::set<u32> &entries, const Function &f, u32 index, bool comments)
		: img_(img), entries_(entries), f_(f), index_(index), comments_(comments) {}

	std::string Emit() {
		order_.assign(f_.body.begin(), f_.body.end());
		// Pass 1 collects goto targets, pass 2 writes them out.
		for (int pass = 0; pass < 2; pass++) {
			out_.clear();
			pending_ = 0;
			emitting_ = pass == 1;
			out_ += Fmt("void B_%08X(Ctx *PSPC_RESTRICT c, u32 ra0, u32 sp0) {\n\t(void)sp0;\n", f_.entry);
			// The guest rewrote this function (runtime sets the flag): run the new code instead.
			out_ += Fmt("\tif (PSPC_UNLIKELY(g_patched[%u])) { g_api->interpretFrom(c, %s, ra0, sp0); return; }\n",
			            index_, Pc(f_.entry).c_str());
			if (f_.body.find(f_.entry) == f_.body.end()) {
				out_ += Fmt("\tg_api->interpretFrom(c, %s, ra0, sp0);\n", Pc(f_.entry).c_str());
			} else if (order_.front() != f_.entry) {
				Goto(f_.entry, "\t");
			}
			for (size_t i = 0; i < order_.size(); i++) {
				cur_ = i;
				u32 pc = order_[i];
				if (needed_.count(pc)) {
					Charge("\t");  // the fall-through path pays for what it ran before the join
					out_ += Fmt("L_%08X:;\n", pc);
				}
				EmitInsn(pc);
			}
			out_ += "}\n";
			out_ += Fmt("void F_%08X(Ctx *PSPC_RESTRICT c) { B_%08X(c, c->r[31], c->r[29]); }\n\n", f_.entry, f_.entry);
		}
		return out_;
	}

private:
	std::string Pc(u32 a) const {
		return img_.relocatable ? Fmt("(0x%08Xu + g_delta)", a) : Fmt("0x%08Xu", a);
	}

	u32 NextEmitted() const { return cur_ + 1 < order_.size() ? order_[cur_ + 1] : 0xFFFFFFFF; }

	void Goto(u32 target, const char *indent) {
		if (!f_.body.count(target)) {
			out_ += Fmt("%sg_api->jumpIndirect(c, %s, ra0, sp0); return;\n", indent, Pc(target).c_str());
			return;
		}
		needed_.insert(target);
		out_ += Fmt("%sgoto L_%08X;\n", indent, target);
	}

	// Continue at `next` after the current instruction finished.
	void FallThrough(u32 next) {
		if (next == NextEmitted() && f_.body.count(next)) return;
		Charge("\t");
		Goto(next, "\t");
	}

	// Cycle accounting matches the interpreter: one cycle per executed instruction, delay slots
	// included. Straight-line code accumulates a count that is paid before anything that leaves
	// it (control transfers, syscalls, labels).
	void Charge(const char *indent) {
		if (pending_ == 0) return;
		out_ += Fmt("%sc->budget -= %d;\n", indent, pending_);
		pending_ = 0;
	}

	void Comment(u32 pc, u32 w) {
		if (!comments_) return;
		char dis[128];
		ag::Disassemble(pc, w, dis, sizeof(dis));
		out_ += Fmt("\t// %08X: %s\n", pc, dis);
	}

	void EmitAlu(u32 pc, u32 w, Op op, const char *indent) {
		if (img_.varImportWords.count(pc)) {
			out_ += Fmt("%sg_api->execWord(c, %s);\n", indent, Pc(pc).c_str());
			return;
		}
		if (w == 0) return;  // nop
		out_ += Fmt("%ssem_%s%s(c, 0x%08Xu);\n", indent, OpId(op), Stale(pc, w, op), w);
		if (op == Op::LUI && img_.relocatable && img_.hi16Relocs.count(pc) && Rt(w) != 0)
			out_ += Fmt("%sc->r[%u] += g_delta;\n", indent, Rt(w));
	}

	// A condition reader right behind the instruction that set the condition sees the value from
	// before it (ag::SeesStaleCondition, the same static rule the interpreter applies); the _PREV
	// semantics read that value.
	const char *Stale(u32 pc, u32 w, Op op) const {
		if (!ag::MaySeeStaleCondition(op)) return "";
		const bool stale = ag::SeesStaleCondition(op, w, img_.Read32(pc - 4), img_.Read32(pc - 8), img_.Read32(pc - 12));
		return stale ? "_PREV" : "";
	}

	// Emits the delay slot instruction at pc (part of the preceding control transfer).
	void EmitDelay(u32 pc, const char *indent) {
		u32 w = img_.Read32(pc);
		Op op = ag::Decode(w);
		switch (ag::OpKind(op)) {
		case Kind::ALU: EmitAlu(pc, w, op, indent); break;
		case Kind::SYSCALL: out_ += Fmt("%sg_api->syscall(c, 0x%08Xu, %s);\n", indent, w, Pc(pc).c_str()); break;
		case Kind::BREAK: out_ += Fmt("%sg_api->breakInsn(c, 0x%08Xu, %s);\n", indent, w, Pc(pc).c_str()); break;
		default: out_ += Fmt("%s// control transfer in delay slot at %08X ignored\n", indent, pc); break;
		}
	}

	void EmitCall(u32 target, u32 ret, const char *indent) {
		if (entries_.count(target) && !img_.IsStub(target)) {
			out_ += Fmt("%sB_%08X(c, c->r[31], c->r[29]);\n", indent, target);
		} else {
			out_ += Fmt("%sg_api->callIndirect(c, %s);\n", indent, Pc(target).c_str());
		}
		out_ += Fmt("%sif (PSPC_UNLIKELY(c->unwinding)) { if (c->pc == %s) c->unwinding = 0; else return; }\n", indent,
		            Pc(ret).c_str());
	}

	// The target continues our frame: it returns where we would (ra0), whatever $ra holds now.
	void EmitTailCall(u32 target, const char *indent) {
		if (entries_.count(target) && !img_.IsStub(target)) {
			out_ += Fmt("%sB_%08X(c, ra0, sp0); return;\n", indent, target);
		} else {
			out_ += Fmt("%sg_api->jumpIndirect(c, %s, ra0, sp0); return;\n", indent, Pc(target).c_str());
		}
	}

	void Budget(u32 pc, u32 target, const char *indent) {
		if (target <= pc) out_ += Fmt("%sif (c->budget < 0) g_api->yieldCheck(c);\n", indent);
	}

	void EmitInsn(u32 pc) {
		u32 w = img_.Read32(pc);
		Op op = ag::Decode(w);
		Kind k = ag::OpKind(op);
		Comment(pc, w);
		pending_++;
		// Branches and jumps also run their delay slot (likely branches only when taken).
		if (k == Kind::BR || k == Kind::BRAL || k == Kind::J || k == Kind::JAL || k == Kind::JR || k == Kind::JALR)
			pending_++;
		switch (k) {
		case Kind::ALU:
			EmitAlu(pc, w, op, "\t");
			FallThrough(pc + 4);
			return;
		case Kind::SYSCALL:
			Charge("\t");
			out_ += Fmt("\tg_api->syscall(c, 0x%08Xu, %s);\n\tif (c->unwinding) return;\n", w, Pc(pc).c_str());
			FallThrough(pc + 4);
			return;
		case Kind::BREAK:
			out_ += Fmt("\tg_api->breakInsn(c, 0x%08Xu, %s);\n", w, Pc(pc).c_str());
			FallThrough(pc + 4);
			return;
		case Kind::BR: {
			u32 t = BranchTarget(pc, w);
			Charge("\t");
			out_ += Fmt("\t{ const bool t_ = cond_%s%s(c, 0x%08Xu);\n", OpId(op), Stale(pc, w, op), w);
			EmitDelay(pc + 4, "\t  ");
			out_ += "\t  if (t_) {\n";
			Budget(pc, t, "\t    ");
			Goto(t, "\t    ");
			out_ += "\t  } }\n";
			FallThrough(pc + 8);
			return;
		}
		case Kind::BRL: {
			u32 t = BranchTarget(pc, w);
			Charge("\t");
			out_ += Fmt("\tif (cond_%s%s(c, 0x%08Xu)) {\n\t  c->budget -= 1;\n", OpId(op), Stale(pc, w, op), w);
			EmitDelay(pc + 4, "\t  ");
			Budget(pc, t, "\t  ");
			Goto(t, "\t  ");
			out_ += "\t}\n";
			FallThrough(pc + 8);
			return;
		}
		case Kind::BRAL:
		case Kind::BRALL: {
			u32 t = BranchTarget(pc, w);
			Charge("\t");
			out_ += Fmt("\t{ const bool t_ = cond_%s(c, 0x%08Xu);\n\t  c->r[31] = %s;\n", OpId(op), w, Pc(pc + 8).c_str());
			if (k == Kind::BRAL) {
				EmitDelay(pc + 4, "\t  ");
				out_ += "\t  if (t_) {\n";
			} else {
				out_ += "\t  if (t_) {\n\t    c->budget -= 1;\n";
				EmitDelay(pc + 4, "\t    ");
			}
			EmitCall(t, pc + 8, "\t    ");
			out_ += "\t  } }\n";
			FallThrough(pc + 8);
			return;
		}
		case Kind::J: {
			u32 t = JumpTarget(pc, w);
			Charge("\t");
			EmitDelay(pc + 4, "\t");
			if (f_.tailCalls.count(pc)) {
				EmitTailCall(t, "\t");
			} else {
				Budget(pc, t, "\t");
				Goto(t, "\t");
			}
			return;
		}
		case Kind::JAL: {
			u32 t = JumpTarget(pc, w);
			Charge("\t");
			out_ += Fmt("\tc->r[31] = %s;\n", Pc(pc + 8).c_str());
			EmitDelay(pc + 4, "\t");
			EmitCall(t, pc + 8, "\t");
			FallThrough(pc + 8);
			return;
		}
		case Kind::JR: {
			u32 rs = Rs(w);
			Charge("\t");
			out_ += Fmt("\t{ const u32 t_ = c->r[%u];\n", rs);
			EmitDelay(pc + 4, "\t  ");
			if (rs == 31) {
				out_ += "\t  if (t_ == ra0) return;\n\t  g_api->nonlocalReturn(c, t_); return; }\n";
				return;
			}
			auto jt = f_.jumpTables.find(pc);
			if (jt != f_.jumpTables.end()) {
				out_ += img_.relocatable ? "\t  switch (t_ - g_delta) {\n" : "\t  switch (t_) {\n";
				std::set<u32> seen;
				for (u32 target : jt->second.targets) {
					if (!seen.insert(target).second) continue;
					out_ += Fmt("\t  case 0x%08Xu: ", target);
					Budget(pc, target, "");
					Goto(target, "");
				}
				out_ += "\t  default: break;\n\t  }\n";
			}
			out_ += "\t  g_api->jumpIndirect(c, t_, ra0, sp0); return; }\n";
			return;
		}
		case Kind::JALR: {
			u32 rd = Rd(w);
			Charge("\t");
			out_ += Fmt("\t{ const u32 t_ = c->r[%u];\n", Rs(w));
			if (rd) out_ += Fmt("\t  c->r[%u] = %s;\n", rd, Pc(pc + 8).c_str());
			EmitDelay(pc + 4, "\t  ");
			out_ += "\t  g_api->callIndirect(c, t_);\n";
			out_ += Fmt("\t  if (PSPC_UNLIKELY(c->unwinding)) { if (c->pc == %s) c->unwinding = 0; else return; } }\n",
			            Pc(pc + 8).c_str());
			FallThrough(pc + 8);
			return;
		}
		default:
			out_ += Fmt("\tg_api->invalidInsn(c, 0x%08Xu, %s); return;\n", w, Pc(pc).c_str());
			return;
		}
	}

	static const char *OpId(Op op) {
		static const char *const ids[] = {
#define OP(id, mn, kind) #id,
#include "allegrex/ops.inc"
#include "allegrex/vfpu_ops.inc"
#undef OP
			"INVALID",
		};
		return ids[(size_t)op];
	}

	const ModuleImage &img_;
	const std::set<u32> &entries_;
	u32 index_;  // position in the function table (and g_patched)
	const Function &f_;
	bool comments_;
	std::vector<u32> order_;
	size_t cur_ = 0;
	int pending_ = 0;  // instructions run since the last budget charge
	std::set<u32> needed_;
	bool emitting_ = false;
	std::string out_;
};

}  // namespace

bool Generate(const ModuleImage &img, const Analysis &an, const CodegenOptions &opts, CodegenStats *stats,
              std::string *err) {
	std::error_code ec;
	fs::create_directories(opts.outDir, ec);
	std::set<u32> entries;
	for (const Function &f : an.functions) entries.insert(f.entry);
	std::string sha = HexString(img.sha1.data(), img.sha1.size());

	// Shared header.
	std::string hdr;
	hdr += "// Generated by pspc-recomp. Do not edit.\n#pragma once\n";
	hdr += "#include \"allegrex/sem.h\"\n#include \"allegrex/aot_abi.h\"\n\n";
	hdr += "extern const PspcRuntimeApi *g_api;\nextern u32 g_delta;\nextern u8 g_patched[];\n\n";
	for (u32 e : entries)
		hdr += Fmt("void B_%08X(Ctx *PSPC_RESTRICT c, u32 ra0, u32 sp0);\nvoid F_%08X(Ctx *PSPC_RESTRICT c);\n", e, e);
	if (!WriteTextFile(fs::path(opts.outDir) / "module.h", hdr)) {
		if (err) *err = "cannot write module.h";
		return false;
	}

	// Clang needs FENV_ACCESS for modules that change fcr31. GCC ignores that pragma and
	// relies on -frounding-math in both AOT build routes to prevent round-to-nearest folding.
	bool setsRounding = false;
	for (const auto &r : img.code)
		for (u32 pc = r.first; pc + 4 <= r.second && !setsRounding; pc += 4) {
			u32 w = img.Read32(pc);
			if (ag::Decode(w) == Op::CTC1 && Rd(w) == 31) setsRounding = true;
		}
	const char *prologue = setsRounding ? "// Generated by pspc-recomp. Do not edit.\n#pragma STDC FENV_ACCESS ON\n"
	                                    : "// Generated by pspc-recomp. Do not edit.\n";

	size_t fileIndex = 0;
	std::string body;
	auto flush = [&]() -> bool {
		if (body.empty()) return true;
		std::string text = std::string(prologue) + "#include \"module.h\"\n\n" + body;
		fs::path p = fs::path(opts.outDir) / Fmt("code_%03zu.cpp", fileIndex++);
		if (stats) stats->bytes += text.size();
		body.clear();
		return WriteTextFile(p, text);
	};
	// an.functions is sorted by entry, like `entries` (the function table order).
	u32 index = 0;
	for (const Function &f : an.functions) {
		FunctionEmitter fe(img, entries, f, index++, opts.comments);
		body += fe.Emit();
		if (body.size() >= opts.maxBytesPerFile && !flush()) {
			if (err) *err = "write failed";
			return false;
		}
	}
	if (!flush()) {
		if (err) *err = "write failed";
		return false;
	}

	// Glue: globals, function table and the entry point.
	std::string glue;
	// The semantics version comes from the header of the tree the DLL is compiled against (not the
	// one pspc-recomp was built with), so the DLL always states what it really contains.
	glue += "// Generated by pspc-recomp. Do not edit.\n#include \"module.h\"\n#include \"pspc_sem_version.h\"\n\n";
	glue += "u8 *g_membase = nullptr;\nu32 *g_cop0 = nullptr;\nu32 *g_intrEnabled = nullptr;\n";
	glue += "const PspcRuntimeApi *g_api = nullptr;\nu32 g_delta = 0;\n";
	glue += Fmt("u8 g_patched[%zu] = {};\n\n", entries.size() ? entries.size() : 1);
	glue += "static const AotFunctionEntry kFunctions[] = {\n";
	for (u32 e : entries) glue += Fmt("\t{0x%08Xu, &F_%08X, &B_%08X},\n", e - img.low, e, e);
	glue += "};\n\n";
	glue += "static AotModuleDesc g_desc = {\n";
	glue += Fmt("\tPSPC_AOT_ABI_VERSION, PSPC_SEM_VERSION, \"%s\",\n\t{", img.name.c_str());
	for (size_t i = 0; i < 20; i++) glue += Fmt("0x%02X%s", img.sha1[i], i + 1 < 20 ? ", " : "");
	glue += "},\n";
	glue += Fmt("\t0x%08Xu, %u, (u32)(sizeof(kFunctions) / sizeof(kFunctions[0])), kFunctions, &g_delta, g_patched,\n};\n\n",
	            img.low, img.relocatable ? 1u : 0u);
	glue += "extern \"C\" PSPC_EXPORT const AotModuleDesc *pspc_aot_module(const PspcRuntimeApi *api) {\n";
	glue += "\tif (!api || api->abiVersion != PSPC_AOT_ABI_VERSION || api->structSize < sizeof(PspcRuntimeApi)) return nullptr;\n";
	glue += "\tg_api = api;\n\tg_membase = api->membase;\n\tg_cop0 = api->cop0;\n\tg_intrEnabled = api->intrEnabled;\n";
	glue += "\treturn &g_desc;\n}\n";
	if (!WriteTextFile(fs::path(opts.outDir) / "glue.cpp", glue)) {
		if (err) *err = "cannot write glue.cpp";
		return false;
	}

	// Build script for the DLL.
	std::string cm;
	cm += "# Generated by pspc-recomp.\ncmake_minimum_required(VERSION 3.20)\nproject(pspc_aot CXX)\n";
	cm += "set(CMAKE_CXX_STANDARD 20)\nset(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreaded\")\n";
	cm += "file(GLOB AOT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)\n";
	cm += "add_library(aot SHARED ${AOT_SOURCES})\n";
	cm += Fmt("set_target_properties(aot PROPERTIES PREFIX \"\" OUTPUT_NAME \"%s\" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)\n", sha.c_str());
	cm += "target_include_directories(aot PRIVATE ${PSPC_SRC_DIR} ${PSPC_GEN_DIR} ${CMAKE_CURRENT_SOURCE_DIR})\n";
	cm += "target_compile_definitions(aot PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)\n";
	cm += "if(MSVC)\n\tif(CMAKE_CXX_COMPILER_ID STREQUAL \"Clang\")\n";
	cm += "\t\ttarget_compile_options(aot PRIVATE /O2 /clang:-fno-strict-aliasing -Wno-unused-label -Wno-unused-variable)\n";
	cm += "\telse()\n\t\ttarget_compile_options(aot PRIVATE /O2 /bigobj /wd4102 /wd4189 /utf-8 /permissive- /Zc:preprocessor)\n\tendif()\n";
	cm += "else()\n\ttarget_compile_options(aot PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -frounding-math)\n";
	// AOT globals belong to this module, never to the executable or another loaded module.
	cm += "\ttarget_link_options(aot PRIVATE -Wl,-Bsymbolic -Wl,--exclude-libs,ALL -Wl,--no-undefined)\nendif()\n";
	cm += "find_package(Threads REQUIRED)\n";
	cm += "target_link_libraries(aot PRIVATE \"${PSPC_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}pspc_sem${CMAKE_STATIC_LIBRARY_SUFFIX}\" \"${PSPC_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}pspc_common${CMAKE_STATIC_LIBRARY_SUFFIX}\" Threads::Threads ${CMAKE_DL_LIBS})\n";
	if (!WriteTextFile(fs::path(opts.outDir) / "CMakeLists.txt", cm)) {
		if (err) *err = "cannot write CMakeLists.txt";
		return false;
	}
	if (stats) {
		stats->files = fileIndex + 2;
		stats->functions = entries.size();
	}
	return true;
}

}  // namespace recomp
