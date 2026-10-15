// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the real module loader, CPU/dispatch bridge and interpreter without game assets.
#include "common/fileutil.h"
#include "gpu/backend.h"
#include "gpu/ge.h"
#include "gpu/hw/opengl_backend.h"
#include "runtime/core/cpu.h"
#include "runtime/core/dispatch.h"
#include "runtime/core/interp.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/sysmem.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
constexpr u32 kEntry = 0x08804000;
constexpr u32 kCallee = kEntry + 32;

void Require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

void Execute(u32 entry, u32 scratch, u32 input, u32 addend, bool native) {
	Ctx c{};
	c.r[4] = scratch;
	c.r[29] = scratch + 0x1000;
	c.budget = 1000;
	GWrite32(scratch, input);
	GWrite32(scratch + 4, 0);
	const u64 before = interp::InstructionCount();
	cpu::SetCurrentCtx(&c);
	cpu::RunFunction(&c, entry);
	cpu::SetCurrentCtx(nullptr);
	Require(GRead32(scratch + 4) == input + addend, "incorrect guest memory result");
	Require(c.r[2] == input + addend + (entry == kEntry ? 1 : 0), "incorrect return/delay slot result");
	Require(c.r[29] == scratch + 0x1000 && !c.unwinding, "guest frame did not return normally");
	const u64 interpreted = interp::InstructionCount() - before;
	Require(native ? interpreted == 0 : interpreted > 0, "unexpected native/interpreter execution path");
	std::printf("entry=%08x result=%u interpreted=%llu\n", entry, c.r[2], (unsigned long long)interpreted);
}

// Opt-in GPU integration: the real guest function supplies a different color
// each frame, via AOT or the interpreter. Both modes must retain that result at
// every render scale, including GPU writeback and full-resolution display reads.
void CheckRender(u32 scratch, bool native) {
	gpu::SetRamSize(32 * 1024 * 1024);
	gpu::Init();
	gpu::SetRenderThreads(1);
	for (int scale : {1, 2, 4, 8, 16}) {
		gpu::hw::OpenGLOptions options;
		options.scale = scale;
		std::string error;
		auto backend = gpu::hw::CreateOpenGLBackend(options, &error);
		Require(backend != nullptr, error.c_str());
		gpu::RasterState state{};
		state.throughMode = true;
		state.scissor = {0, 0, 480 * gpu::SUBPIX - 1, 272 * gpu::SUBPIX - 1};
		state.pix.fb = GPtr(0x04000000);
		state.pix.fbStride = 512;
		state.pix.fbFormat = gpu::GE_FORMAT_8888;
		state.pix.clearMode = state.pix.colorClear = state.pix.stencilClear = true;
		state.pix.depthTestFunc = gpu::GE_COMP_ALWAYS;
		gpu::BatchPrim prim{};
		prim.type = gpu::PrimType::ClearRect;
		prim.range = state.scissor;
		prim.v[1].x = 480 * gpu::SUBPIX;
		prim.v[1].y = 272 * gpu::SUBPIX;
		u32 color = 0;
		for (u32 frame = 0; frame < 3; ++frame) {
			Execute(kEntry, scratch, frame + 20, 7, native);
			color = 0xFF000000 | (GRead32(scratch + 4) * 0x010203);
			prim.v[0].color0 = prim.v[1].color0 = color;
			backend->DrawBatch(&prim, 1, &state, 1);
			backend->SyncRange(0x04000000, 512 * 272 * 4);
			Require(GRead32(0x04000000 + (136 * 512 + 240) * 4) == color, "render writeback mismatch");
		}
		std::vector<u32> image;
		int w = 0, h = 0;
		Require(backend->GetDisplayImage(0x04000000, 512, gpu::GE_FORMAT_8888, 480, 272, image, &w, &h, 0),
		        "high-resolution display image missing");
		Require(w == 480 * scale && h == 272 * scale && image.size() == (size_t)w * h, "render scale mismatch");
		for (u32 pixel : image) Require(pixel == color, "high-resolution guest result mismatch");
		std::printf("%s render %dx: %dx%d, guest-generated color and writeback passed\n",
		            native ? "AOT" : "interpreter", scale, w, h);
	}
	gpu::Shutdown();
}

void Check(const char *elfPath, const char *cache, const std::string &mode) {
	constexpr u32 ramSize = 32 * 1024 * 1024;
	Require(mem::Init(ramSize), "cannot initialize guest memory");
	kernel::InitMemoryPartitions(ramSize, false);
	cpu::Init();
	dispatch::Init();
	hle::Init();
	kernel::SetAotCacheDir(cache);
	kernel::SetAotEnabled(mode != "disabled");
	// Keep scratch/stack away from the fixed ELF at the bottom of the user partition.
	const u32 scratch = kernel::g_userMem.Alloc(0x1000, true, "aot-test");
	Require(scratch != 0, "cannot allocate guest scratch/stack");
	const u32 freeBefore = kernel::g_userMem.TotalFree();
	const u32 registeredBefore = dispatch::RegisteredCount();
	std::vector<u8> elf;
	Require(ReadWholeFile(elfPath, elf), "cannot read synthetic ELF");
	const SceUID uid = kernel::LoadModuleFromMemory(elf, elfPath, {});
	auto *module = kernel::Get<kernel::PspModule>(uid);
	Require(module && module->moduleStart == kEntry, "runtime did not load the guest module");
	const bool native = mode == "native";
	Require(module->aot == native, "runtime selected the wrong module execution mode");
	Require((dispatch::Lookup(kEntry) != nullptr) == native &&
	        (dispatch::LookupBody(kCallee) != nullptr) == native, "incorrect native registration");
	Execute(kEntry, scratch, 20, 7, native);
	if (std::getenv("PSPC_AOT_RENDER_TEST") && (native || mode == "disabled"))
		CheckRender(scratch, native);

	if (native) {
		Require(kernel::InvalidateNativeCode(kEntry, module->memSize) == 0,
		        "unchanged code invalidated a native function");
		const HostFn caller = dispatch::Lookup(kEntry);
		GWrite32(kCallee + 4, 0x25020011);  // addiu v0,t0,17 instead of 7
		Require(kernel::InvalidateNativeCode(kCallee + 4, 4) == 1, "patched callee was not invalidated");
		Require(dispatch::Lookup(kEntry) == caller && dispatch::LookupBody(kEntry), "caller was unnecessarily invalidated");
		Require(!dispatch::Lookup(kCallee) && !dispatch::LookupBody(kCallee), "patched callee is still registered");
		// The caller is still native: its direct callee entry must hand off to the real interpreter.
		Execute(kEntry, scratch, 30, 17, false);
		// A dispatch miss must see the same patched code, not stale generated instructions.
		Execute(kCallee, scratch, 40, 17, false);
		Require(kernel::InvalidateNativeCode(kCallee + 4, 4) == 0, "repeated invalidation dropped code twice");
	}

	Require(kernel::UnloadModule(module) == 0, "module unload failed");
	Require(!kernel::Get<kernel::PspModule>(uid) && !kernel::ModuleByAddress(kEntry), "unloaded module remains visible");
	Require(!dispatch::Lookup(kEntry) && !dispatch::LookupBody(kEntry) &&
	        !dispatch::Lookup(kCallee) && !dispatch::LookupBody(kCallee), "unload left native function pointers");
	Require(dispatch::RegisteredCount() == registeredBefore, "unload leaked dispatch entries");
	Require(kernel::g_userMem.TotalFree() == freeBefore, "unload leaked guest memory");
	Require(mem::BadAccessCount() == 0, "bad guest memory access");
	kernel::g_userMem.Free(scratch);
	kernel::DestroyAll();
	mem::Shutdown();
	std::printf("%s: runtime loading/execution/unload passed\n", mode.c_str());
}
}  // namespace

int main(int argc, char **argv) {
	if (argc != 4) {
		std::fprintf(stderr, "usage: pspc_aot_runtime_probe <elf> <cache> <native|disabled|missing|mismatched>\n");
		return 2;
	}
	const std::string mode = argv[3];
	if (mode != "native" && mode != "disabled" && mode != "missing" && mode != "mismatched") return 2;
	try {
		Check(argv[1], argv[2], mode);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "Runtime AOT probe: %s\n", e.what());
		return 1;
	}
}
