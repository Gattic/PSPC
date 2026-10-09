// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// The display list queue and command processor: GPU/GPUCommon.cpp's list state machine (see
// reference/ppsspp/docs/sceGe.md), Core/HLE/sceGe.cpp's interrupt handling, and the software GPU's
// command handlers (GPU/Software/SoftGpu.cpp).
#include <algorithm>
#include <cstring>
#include <deque>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "gpu/color.h"
#include "gpu/ge.h"
#include "gpu/ge_state.h"
#include "gpu/render.h"
#include "gpu/vertex.h"

namespace gpu {

u32 g_ramSize = 0x04000000;

namespace {

constexpr int MAX_LISTS = 64;
constexpr u32 LIST_ID_MAGIC = 0x35000000;

constexpr u32 ERR_ALREADY = 0x80000020;
constexpr u32 ERR_BUSY = 0x80000021;
constexpr u32 ERR_OUT_OF_MEMORY = 0x80000022;
constexpr u32 ERR_PRIV_REQUIRED = 0x80000023;
constexpr u32 ERR_INVALID_ID = 0x80000100;
constexpr u32 ERR_INVALID_INDEX = 0x80000102;
constexpr u32 ERR_INVALID_POINTER = 0x80000103;
constexpr u32 ERR_INVALID_SIZE = 0x80000104;
constexpr u32 ERR_INVALID_MODE = 0x80000107;
constexpr u32 ERR_INVALID_VALUE = 0x800001FE;

enum GPURunState { GPUSTATE_RUNNING, GPUSTATE_DONE, GPUSTATE_STALL, GPUSTATE_INTERRUPT, GPUSTATE_ERROR };

struct StackEntry {
	u32 pc;
	u32 offsetAddr;
	u32 baseAddr;
};

struct DisplayList {
	int id;
	u32 startpc;
	u32 pc;
	u32 stall;
	DisplayListState state;
	SignalBehavior signal;
	int callbackId;
	u16 subIntrToken;
	StackEntry stack[32];
	int stackptr;
	bool interrupted;
	bool waitPending;  // threads in sceGeListSync(id, 0) wait
	bool interruptsEnabled;
	bool pendingInterrupt;
	bool started;
	u32 context;
	u32 offsetAddr;
	bool bboxResult;
	u32 stackAddr;
};

struct PendingInterrupt {
	int listid;
	u32 pc;
	u32 cmd;
};

DisplayList dls[MAX_LISTS];
std::deque<int> dlQueue;
DisplayList *currentList = nullptr;
int nextListID = 0;
bool interruptRunning = false;
bool isbreak = false;
GPURunState gpuState = GPUSTATE_DONE;
bool drawPending = false;
u32 sdkVersion = 0x06060010;
Hooks hooks;
std::deque<PendingInterrupt> pendingIntrs;
bool cpuInterruptsEnabled = true;
bool inRun = false;
bool rerun = false;
u32 intrListPc = 0;
bool asyncTaken = false;
u32 edramTranslation = 0x400;

// The CLUT as LOADCLUT leaves it (a partial load keeps the rest).
u8 clutData[1024];

// Immediate mode vertices (VAP).
RVertex immVerts[64];
float immW[64];
int immCount = 0;
GEPrimitiveType immPrim = GE_PRIM_INVALID;
u32 immFlags = 0;
bool immFirstSent = false;

enum : u8 {
	CF_RASTER = 1,
	CF_TRANSFORM = 2,
	CF_TARGET = 4,
};
u8 cmdFlags[256];

VertexFormat drawFmt;

inline u32 ReadListWord(u32 pc) {
	return GRead32(FoldAddress(pc));
}

void InitCmdFlags() {
	memset(cmdFlags, CF_RASTER | CF_TRANSFORM, sizeof(cmdFlags));
	static const u8 none[] = {
		GE_CMD_NOP, GE_CMD_VADDR, GE_CMD_IADDR, 0x03, GE_CMD_PRIM, GE_CMD_BEZIER, GE_CMD_SPLINE, GE_CMD_BOUNDINGBOX,
		GE_CMD_JUMP, GE_CMD_BJUMP, GE_CMD_CALL, GE_CMD_RET, GE_CMD_END, 0x0D, GE_CMD_SIGNAL, GE_CMD_FINISH, GE_CMD_BASE,
		0x11, GE_CMD_OFFSETADDR, GE_CMD_ORIGIN, GE_CMD_PATCHCULLENABLE, 0x29, GE_CMD_BONEMATRIXNUMBER, GE_CMD_BONEMATRIXDATA,
		0x34, 0x35, GE_CMD_PATCHDIVISION, GE_CMD_PATCHPRIMITIVE, GE_CMD_PATCHFACING, 0x39,
		GE_CMD_WORLDMATRIXNUMBER, GE_CMD_VIEWMATRIXNUMBER, GE_CMD_PROJMATRIXNUMBER, GE_CMD_TGENMATRIXNUMBER,
		GE_CMD_WORLDMATRIXDATA, GE_CMD_VIEWMATRIXDATA, GE_CMD_PROJMATRIXDATA, GE_CMD_TGENMATRIXDATA,
		0x4E, 0x4F, 0x52, 0x59, 0x5A, GE_CMD_CULL, GE_CMD_CLUTADDR, GE_CMD_CLUTADDRUPPER,
		GE_CMD_TRANSFERSRC, GE_CMD_TRANSFERSRCW, GE_CMD_TRANSFERDST, GE_CMD_TRANSFERDSTW, 0xB6, 0xB7, GE_CMD_LOADCLUT,
		GE_CMD_TEXFLUSH, GE_CMD_TEXSYNC, 0xD1, GE_CMD_TRANSFERSTART, GE_CMD_TRANSFERSRCPOS, GE_CMD_TRANSFERDSTPOS, 0xED,
		GE_CMD_TRANSFERSIZE, 0xEF, GE_CMD_VSCX, GE_CMD_VSCY, GE_CMD_VSCZ, GE_CMD_VTCS, GE_CMD_VTCT, GE_CMD_VTCQ, GE_CMD_VCV,
		GE_CMD_VAP, GE_CMD_VFC, GE_CMD_VSCV, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
	};
	for (u8 c : none)
		cmdFlags[c] = 0;
	for (int c = GE_CMD_MORPHWEIGHT0; c <= GE_CMD_MORPHWEIGHT7; ++c)
		cmdFlags[c] = 0;
	// Only the transform reads these.
	static const u8 transformOnly[] = {
		GE_CMD_LIGHTINGENABLE, GE_CMD_LIGHTENABLE0, GE_CMD_LIGHTENABLE1, GE_CMD_LIGHTENABLE2, GE_CMD_LIGHTENABLE3,
		GE_CMD_DEPTHCLIPENABLE, GE_CMD_VIEWPORTXSCALE, GE_CMD_VIEWPORTYSCALE, GE_CMD_VIEWPORTZSCALE, GE_CMD_VIEWPORTXCENTER,
		GE_CMD_VIEWPORTYCENTER, GE_CMD_VIEWPORTZCENTER, GE_CMD_TEXSCALEU, GE_CMD_TEXSCALEV, GE_CMD_TEXOFFSETU,
		GE_CMD_TEXOFFSETV, GE_CMD_OFFSETX, GE_CMD_OFFSETY, GE_CMD_REVERSENORMAL, GE_CMD_MATERIALUPDATE,
		GE_CMD_MATERIALEMISSIVE, GE_CMD_MATERIALAMBIENT, GE_CMD_MATERIALDIFFUSE, GE_CMD_MATERIALSPECULAR,
		GE_CMD_MATERIALALPHA, GE_CMD_MATERIALSPECULARCOEF, GE_CMD_AMBIENTCOLOR, GE_CMD_AMBIENTALPHA, GE_CMD_LIGHTMODE,
		GE_CMD_TEXSHADELS, GE_CMD_FOG1, GE_CMD_FOG2,
	};
	for (u8 c : transformOnly)
		cmdFlags[c] = CF_TRANSFORM;
	for (int c = GE_CMD_LIGHTTYPE0; c <= GE_CMD_LSC3; ++c)
		cmdFlags[c] = CF_TRANSFORM;
	// Only the raster state.
	static const u8 rasterOnly[] = {
		GE_CMD_REGION1, GE_CMD_REGION2, GE_CMD_DITHERENABLE, GE_CMD_ALPHABLENDENABLE, GE_CMD_ALPHATESTENABLE,
		GE_CMD_ZTESTENABLE, GE_CMD_STENCILTESTENABLE, GE_CMD_ANTIALIASENABLE, GE_CMD_COLORTESTENABLE, GE_CMD_LOGICOPENABLE,
		GE_CMD_SHADEMODE, GE_CMD_TEXMODE, GE_CMD_TEXFORMAT, GE_CMD_CLUTFORMAT, GE_CMD_TEXFILTER, GE_CMD_TEXWRAP,
		GE_CMD_TEXLEVEL, GE_CMD_TEXFUNC, GE_CMD_TEXENVCOLOR, GE_CMD_FOGCOLOR, GE_CMD_TEXLODSLOPE, GE_CMD_SCISSOR1,
		GE_CMD_SCISSOR2, GE_CMD_MINZ, GE_CMD_MAXZ, GE_CMD_COLORTEST, GE_CMD_COLORREF, GE_CMD_COLORTESTMASK,
		GE_CMD_ALPHATEST, GE_CMD_STENCILTEST, GE_CMD_STENCILOP, GE_CMD_ZTEST, GE_CMD_BLENDMODE, GE_CMD_BLENDFIXEDA,
		GE_CMD_BLENDFIXEDB, GE_CMD_LOGICOP, GE_CMD_ZWRITEDISABLE, GE_CMD_MASKRGB, GE_CMD_MASKALPHA,
	};
	for (u8 c : rasterOnly)
		cmdFlags[c] = CF_RASTER;
	for (int c = GE_CMD_TEXADDR0; c <= GE_CMD_TEXBUFWIDTH7; ++c)
		cmdFlags[c] = CF_RASTER;
	for (int c = GE_CMD_TEXSIZE0; c <= GE_CMD_TEXSIZE7; ++c)
		cmdFlags[c] = CF_RASTER;
	for (int c = GE_CMD_DITH0; c <= GE_CMD_DITH3; ++c)
		cmdFlags[c] = CF_RASTER;
	cmdFlags[GE_CMD_FRAMEBUFPTR] = CF_RASTER | CF_TARGET;
	cmdFlags[GE_CMD_FRAMEBUFWIDTH] = CF_RASTER | CF_TARGET;
	cmdFlags[GE_CMD_FRAMEBUFPIXFORMAT] = CF_RASTER | CF_TARGET;
	cmdFlags[GE_CMD_ZBUFPTR] = CF_RASTER | CF_TARGET;
	cmdFlags[GE_CMD_ZBUFWIDTH] = CF_RASTER | CF_TARGET;
	// Both: vertex type (through mode), texture/fog enables, clear mode, UV gen mode.
	cmdFlags[GE_CMD_VERTEXTYPE] = CF_RASTER | CF_TRANSFORM;
	cmdFlags[GE_CMD_TEXTUREMAPENABLE] = CF_RASTER | CF_TRANSFORM;
	cmdFlags[GE_CMD_FOGENABLE] = CF_RASTER | CF_TRANSFORM;
	cmdFlags[GE_CMD_CLEARMODE] = CF_RASTER | CF_TRANSFORM;
	cmdFlags[GE_CMD_TEXMAPMODE] = CF_RASTER | CF_TRANSFORM;
	cmdFlags[GE_CMD_CULLFACEENABLE] = 0;
}

// --- Display list queue (GPUCommon) ---

void PopDLQueue() {
	if (!dlQueue.empty()) {
		dlQueue.pop_front();
		if (!dlQueue.empty()) {
			const bool running = currentList && currentList->state == DL_STATE_RUNNING;
			currentList = &dls[dlQueue.front()];
			if (running)
				currentList->state = DL_STATE_RUNNING;
		} else {
			currentList = nullptr;
		}
	}
}

void RemoveFromQueue(int id) {
	dlQueue.erase(std::remove(dlQueue.begin(), dlQueue.end(), id), dlQueue.end());
}

void LoadClut();

void ReapplyGfxState() {
	binner::TargetChanged();
	binner::DirtyState();
	TransformDirty();
	for (int i = 0; i < 8; ++i)
		gstate_c.morphWeights[i] = getFloat24(gstate.cmdmem[GE_CMD_MORPHWEIGHT0 + i]);
	LoadClut();
}

void SaveContextTo(u32 addr) {
	if (ValidBytesAt(addr) >= 512 * 4)
		SaveGEState((u32 *)GuestPtr(FoldAddress(addr)));
}

void RestoreContextFrom(u32 addr) {
	if (ValidBytesAt(addr) >= 512 * 4) {
		binner::Flush();
		RestoreGEState((const u32 *)GuestPtr(FoldAddress(addr)));
		ReapplyGfxState();
	}
}

void NotifyDrawSyncDone() {
	drawPending = false;
	if (hooks.onDrawSyncDone)
		hooks.onDrawSyncDone(hooks.user);
}

void NotifyListDone(DisplayList &dl) {
	dl.waitPending = false;
	if (hooks.onListDone)
		hooks.onListDone(hooks.user, (int)(LIST_ID_MAGIC ^ (u32)dl.id));
}

void CompleteFailedList(DisplayList &list) {
	if (list.started && list.context) {
		RestoreContextFrom(list.context);
		list.started = false;
	}
	list.state = DL_STATE_COMPLETED;
	NotifyListDone(list);
}

void TriggerInterrupt(DisplayList &list) {
	const u32 pc = list.pc;
	pendingIntrs.push_back({ list.id, pc, ReadListWord(pc - 4) >> 24 });
	list.pendingInterrupt = true;
	if (hooks.onInterruptPending)
		hooks.onInterruptPending(hooks.user);
}

void InterruptEnd(int listid) {
	interruptRunning = false;
	isbreak = false;
	DisplayList &dl = dls[listid];
	dl.pendingInterrupt = false;
	if (dl.state == DL_STATE_COMPLETED || dl.state == DL_STATE_NONE) {
		if (dl.started && dl.context)
			RestoreContextFrom(dl.context);
		if (!dlQueue.empty()) {
			if (listid == dlQueue.front())
				PopDLQueue();
			else
				RemoveFromQueue(listid);
		}
		// If that was the last list, sceGeDrawSync waiters are woken before the ones of this list.
		if (dlQueue.empty() && dl.state == DL_STATE_COMPLETED)
			NotifyDrawSyncDone();
		NotifyListDone(dl);
	}
}

// --- Command handlers ---

u32 RelAddr(u32 data) {
	return gstate_c.getRelativeAddress(data, gstate.base);
}

void AdvanceVerts(u32 vertType, int count, int bytesRead) {
	if ((vertType & GE_VTYPE_IDX_MASK) != 0) {
		const int indexShift = ((vertType & GE_VTYPE_IDX_MASK) >> 11) - 1;
		gstate_c.indexAddr += count << indexShift;
	} else {
		gstate_c.vertexAddr += bytesRead;
	}
}

// Whether a draw's vertices and indices are readable; sets the pointers.
bool DrawPointers(u32 vertType, int count, int maxVertexCount, const u8 **verts, const u8 **inds) {
	if (drawFmt.vtype != vertType)
		drawFmt.Set(vertType);
	*inds = nullptr;
	const u32 vaddr = gstate_c.vertexAddr;
	if (!IsValidAddress(vaddr)) {
		LOGW("gpu", "bad vertex address %08x", vaddr);
		return false;
	}
	int vertsNeeded = maxVertexCount;
	if ((vertType & GE_VTYPE_IDX_MASK) != 0) {
		const u32 iaddr = gstate_c.indexAddr;
		const int idxType = (vertType >> 11) & 3;
		const u32 idxBytes = (u32)count << (idxType - 1);
		if (ValidBytesAt(iaddr) < idxBytes) {
			LOGW("gpu", "bad index address %08x", iaddr);
			return false;
		}
		*inds = GuestPtr(FoldAddress(iaddr));
		int lo, hi;
		GetIndexBounds(*inds, count, idxType, &lo, &hi);
		vertsNeeded = hi + 1;
	}
	if ((u64)ValidBytesAt(vaddr) < (u64)vertsNeeded * drawFmt.size) {
		LOGW("gpu", "vertex data out of range %08x (%d x %d)", vaddr, vertsNeeded, drawFmt.size);
		return false;
	}
	*verts = GuestPtr(FoldAddress(vaddr));
	return true;
}

void FlushImm();

void Execute_Prim(u32 op) {
	const int count = op & 0xFFFF;
	if (count == 0)
		return;
	const GEPrimitiveType prim = (GEPrimitiveType)((op >> 16) & 7);
	FlushImm();
	const u32 vertType = gstate.vertType;
	const u8 *verts, *inds;
	if (!DrawPointers(vertType, count, count, &verts, &inds))
		return;
	SubmitPrimitive(verts, inds, prim, count, vertType);
	// The vertex (or index) address advances; VADDR/IADDR stay.
	AdvanceVerts(vertType, count, count * drawFmt.size);
}

void Execute_Curve(u32 op, bool spline) {
	const int nu = op & 0xFF, nv = (op >> 8) & 0xFF;
	const int count = nu * nv;
	const u32 vertType = gstate.vertType;
	const u8 *verts, *inds;
	if (count == 0 || !DrawPointers(vertType, count, count, &verts, &inds))
		return;
	if (spline)
		SubmitSpline(verts, inds, op, vertType);
	else
		SubmitBezier(verts, inds, op, vertType);
	AdvanceVerts(vertType, count, count * drawFmt.size);
}

void Execute_BoundingBox(u32 op) {
	const int count = op & 0xFFFF;
	if (count == 0) {
		currentList->bboxResult = false;
		return;
	}
	const u32 vertType = gstate.vertType;
	if (drawFmt.vtype != vertType)
		drawFmt.Set(vertType);
	const bool useInds = (vertType & GE_VTYPE_IDX_MASK) != 0;
	const int bytesRead = (useInds ? 1 : drawFmt.size) * count;
	const u8 *verts, *inds;
	if (!DrawPointers(vertType, count, count, &verts, &inds)) {
		// Data seems invalid: assume the box test passed.
		currentList->bboxResult = true;
		return;
	}
	// The PSP only varies the result based on a single range of 0x100.
	if (count > 0x200) {
		const size_t skip = (size_t)(count - 0x200) * drawFmt.size;
		currentList->bboxResult = TestBoundingBox(verts + skip, inds, 0x100, vertType);
	} else if (count > 0x100) {
		currentList->bboxResult = TestBoundingBox(verts, inds, count - 0x100, vertType);
	} else {
		currentList->bboxResult = TestBoundingBox(verts, inds, count, vertType);
	}
	AdvanceVerts(vertType, count, bytesRead);
}

void LoadClut() {
	const u32 clutAddr = gstate.getClutAddress();
	u32 bytes = (u32)(gstate.getClutLoadBlocks() & 0x3F) * 32;
	if (bytes > 1024)
		bytes = 1024;
	if (bytes == 0)
		return;
	binner::FlushIfOverlap(clutAddr, bytes, false);
	const u32 valid = ValidBytesAt(clutAddr);
	if (valid) {
		const u32 n = std::min(valid, bytes);
		memcpy(clutData, GuestPtr(FoldAddress(clutAddr)), n);
		if (n < bytes)
			memset(clutData + n, 0, bytes - n);
	} else if (clutAddr != 0) {
		memset(clutData, 0, bytes);
	}
	binner::UpdateClut(clutData);
}

// One row of a block transfer whose source and destination overlap, as the GE copies it (ppsspp-re
// geprobe exp219, exp220): whole aligned 16-byte source blocks, four at a time, through four slots.
void BlockTransferOverlappingRow(u32 srcAddr, u32 dstAddr, u32 bytes) {
	struct Slot {
		u32 block;
		bool used;
		u16 written;
		u8 data[16];
	};
	Slot slots[4]{};
	auto writeOut = [](const Slot &slot) {
		if (!slot.used || !slot.written)
			return;
		u8 *p = GuestPtr(slot.block << 4);
		for (int k = 0; k < 16; ++k) {
			if (slot.written & (1 << k))
				p[k] = slot.data[k];
		}
	};
	const u32 firstBlock = srcAddr >> 4, lastBlock = (srcAddr + bytes - 1) >> 4;
	const u32 delta = dstAddr - srcAddr;
	for (u32 group = firstBlock; group <= lastBlock; group += 4) {
		const u32 count = std::min(4u, lastBlock - group + 1);
		u8 read[4][16];
		for (u32 i = 0; i < count; ++i) {
			const u32 block = group + i;
			const Slot &slot = slots[block & 3];
			if (slot.used && slot.block == block && slot.written)
				memcpy(read[i], slot.data, 16);
			else
				memcpy(read[i], GuestPtr(block << 4), 16);
		}
		for (u32 i = 0; i < count; ++i) {
			for (u32 k = 0; k < 16; ++k) {
				const u32 addr = ((group + i) << 4) + k;
				const u32 dst = addr + delta;
				Slot &slot = slots[(dst >> 4) & 3];
				if (!slot.used || slot.block != dst >> 4) {
					writeOut(slot);
					slot.used = true;
					slot.block = dst >> 4;
					slot.written = 0;
				}
				slot.data[dst & 15] = read[i][k];
				if (addr - srcAddr < bytes)
					slot.written |= 1 << (dst & 15);
			}
		}
	}
	for (const Slot &slot : slots)
		writeOut(slot);
}

// Folds an address that ran past VRAM's end back to its start (the transfer wraps there).
inline u32 WrapVram(u32 a) {
	if ((a & 0x0F800000) == 0x04800000)
		a &= ~0x00800000u;
	return a;
}

void Execute_BlockTransfer() {
	g_stats.transfers++;
	u32 srcBase = gstate.getTransferSrcAddress() & 0x0FFFFFFF;
	const u32 srcStride = gstate.getTransferSrcStride();
	u32 dstBase = gstate.getTransferDstAddress() & 0x0FFFFFFF;
	const u32 dstStride = gstate.getTransferDstStride();
	const int srcX = gstate.getTransferSrcX(), srcY = gstate.getTransferSrcY();
	const int dstX = gstate.getTransferDstX(), dstY = gstate.getTransferDstY();
	const int width = gstate.getTransferWidth(), height = gstate.getTransferHeight();
	const int bpp = gstate.getTransferBpp();
	srcBase = WrapVram(srcBase);
	dstBase = WrapVram(dstBase);

	const u32 bytesPerRow = (u32)width * bpp;
	const u32 src = srcBase + (u32)(srcY * srcStride + srcX) * bpp;
	const u32 dst = dstBase + (u32)(dstY * dstStride + dstX) * bpp;
	const u32 srcSize = (u32)(height - 1) * srcStride * bpp + bytesPerRow;
	const u32 dstSize = (u32)(height - 1) * dstStride * bpp + bytesPerRow;
	binner::FlushIfOverlap(src, srcSize, false);
	binner::FlushIfOverlap(dst, dstSize, true);

	for (int y = 0; y < height; ++y) {
		const u32 s = WrapVram(srcBase + (u32)((y + srcY) * srcStride + srcX) * bpp);
		const u32 d = WrapVram(dstBase + (u32)((y + dstY) * dstStride + dstX) * bpp);
		const u32 sv = ValidBytesAt(s), dv = ValidBytesAt(d);
		if (sv >= bytesPerRow && dv >= bytesPerRow) {
			const u32 fs = FoldAddress(s), fd = FoldAddress(d);
			const bool overlap = fs < fd + bytesPerRow && fd < fs + bytesPerRow;
			if (overlap && fs != fd)
				BlockTransferOverlappingRow(fs, fd, bytesPerRow);
			else
				memmove(GuestPtr(fd), GuestPtr(fs), bytesPerRow);
		} else if (sv && dv) {
			// Wrapping inside the row (VRAM): byte by byte.
			for (u32 i = 0; i < bytesPerRow; ++i) {
				const u32 sa = WrapVram(s + i), da = WrapVram(d + i);
				if (IsValidAddress(sa) && IsValidAddress(da))
					*GuestPtr(FoldAddress(da)) = *GuestPtr(FoldAddress(sa));
			}
		}
	}
}

// --- Matrices (SoftGPU) ---

void Execute_MtxData(u32 op, u32 &numReg, u32 &dataReg, float *mtx, int size, u32 *visible, int visibleBase, int cmdNum, int cmdData) {
	const int num = numReg & 0x00FFFFFF;
	if (num < size) {
		u32 *target = (u32 *)&mtx[num];
		const u32 newVal = op << 8;
		if (newVal != *target) {
			binner::Flush();
			*target = newVal;
			TransformDirty();
			binner::DirtyState();
		}
	}
	// The CPU visible values update differently.
	visible[visibleBase + (num & 0xF)] = op & 0x00FFFFFF;
	numReg = ((u32)cmdNum << 24) | ((num + 1) & 0x00FFFFFF);
	dataReg = (u32)cmdData << 24;
}

// --- Immediate vertices (GPUCommon::Execute_ImmVertexAlphaPrim) ---

void FlushImm() {
	if (immCount == 0 || immPrim == GE_PRIM_INVALID)
		return;
	// Immediate points at 0,0 with no color are leftovers of clearing the state.
	if (immCount == 1 && immPrim == GE_PRIM_POINTS && immVerts[0].x == 0 && immVerts[0].y == 0 && immVerts[0].z == 0 && immVerts[0].color0 == 0) {
		immCount = 0;
		return;
	}
	// Flags from the first prim override the state for this draw.
	const u32 saved[6] = { gstate.antiAliasEnable, gstate.shademodel, gstate.cullfaceEnable, gstate.textureMapEnable, gstate.fogEnable, gstate.ditherEnable };
	const u32 savedCull = gstate.cullmode;
	gstate.antiAliasEnable = ((u32)GE_CMD_ANTIALIASENABLE << 24) | ((immFlags & GE_IMM_ANTIALIAS) ? 1 : 0);
	gstate.shademodel = ((u32)GE_CMD_SHADEMODE << 24) | ((immFlags & GE_IMM_SHADING) ? 1 : 0);
	gstate.cullfaceEnable = ((u32)GE_CMD_CULLFACEENABLE << 24) | ((immFlags & GE_IMM_CULLENABLE) ? 1 : 0);
	gstate.textureMapEnable = ((u32)GE_CMD_TEXTUREMAPENABLE << 24) | ((immFlags & GE_IMM_TEXTURE) ? 1 : 0);
	gstate.fogEnable = ((u32)GE_CMD_FOGENABLE << 24) | ((immFlags & GE_IMM_FOG) ? 1 : 0);
	gstate.ditherEnable = ((u32)GE_CMD_DITHERENABLE << 24) | ((immFlags & GE_IMM_DITHER) ? 1 : 0);
	const int cullMode = (immFlags & GE_IMM_CULLFACE) ? 1 : 0;
	gstate.cullmode = (gstate.cullmode & ~1u) | (u32)cullMode;
	binner::DirtyState();
	TransformDirty();

	for (int i = 0; i < immCount; ++i)
		SubmitImmVertex(immPrim, immVerts[i], immW[i], !immFirstSent && i == 0);
	immCount = 0;
	immFirstSent = true;

	gstate.antiAliasEnable = saved[0];
	gstate.shademodel = saved[1];
	gstate.cullfaceEnable = saved[2];
	gstate.textureMapEnable = saved[3];
	gstate.fogEnable = saved[4];
	gstate.ditherEnable = saved[5];
	gstate.cullmode = savedCull;
	binner::DirtyState();
	TransformDirty();
}

void Execute_ImmVertex(u32 op) {
	if (immCount >= 64)
		FlushImm();
	const int prim = (op >> 8) & 0x7;
	if (prim != GE_PRIM_KEEP_PREVIOUS)
		FlushImm();
	RVertex &v = immVerts[immCount];
	const bool through = gstate.isModeThrough();
	float x, y;
	if (through) {
		x = ((int)(gstate.imm_vscx & 0xFFFF) - 0x8000) / 16.0f;
		y = ((int)(gstate.imm_vscy & 0xFFFF) - 0x8000) / 16.0f;
	} else {
		x = ((int)(gstate.imm_vscx & 0xFFFF) - gstate.getOffsetX16()) / 16.0f;
		y = ((int)(gstate.imm_vscy & 0xFFFF) - gstate.getOffsetY16()) / 16.0f;
	}
	v.x = (int)(x * 16.0f);
	v.y = (int)(y * 16.0f);
	v.z = (int)(gstate.imm_vscz & 0xFFFF);
	v.s = getFloat24(gstate.imm_vtcs);
	v.t = getFloat24(gstate.imm_vtct);
	v.q = getFloat24(gstate.imm_vtcq);
	if (through) {
		v.s *= (float)gstate.getTextureWidth(0);
		v.t *= (float)gstate.getTextureHeight(0);
	}
	v.color0 = (gstate.imm_cv & 0xFFFFFF) | (gstate.imm_ap << 24);
	v.fog = (gstate.imm_fc & 0xFF) / 255.0f;
	v.color1 = gstate.isUsingSecondaryColor() && !through ? (gstate.imm_scv & 0xFFFFFF) : 0;
	v.clipw = 1.0f;
	immW[immCount] = 1.0f;
	immCount++;
	if (prim != GE_PRIM_KEEP_PREVIOUS) {
		immPrim = (GEPrimitiveType)prim;
		// Flags are only respected from the first prim.
		immFlags = op & 0x00FFF800;
		immFirstSent = false;
	}
	// Drawn right away (as the software renderer does).
	FlushImm();
}

// --- END: signals and finish (GPUCommon::Execute_End) ---

void Execute_End(u32 op) {
	DisplayList *list = currentList;
	const u32 prev = ReadListWord(list->pc - 4);
	switch (prev >> 24) {
	case GE_CMD_SIGNAL: {
		const SignalBehavior behaviour = (SignalBehavior)((prev >> 16) & 0xFF);
		const int signal = prev & 0xFFFF;
		const int enddata = op & 0xFFFF;
		bool trigger = true;
		list->subIntrToken = (u16)signal;
		switch (behaviour) {
		case SIGNAL_HANDLER_SUSPEND:
			// Before SDK 0x02000010, sceGeListSync reads PAUSED during the callback.
			if (sdkVersion <= 0x02000010)
				list->state = DL_STATE_PAUSED;
			list->signal = behaviour;
			break;
		case SIGNAL_HANDLER_CONTINUE:
			list->signal = behaviour;
			break;
		case SIGNAL_HANDLER_PAUSE:
			// Paused from here on, though it runs to the FINISH that delivers it.
			trigger = false;
			list->signal = behaviour;
			list->state = DL_STATE_PAUSED;
			break;
		case SIGNAL_SYNC:
			trigger = false;
			list->signal = behaviour;
			break;
		case SIGNAL_JUMP:
		case SIGNAL_RJUMP:
		case SIGNAL_OJUMP: {
			trigger = false;
			list->signal = behaviour;
			u32 target = ((((u32)signal << 16) | (u32)enddata) & 0xFFFFFFFC) - 4;
			if (behaviour == SIGNAL_RJUMP)
				target += list->pc - 4;
			else if (behaviour == SIGNAL_OJUMP)
				target = RelAddr(target);
			if (!IsValidAddress(target))
				gpuState = GPUSTATE_ERROR;
			else
				list->pc = target;
			break;
		}
		case SIGNAL_CALL:
		case SIGNAL_RCALL:
		case SIGNAL_OCALL: {
			trigger = false;
			list->signal = behaviour;
			u32 target = ((((u32)signal << 16) | (u32)enddata) & 0xFFFFFFFC) - 4;
			if (behaviour == SIGNAL_RCALL)
				target += list->pc - 4;
			else if (behaviour == SIGNAL_OCALL)
				target = RelAddr(target);
			if (list->stackptr == 32) {
				LOGW("gpu", "signal CALL: stack full");
			} else if (!IsValidAddress(target)) {
				gpuState = GPUSTATE_ERROR;
			} else {
				StackEntry &e = list->stack[list->stackptr++];
				e.pc = list->pc;
				e.offsetAddr = gstate_c.offsetAddr;
				e.baseAddr = gstate.base;
				list->pc = target;
			}
			break;
		}
		case SIGNAL_RET: {
			trigger = false;
			list->signal = behaviour;
			if (list->stackptr == 0) {
				LOGW("gpu", "signal RET: stack empty");
			} else {
				const StackEntry &e = list->stack[--list->stackptr];
				gstate_c.offsetAddr = e.offsetAddr;
				gstate.base = e.baseAddr;
				list->pc = e.pc;
			}
			break;
		}
		default:
			LOGW("gpu", "unknown signal behaviour %02x (%04x %04x)", behaviour, signal, enddata);
			break;
		}
		if (list->interruptsEnabled && trigger) {
			TriggerInterrupt(*list);
			gpuState = GPUSTATE_INTERRUPT;
		}
		break;
	}
	case GE_CMD_FINISH:
		switch (list->signal) {
		case SIGNAL_HANDLER_PAUSE:
			list->state = DL_STATE_PAUSED;
			if (list->interruptsEnabled) {
				TriggerInterrupt(*list);
				gpuState = GPUSTATE_INTERRUPT;
			}
			break;
		case SIGNAL_SYNC:
			list->signal = SIGNAL_NONE;
			break;
		default:
			FlushImm();
			list->subIntrToken = (u16)(prev & 0xFFFF);
			gpuState = GPUSTATE_DONE;
			if (list->interruptsEnabled) {
				// The context is restored once the finish callback has run.
				TriggerInterrupt(*list);
			} else {
				if (list->started && list->context) {
					RestoreContextFrom(list->context);
					list->started = false;
				}
				list->state = DL_STATE_COMPLETED;
				NotifyListDone(*list);
			}
			break;
		}
		break;
	default:
		break;
	}
}

void ExecuteOp(u32 op, u32 diff) {
	const u32 cmd = op >> 24;
	const u8 flags = cmdFlags[cmd];
	if (diff && flags) {
		if (flags & CF_TARGET)
			binner::TargetChanged();
		if (flags & CF_RASTER)
			binner::DirtyState();
		if (flags & CF_TRANSFORM)
			TransformDirty();
	}
	DisplayList *list = currentList;
	switch (cmd) {
	case GE_CMD_VADDR:
		gstate_c.vertexAddr = RelAddr(op & 0x00FFFFFF);
		break;
	case GE_CMD_IADDR:
		gstate_c.indexAddr = RelAddr(op & 0x00FFFFFF);
		break;
	case GE_CMD_PRIM:
		Execute_Prim(op);
		break;
	case GE_CMD_BEZIER:
		FlushImm();
		Execute_Curve(op, false);
		break;
	case GE_CMD_SPLINE:
		FlushImm();
		Execute_Curve(op, true);
		break;
	case GE_CMD_BOUNDINGBOX:
		Execute_BoundingBox(op);
		break;
	case GE_CMD_JUMP: {
		const u32 target = RelAddr(op & 0x00FFFFFC);
		if (!IsValidAddress(target)) {
			LOGW("gpu", "JUMP to illegal address %08x", target);
			gpuState = GPUSTATE_ERROR;
		} else {
			list->pc = target - 4;
		}
		break;
	}
	case GE_CMD_BJUMP:
		if (!list->bboxResult) {
			const u32 target = RelAddr(op & 0x00FFFFFC);
			if (IsValidAddress(target))
				list->pc = target - 4;
			else
				gpuState = GPUSTATE_ERROR;
		}
		break;
	case GE_CMD_CALL: {
		const u32 target = RelAddr(op & 0x00FFFFFC);
		if (!IsValidAddress(target)) {
			LOGW("gpu", "CALL to illegal address %08x", target);
			gpuState = GPUSTATE_ERROR;
			break;
		}
		if (list->stackptr == 32) {
			LOGW("gpu", "CALL: stack full");
		} else {
			StackEntry &e = list->stack[list->stackptr++];
			e.pc = list->pc + 4;
			// The base address isn't saved for a regular call.
			e.offsetAddr = gstate_c.offsetAddr;
			list->pc = target - 4;
		}
		break;
	}
	case GE_CMD_RET:
		if (list->stackptr != 0) {
			const StackEntry &e = list->stack[--list->stackptr];
			gstate_c.offsetAddr = e.offsetAddr;
			list->pc = (e.pc & 0x0FFFFFFF) - 4;
		}
		break;
	case GE_CMD_END:
		Execute_End(op);
		break;
	case GE_CMD_OFFSETADDR:
		gstate_c.offsetAddr = op << 8;
		break;
	case GE_CMD_ORIGIN:
		gstate_c.offsetAddr = list->pc;
		break;
	case GE_CMD_LOADCLUT:
		LoadClut();
		break;
	case GE_CMD_TEXFLUSH:
	case GE_CMD_TEXSYNC:
		break;
	case GE_CMD_TRANSFERSTART:
		Execute_BlockTransfer();
		break;
	case GE_CMD_VAP:
		Execute_ImmVertex(op);
		break;
	case GE_CMD_MORPHWEIGHT0: case GE_CMD_MORPHWEIGHT0 + 1: case GE_CMD_MORPHWEIGHT0 + 2: case GE_CMD_MORPHWEIGHT0 + 3:
	case GE_CMD_MORPHWEIGHT0 + 4: case GE_CMD_MORPHWEIGHT0 + 5: case GE_CMD_MORPHWEIGHT0 + 6: case GE_CMD_MORPHWEIGHT7:
		gstate_c.morphWeights[cmd - GE_CMD_MORPHWEIGHT0] = getFloat24(op);
		break;
	case GE_CMD_WORLDMATRIXNUMBER:
		gstate.worldmtxnum = ((u32)GE_CMD_WORLDMATRIXNUMBER << 24) | (op & 0xF);
		break;
	case GE_CMD_VIEWMATRIXNUMBER:
		gstate.viewmtxnum = ((u32)GE_CMD_VIEWMATRIXNUMBER << 24) | (op & 0xF);
		break;
	case GE_CMD_PROJMATRIXNUMBER:
		gstate.projmtxnum = ((u32)GE_CMD_PROJMATRIXNUMBER << 24) | (op & 0xF);
		break;
	case GE_CMD_TGENMATRIXNUMBER:
		gstate.texmtxnum = ((u32)GE_CMD_TGENMATRIXNUMBER << 24) | (op & 0xF);
		break;
	case GE_CMD_BONEMATRIXNUMBER:
		gstate.boneMatrixNumber = ((u32)GE_CMD_BONEMATRIXNUMBER << 24) | (op & 0x7F);
		break;
	case GE_CMD_WORLDMATRIXDATA:
		Execute_MtxData(op, gstate.worldmtxnum, gstate.worldmtxdata, gstate.worldMatrix, 12, matrixVisible.all, 96, GE_CMD_WORLDMATRIXNUMBER, GE_CMD_WORLDMATRIXDATA);
		break;
	case GE_CMD_VIEWMATRIXDATA:
		Execute_MtxData(op, gstate.viewmtxnum, gstate.viewmtxdata, gstate.viewMatrix, 12, matrixVisible.all, 108, GE_CMD_VIEWMATRIXNUMBER, GE_CMD_VIEWMATRIXDATA);
		break;
	case GE_CMD_PROJMATRIXDATA:
		Execute_MtxData(op, gstate.projmtxnum, gstate.projmtxdata, gstate.projMatrix, 16, matrixVisible.all, 120, GE_CMD_PROJMATRIXNUMBER, GE_CMD_PROJMATRIXDATA);
		break;
	case GE_CMD_TGENMATRIXDATA: {
		const int num = gstate.texmtxnum & 0x00FFFFFF;
		if (num < 12) {
			u32 *target = (u32 *)&gstate.tgenMatrix[num];
			const u32 newVal = op << 8;
			if (newVal != *target) {
				*target = newVal;
				TransformDirty();
				binner::DirtyState();
			}
		}
		// Doesn't wrap to any other matrix.
		if ((num & 0xF) < 12)
			matrixVisible.tgen[num & 0xF] = op & 0x00FFFFFF;
		gstate.texmtxnum = ((u32)GE_CMD_TGENMATRIXNUMBER << 24) | ((num + 1) & 0x00FFFFFF);
		gstate.texmtxdata = (u32)GE_CMD_TGENMATRIXDATA << 24;
		break;
	}
	case GE_CMD_BONEMATRIXDATA: {
		const int num = gstate.boneMatrixNumber & 0x00FFFFFF;
		if (num < 96)
			*(u32 *)&gstate.boneMatrix[num] = op << 8;
		matrixVisible.all[num & 0x7F] = op & 0x00FFFFFF;
		gstate.boneMatrixNumber = ((u32)GE_CMD_BONEMATRIXNUMBER << 24) | ((num + 1) & 0x00FFFFFF);
		gstate.boneMatrixData = (u32)GE_CMD_BONEMATRIXDATA << 24;
		break;
	}
	default:
		break;
	}
}

// Runs lists until one stalls, pauses or raises an interrupt (GPUCommon::ProcessDLQueue).
void ProcessDLQueue() {
	while (!dlQueue.empty()) {
		const int listIndex = dlQueue.front();
		DisplayList &list = dls[listIndex];
		if (list.state == DL_STATE_PAUSED)
			return;
		// Until the interrupt of a SIGNAL or FINISH has been delivered, nothing runs.
		if (list.pendingInterrupt)
			return;

		currentList = &list;
		if (!list.started && list.context)
			SaveContextTo(list.context);
		list.started = true;
		gstate_c.offsetAddr = list.offsetAddr;
		if (!IsValidAddress(list.pc)) {
			LOGW("gpu", "display list %d has a bad pc %08x, dropping it", listIndex, list.pc);
			CompleteFailedList(list);
			RemoveFromQueue(listIndex);
			continue;
		}
		list.state = DL_STATE_RUNNING;
		list.interrupted = false;
		gpuState = list.pc == list.stall ? GPUSTATE_STALL : GPUSTATE_RUNNING;

		while (gpuState == GPUSTATE_RUNNING) {
			if (list.pc == list.stall) {
				gpuState = GPUSTATE_STALL;
				break;
			}
			if (!IsValidAddress(list.pc)) {
				gpuState = GPUSTATE_ERROR;
				break;
			}
			const u32 op = ReadListWord(list.pc);
			const u32 cmd = op >> 24;
			const u32 diff = op ^ gstate.cmdmem[cmd];
			gstate.cmdmem[cmd] = op;
			g_stats.commands++;
			ExecuteOp(op, diff);
			list.pc += 4;
		}

		list.offsetAddr = gstate_c.offsetAddr;
		switch (gpuState) {
		case GPUSTATE_DONE:
			if (list.pendingInterrupt) {
				// The list stays at the head until its finish callback has run (InterruptEnd).
				if (dlQueue.size() == 1)
					NotifyDrawSyncDone();
				return;
			}
			break;
		case GPUSTATE_ERROR:
			CompleteFailedList(list);
			break;
		case GPUSTATE_STALL:
		case GPUSTATE_INTERRUPT:
		default:
			return;
		}
		if (list.state != DL_STATE_QUEUED)
			RemoveFromQueue(listIndex);
	}
	currentList = nullptr;
	if (drawPending)
		NotifyDrawSyncDone();
}

// GeIntrHandler::run: starts the oldest pending interrupt. Returns true if there's a callback to run.
bool InterruptBegin(Interrupt *out) {
	const PendingInterrupt intr = pendingIntrs.front();
	DisplayList &dl = dls[intr.listid];
	interruptRunning = true;
	const u32 cmd = intr.cmd;
	int kind = 0;  // 1 signal callback, 2 finish callback
	if (dl.callbackId >= 0) {
		switch (dl.signal) {
		case SIGNAL_SYNC:
		case SIGNAL_JUMP:
		case SIGNAL_CALL:
		case SIGNAL_RET:
			break;
		case SIGNAL_HANDLER_PAUSE:
			if (cmd == GE_CMD_FINISH)
				kind = 1;
			break;
		default:
			kind = cmd == GE_CMD_SIGNAL ? 1 : 2;
			break;
		}
	}
	// The list completes once the interrupt starts; one sceGeBreak(1) reset stays that way.
	if (dl.signal != SIGNAL_HANDLER_PAUSE && cmd == GE_CMD_FINISH && dl.state != DL_STATE_NONE)
		dl.state = DL_STATE_COMPLETED;
	// A delivered pause is marked like sceGeBreak's, which lets sceGeContinue through.
	if (dl.signal == SIGNAL_HANDLER_PAUSE && cmd == GE_CMD_FINISH)
		dl.signal = SIGNAL_HANDLER_SUSPEND;
	intrListPc = intr.pc + 4;
	if (out) {
		out->listId = (int)(LIST_ID_MAGIC ^ (u32)intr.listid);
		out->callbackId = kind ? dl.callbackId : -1;
		out->isFinish = kind == 2;
		out->arg = dl.subIntrToken & 0xFFFF;
		out->listPc = sdkVersion <= 0x02000010 ? 0 : intr.pc + 4;
	}
	if (kind)
		return true;
	if (dl.signal == SIGNAL_HANDLER_SUSPEND && cmd == GE_CMD_SIGNAL && sdkVersion <= 0x02000010) {
		if (dl.state != DL_STATE_NONE && dl.state != DL_STATE_COMPLETED)
			dl.state = DL_STATE_QUEUED;
	}
	pendingIntrs.pop_front();
	InterruptEnd(intr.listid);
	return false;
}

// GeIntrHandler::handleResult: after the callback.
void InterruptFinish() {
	if (pendingIntrs.empty()) {
		interruptRunning = false;
		return;
	}
	const PendingInterrupt intr = pendingIntrs.front();
	pendingIntrs.pop_front();
	DisplayList &dl = dls[intr.listid];
	if (intr.cmd == GE_CMD_SIGNAL && dl.signal == SIGNAL_HANDLER_SUSPEND && sdkVersion <= 0x02000010) {
		if (dl.state != DL_STATE_NONE && dl.state != DL_STATE_COMPLETED)
			dl.state = DL_STATE_QUEUED;
	}
	InterruptEnd(intr.listid);
}

void DeliverInterruptSync() {
	Interrupt info;
	binner::Flush();
	if (InterruptBegin(&info)) {
		if (info.isFinish) {
			if (hooks.onFinish)
				hooks.onFinish(hooks.user, info.listId, info.callbackId, info.arg);
		} else {
			if (hooks.onSignal)
				hooks.onSignal(hooks.user, info.listId, info.callbackId, info.arg);
		}
		InterruptFinish();
	}
}

bool BusyDrawing() {
	// Stopped for a finish callback, and a signal callback unless it's a CONTINUE one.
	if (interruptRunning) {
		const bool continueSignal = gpuState == GPUSTATE_INTERRUPT && currentList && currentList->signal == SIGNAL_HANDLER_CONTINUE;
		if (!continueSignal)
			return false;
	}
	const int state = DrawSync(1);
	if (state == 2 || state == 3) {
		if (currentList && currentList->state != DL_STATE_PAUSED)
			return true;
	}
	return false;
}

void ResetLists() {
	for (int i = 0; i < MAX_LISTS; ++i) {
		memset(&dls[i], 0, sizeof(dls[i]));
		dls[i].state = DL_STATE_NONE;
		dls[i].callbackId = -1;
	}
	dlQueue.clear();
	pendingIntrs.clear();
	nextListID = 0;
	currentList = nullptr;
	interruptRunning = false;
	isbreak = false;
	drawPending = false;
	gpuState = GPUSTATE_DONE;
	asyncTaken = false;
}

}  // namespace

// --- Memory helpers ---

u32 FoldAddress(u32 addr) {
	u32 a = addr & 0x1FFFFFFF;
	if ((a & 0x1F800000) == 0x04000000)
		a = 0x04000000 | (a & 0x1FFFFF);
	return a;
}

bool IsValidAddress(u32 addr) {
	const u32 a = addr & 0x1FFFFFFF;
	if ((a & 0x1F800000) == 0x04000000)
		return true;
	if (a >= 0x08000000 && a < 0x08000000 + g_ramSize)
		return true;
	if (a >= 0x00010000 && a < 0x00014000)
		return true;
	return false;
}

u32 ValidBytesAt(u32 addr) {
	const u32 a = FoldAddress(addr);
	if (a >= 0x04000000 && a < 0x04000000 + VRAM_SIZE)
		return 0x04000000 + VRAM_SIZE - a;
	if (a >= 0x08000000 && a < 0x08000000 + g_ramSize)
		return 0x08000000 + g_ramSize - a;
	if (a >= 0x00010000 && a < 0x00014000)
		return 0x00014000 - a;
	return 0;
}

u8 *GuestPtr(u32 addr) {
	return GPtr(addr);
}

u8 *VramPtr() {
	return GPtr(VRAM_BASE);
}

// --- Public API ---

void Init() {
	InitCmdFlags();
	ResetGEState();
	ResetLists();
	memset(clutData, 0, sizeof(clutData));
	immCount = 0;
	immPrim = GE_PRIM_INVALID;
	binner::Init();
	ResetPrimAssembly();
	binner::UpdateClut(clutData);
	g_stats = InternalStats{};
}

void Shutdown() {
	binner::Shutdown();
	ResetLists();
}

void Reinitialize() {
	binner::Flush();
	ResetLists();
	ResetGEState();
	ResetPrimAssembly();
	binner::TargetChanged();
	binner::DirtyState();
}

void SetHooks(const Hooks &h) {
	hooks = h;
}

void SetSdkVersion(u32 version) {
	sdkVersion = version;
}

void SetRamSize(u32 bytes) {
	g_ramSize = bytes;
}

void SetRenderThreads(int count) {
	binner::Flush();
	binner::SetThreads(count);
}

void SetInterruptsEnabled(bool enabled) {
	cpuInterruptsEnabled = enabled;
}

u32 CurrentInterruptListPc() {
	return sdkVersion <= 0x02000010 ? 0 : intrListPc;
}

void Run() {
	if (inRun) {
		rerun = true;
		return;
	}
	inRun = true;
	do {
		rerun = false;
		ProcessDLQueue();
		if (!hooks.onInterruptPending) {
			while (cpuInterruptsEnabled && !interruptRunning && !pendingIntrs.empty()) {
				DeliverInterruptSync();
				ProcessDLQueue();
			}
		}
	} while (rerun);
	binner::Flush();
	inRun = false;
}

bool TakeInterrupt(Interrupt *out) {
	if (pendingIntrs.empty() || interruptRunning)
		return false;
	binner::Flush();
	if (InterruptBegin(out)) {
		asyncTaken = true;
		return true;
	}
	// No callback: it's done already. Report it with callbackId -1.
	Run();
	return true;
}

void InterruptDone() {
	if (asyncTaken) {
		asyncTaken = false;
		InterruptFinish();
	}
	Run();
}

int EnqueueList(u32 listpc, u32 stall, int callbackId, u32 optParamAddr, bool head) {
	if (((listpc | stall) & 3) != 0 || !IsValidAddress(listpc))
		return (int)ERR_INVALID_POINTER;
	const bool argsValid = optParamAddr != 0 && ValidBytesAt(optParamAddr) >= 4;
	const u32 argsSize = argsValid ? GRead32(optParamAddr) : 0;
	if (argsValid && argsSize >= 16 && GRead32(optParamAddr + 8) >= 256)
		return (int)ERR_INVALID_SIZE;
	const u32 stackAddr = argsValid && argsSize >= 16 ? GRead32(optParamAddr + 12) : 0;

	// Same list address, or the same stack as a list that has started: busy (newer SDKs only).
	if (sdkVersion > 0x01FFFFFF) {
		for (int i : dlQueue) {
			const DisplayList &other = dls[i];
			// A list only waiting for its FINISH interrupt is already off the queue on hardware.
			if (other.pendingInterrupt && other.state != DL_STATE_COMPLETED && gpuState == GPUSTATE_DONE)
				continue;
			if (other.startpc == (listpc & 0x0FFFFFFF))
				return (int)ERR_BUSY;
			if (stackAddr != 0 && other.stackAddr == stackAddr && other.started)
				return (int)ERR_BUSY;
		}
	}

	int id = -1;
	for (int i = 0; i < MAX_LISTS; ++i) {
		const int possibleID = (i + nextListID) % MAX_LISTS;
		const DisplayList &possible = dls[possibleID];
		if (possible.pendingInterrupt)
			continue;
		if (possible.state == DL_STATE_NONE) {
			id = possibleID;
			break;
		}
		if (possible.state == DL_STATE_COMPLETED && !possible.waitPending)
			id = possibleID;
	}
	if (id < 0)
		return (int)ERR_OUT_OF_MEMORY;
	nextListID = id + 1;

	DisplayList &dl = dls[id];
	dl.id = id;
	dl.startpc = listpc & 0x0FFFFFFF;
	dl.pc = listpc & 0x0FFFFFFF;
	dl.stall = stall & 0x0FFFFFFF;
	dl.callbackId = callbackId >= 0 ? callbackId : -1;
	dl.stackptr = 0;
	dl.signal = SIGNAL_NONE;
	dl.interrupted = false;
	dl.waitPending = true;
	dl.interruptsEnabled = true;
	dl.started = false;
	dl.offsetAddr = 0;
	dl.bboxResult = false;
	dl.stackAddr = stackAddr;
	dl.context = 0;
	if (argsValid && argsSize >= 8) {
		const u32 ctx = GRead32(optParamAddr + 4);
		if (ctx && IsValidAddress(ctx))
			dl.context = ctx;
	}

	bool runList = false;
	if (head) {
		if (currentList) {
			if (currentList->state != DL_STATE_PAUSED)
				return (int)ERR_INVALID_VALUE;
			currentList->state = DL_STATE_QUEUED;
			currentList->signal = SIGNAL_NONE;
		}
		dl.state = DL_STATE_PAUSED;
		currentList = &dl;
		dlQueue.push_front(id);
	} else if (currentList) {
		dl.state = DL_STATE_QUEUED;
		dlQueue.push_back(id);
		drawPending = true;
	} else {
		dl.state = DL_STATE_RUNNING;
		currentList = &dl;
		dlQueue.push_front(id);
		drawPending = true;
		runList = true;
	}
	if (runList)
		Run();
	return (int)(LIST_ID_MAGIC ^ (u32)id);
}

int DequeueList(int listId) {
	const int listid = (int)(LIST_ID_MAGIC ^ (u32)listId);
	if (listid < 0 || listid >= MAX_LISTS || dls[listid].state == DL_STATE_NONE)
		return (int)ERR_INVALID_ID;
	DisplayList &dl = dls[listid];
	// Started lists are off limits, including completed ones until sceGeDrawSync recycles them.
	if (dl.started || dl.state == DL_STATE_COMPLETED)
		return (int)ERR_BUSY;
	dl.state = DL_STATE_NONE;
	if (!dlQueue.empty() && listid == dlQueue.front())
		PopDLQueue();
	else
		RemoveFromQueue(listid);
	NotifyListDone(dl);
	return 0;
}

int UpdateStallAddr(int listId, u32 stallAddr) {
	const int listid = (int)(LIST_ID_MAGIC ^ (u32)listId);
	if (listid < 0 || listid >= MAX_LISTS || dls[listid].state == DL_STATE_NONE)
		return (int)ERR_INVALID_ID;
	DisplayList &dl = dls[listid];
	if (dl.state == DL_STATE_COMPLETED)
		return (int)ERR_ALREADY;
	dl.stall = stallAddr & 0x0FFFFFFF;
	Run();
	return 0;
}

int ListSync(int listId, int mode) {
	const int listid = (int)(LIST_ID_MAGIC ^ (u32)listId);
	if (listid < 0 || listid >= MAX_LISTS)
		return (int)ERR_INVALID_ID;
	if (mode < 0 || mode > 1)
		return (int)ERR_INVALID_MODE;
	const DisplayList &dl = dls[listid];
	if (mode == 1) {
		switch (dl.state) {
		case DL_STATE_QUEUED: return dl.interrupted ? 4 : 1;
		case DL_STATE_RUNNING: return dl.pc == dl.stall ? 3 : 2;
		case DL_STATE_COMPLETED: return 0;
		case DL_STATE_PAUSED: return 4;
		default: return (int)ERR_INVALID_ID;
		}
	}
	return dl.waitPending ? SYNC_WAIT : 0;
}

int DrawSync(int mode) {
	if (mode < 0 || mode > 1)
		return (int)ERR_INVALID_MODE;
	if (mode == 0) {
		if (drawPending)
			return SYNC_WAIT;
		DrawSyncWoken();
		return 0;
	}
	const DisplayList *top = nullptr;
	for (int i : dlQueue) {
		if (dls[i].state != DL_STATE_COMPLETED) {
			top = &dls[i];
			break;
		}
	}
	if (!top || top->state == DL_STATE_COMPLETED)
		return 0;
	// The firmware compares the stall address against the hardware's pc.
	if (top->state != DL_STATE_QUEUED && top->pc == top->stall)
		return 3;
	return 2;
}

void DrawSyncWoken() {
	for (int i = 0; i < MAX_LISTS; ++i) {
		if (dls[i].state == DL_STATE_COMPLETED)
			dls[i].state = DL_STATE_NONE;
	}
}

int Break(int mode, u32 paramAddr) {
	if (mode < 0 || mode > 1)
		return (int)ERR_INVALID_MODE;
	if ((int)paramAddr < 0 || (int)(paramAddr + 16) < 0)
		return (int)ERR_PRIV_REQUIRED;
	if (!currentList)
		return (int)ERR_ALREADY;
	if (mode == 1) {
		// Throws the queue away and resets the GE; raised interrupts go too, but not the one running.
		dlQueue.clear();
		for (int i = 0; i < MAX_LISTS; ++i) {
			dls[i].state = DL_STATE_NONE;
			dls[i].signal = SIGNAL_NONE;
		}
		auto it = pendingIntrs.begin();
		if (interruptRunning && it != pendingIntrs.end())
			++it;
		while (it != pendingIntrs.end()) {
			dls[it->listid].pendingInterrupt = false;
			it = pendingIntrs.erase(it);
		}
		nextListID = 0;
		currentList = nullptr;
		return 0;
	}
	if (currentList->state == DL_STATE_NONE || currentList->state == DL_STATE_COMPLETED)
		return sdkVersion >= 0x02000000 ? (int)0x80000004 : -1;
	if (currentList->state == DL_STATE_PAUSED) {
		if (sdkVersion > 0x02000010) {
			if (currentList->signal != SIGNAL_HANDLER_PAUSE)
				return (int)ERR_ALREADY;
		}
		return (int)ERR_BUSY;
	}
	if (currentList->state == DL_STATE_QUEUED) {
		currentList->state = DL_STATE_PAUSED;
		return (int)(LIST_ID_MAGIC ^ (u32)currentList->id);
	}
	if (currentList->signal == SIGNAL_SYNC)
		currentList->pc += 8;
	// The pc to resume at is now what a new list is compared against.
	currentList->startpc = currentList->pc;
	currentList->interrupted = true;
	currentList->state = DL_STATE_PAUSED;
	currentList->signal = SIGNAL_HANDLER_SUSPEND;
	isbreak = interruptRunning || !cpuInterruptsEnabled;
	return (int)(LIST_ID_MAGIC ^ (u32)currentList->id);
}

int Continue() {
	if (!currentList)
		return 0;
	if (currentList->state == DL_STATE_PAUSED) {
		if (!isbreak) {
			// Inside the pause window (PAUSE signal seen, FINISH not yet).
			if (currentList->signal == SIGNAL_HANDLER_PAUSE)
				return (int)ERR_BUSY;
			currentList->state = DL_STATE_RUNNING;
			currentList->signal = SIGNAL_NONE;
			drawPending = true;
		} else {
			currentList->state = DL_STATE_QUEUED;
			currentList->signal = SIGNAL_NONE;
		}
	} else if (currentList->state == DL_STATE_RUNNING) {
		return sdkVersion >= 0x02000000 ? (int)0x80000020 : -1;
	} else {
		return sdkVersion >= 0x02000000 ? (int)0x80000004 : -1;
	}
	Run();
	return 0;
}

u32 GetCmd(int cmd) {
	if (cmd < 0 || cmd >= 256)
		return ERR_INVALID_INDEX;
	u32 val = gstate.cmdmem[cmd];
	switch (cmd) {
	case GE_CMD_BONEMATRIXDATA:
	case GE_CMD_WORLDMATRIXDATA:
	case GE_CMD_VIEWMATRIXDATA:
	case GE_CMD_PROJMATRIXDATA:
	case GE_CMD_TGENMATRIXDATA:
		val &= 0xFF000000;
		break;
	case GE_CMD_BONEMATRIXNUMBER:
		val &= 0xFF00007F;
		break;
	case GE_CMD_WORLDMATRIXNUMBER:
	case GE_CMD_VIEWMATRIXNUMBER:
	case GE_CMD_PROJMATRIXNUMBER:
	case GE_CMD_TGENMATRIXNUMBER:
		val &= 0xFF00000F;
		break;
	default:
		break;
	}
	return val;
}

int GetMatrix(int type, u32 outAddr) {
	const int size = type == GE_MTX_PROJECTION ? 16 : 12;
	if (ValidBytesAt(outAddr) < (u32)size * 4)
		return -1;
	const u32 *src;
	switch (type) {
	case GE_MTX_WORLD: src = matrixVisible.world; break;
	case GE_MTX_VIEW: src = matrixVisible.view; break;
	case GE_MTX_PROJECTION: src = matrixVisible.proj; break;
	case GE_MTX_TEXGEN: src = matrixVisible.tgen; break;
	default:
		if (type >= GE_MTX_BONE0 && type <= GE_MTX_BONE7) {
			src = matrixVisible.bone + (type - GE_MTX_BONE0) * 12;
			break;
		}
		return (int)ERR_INVALID_INDEX;
	}
	for (int i = 0; i < size; ++i)
		GWrite32(outAddr + 4 * i, src[i]);
	return 0;
}

int GetStack(int index, u32 stackAddr) {
	if (!currentList)
		return 0;
	if (currentList->stackptr <= index)
		return (int)ERR_INVALID_INDEX;
	if (index >= 0 && ValidBytesAt(stackAddr) >= 32) {
		const StackEntry &entry = currentList->stack[index];
		GWrite32(stackAddr + 0, 0);
		GWrite32(stackAddr + 4, entry.pc + 4);
		GWrite32(stackAddr + 8, entry.offsetAddr);
		GWrite32(stackAddr + 28, entry.baseAddr);
	}
	return currentList->stackptr;
}

int SaveContext(u32 ctxAddr) {
	if (BusyDrawing())
		return -1;
	SaveContextTo(ctxAddr);
	return 0;
}

int RestoreContext(u32 ctxAddr) {
	if (BusyDrawing())
		return (int)ERR_BUSY;
	RestoreContextFrom(ctxAddr);
	if (ValidBytesAt(ctxAddr) < 512 * 4)
		ReapplyGfxState();
	return 0;
}

int SetEdramAddrTranslation(u32 value) {
	const bool outsideRange = value != 0 && (value < 0x200 || value > 0x1000);
	const bool notPowerOfTwo = (value & (value - 1)) != 0;
	if (outsideRange || notPowerOfTwo)
		return (int)ERR_INVALID_VALUE;
	std::swap(edramTranslation, value);
	return (int)value;
}

void ReadFramebuffer(u32 addr, int stride, int pixelFormat, int width, int height, u32 *outRgba) {
	binner::Flush();
	const int bpp = pixelFormat == 3 ? 4 : 2;
	for (int y = 0; y < height; ++y) {
		u32 *out = outRgba + (size_t)y * width;
		const u32 row = addr + (u32)(y * stride * bpp);
		for (int x = 0; x < width; ++x) {
			const u32 a = row + (u32)(x * bpp);
			u32 c = 0;
			if (ValidBytesAt(a) >= (u32)bpp) {
				const u8 *p = GuestPtr(FoldAddress(a));
				if (bpp == 4) {
					memcpy(&c, p, 4);
				} else {
					u16 v;
					memcpy(&v, p, 2);
					c = ConvertToRGBA8888(v, pixelFormat);
				}
			}
			out[x] = c | 0xFF000000;
		}
	}
}

Stats GetStats() {
	Stats s;
	s.commands = g_stats.commands;
	s.prims = g_stats.prims;
	s.vertices = g_stats.vertices;
	s.pixelsSubmitted = g_stats.pixels;
	s.flushes = g_stats.flushes;
	s.threadedFlushes = g_stats.threadedFlushes;
	s.transfers = g_stats.transfers;
	return s;
}

void ResetStats() {
	g_stats = InternalStats{};
}

}  // namespace gpu
