// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// GE register reset and the context format of sceGeSaveContext/RestoreContext (GPU/GPUState.cpp).
#include "gpu/ge_state.h"

#include <cstring>

namespace gpu {

alignas(16) GEState gstate;
alignas(16) GEStateCache gstate_c;
MatrixVisible matrixVisible;

namespace {

struct CmdRange {
	u8 start;
	u8 end;
};

// The registers a context holds, from word 17 on.
const CmdRange contextCmdRanges[] = {
	{ 0x00, 0x02 }, { 0x10, 0x10 }, { 0x12, 0x28 }, { 0x2c, 0x33 }, { 0x36, 0x38 }, { 0x42, 0x4D },
	{ 0x50, 0x51 }, { 0x53, 0x58 }, { 0x5B, 0xB5 }, { 0xB8, 0xC3 }, { 0xC5, 0xD0 }, { 0xD2, 0xE9 },
	{ 0xEB, 0xEC }, { 0xEE, 0xEE }, { 0xF0, 0xF6 }, { 0xF8, 0xF9 },
};

u32 *SaveMatrix(u32 *cmds, const u32 *visible, int count, int numcmd, int datacmd) {
	*cmds++ = (u32)numcmd << 24;
	for (int i = 0; i < count; ++i)
		*cmds++ = visible[i] | ((u32)datacmd << 24);
	return cmds;
}

const u32 *LoadMatrix(const u32 *cmds, float *mtx, int count) {
	// Skip the number reset.
	cmds++;
	for (int i = 0; i < count; ++i)
		mtx[i] = getFloat24(*cmds++);
	return cmds;
}

}  // namespace

void ResetGEState() {
	for (int i = 0; i < 256; i++)
		gstate.cmdmem[i] = (u32)i << 24;
	memset(gstate.worldMatrix, 0, sizeof(gstate.worldMatrix));
	memset(gstate.viewMatrix, 0, sizeof(gstate.viewMatrix));
	memset(gstate.projMatrix, 0, sizeof(gstate.projMatrix));
	memset(gstate.tgenMatrix, 0, sizeof(gstate.tgenMatrix));
	memset(gstate.boneMatrix, 0, sizeof(gstate.boneMatrix));
	memset(&gstate_c, 0, sizeof(gstate_c));
	ResetVisibleMatrices();
}

void ResetVisibleMatrices() {
	for (int i = 0; i < 96; ++i)
		matrixVisible.bone[i] = toFloat24(gstate.boneMatrix[i]);
	for (int i = 0; i < 12; ++i) {
		matrixVisible.world[i] = toFloat24(gstate.worldMatrix[i]);
		matrixVisible.view[i] = toFloat24(gstate.viewMatrix[i]);
		matrixVisible.tgen[i] = toFloat24(gstate.tgenMatrix[i]);
	}
	for (int i = 0; i < 16; ++i)
		matrixVisible.proj[i] = toFloat24(gstate.projMatrix[i]);
}

void SaveGEState(u32 *ptr) {
	ptr[5] = gstate_c.vertexAddr;
	ptr[6] = gstate_c.indexAddr;
	ptr[7] = gstate_c.offsetAddr;
	u32 *cmds = ptr + 17;
	for (const CmdRange &r : contextCmdRanges) {
		for (int n = r.start; n <= r.end; ++n)
			*cmds++ = gstate.cmdmem[n];
	}
	cmds = SaveMatrix(cmds, matrixVisible.bone, 96, GE_CMD_BONEMATRIXNUMBER, GE_CMD_BONEMATRIXDATA);
	cmds = SaveMatrix(cmds, matrixVisible.world, 12, GE_CMD_WORLDMATRIXNUMBER, GE_CMD_WORLDMATRIXDATA);
	cmds = SaveMatrix(cmds, matrixVisible.view, 12, GE_CMD_VIEWMATRIXNUMBER, GE_CMD_VIEWMATRIXDATA);
	cmds = SaveMatrix(cmds, matrixVisible.proj, 16, GE_CMD_PROJMATRIXNUMBER, GE_CMD_PROJMATRIXDATA);
	cmds = SaveMatrix(cmds, matrixVisible.tgen, 12, GE_CMD_TGENMATRIXNUMBER, GE_CMD_TGENMATRIXDATA);
	*cmds++ = gstate.boneMatrixNumber & 0xFF00007F;
	*cmds++ = gstate.worldmtxnum & 0xFF00000F;
	*cmds++ = gstate.viewmtxnum & 0xFF00000F;
	*cmds++ = gstate.projmtxnum & 0xFF00000F;
	*cmds++ = gstate.texmtxnum & 0xFF00000F;
	*cmds++ = (u32)GE_CMD_END << 24;
}

void RestoreGEState(const u32 *ptr) {
	gstate_c.vertexAddr = ptr[5];
	gstate_c.indexAddr = ptr[6];
	gstate_c.offsetAddr = ptr[7];
	const u32 *cmds = ptr + 17;
	for (const CmdRange &r : contextCmdRanges) {
		for (int n = r.start; n <= r.end; ++n)
			gstate.cmdmem[n] = *cmds++;
	}
	cmds = LoadMatrix(cmds, gstate.boneMatrix, 96);
	cmds = LoadMatrix(cmds, gstate.worldMatrix, 12);
	cmds = LoadMatrix(cmds, gstate.viewMatrix, 12);
	cmds = LoadMatrix(cmds, gstate.projMatrix, 16);
	cmds = LoadMatrix(cmds, gstate.tgenMatrix, 12);
	gstate.boneMatrixNumber = (*cmds++) & 0xFF00007F;
	gstate.worldmtxnum = (*cmds++) & 0xFF00000F;
	gstate.viewmtxnum = (*cmds++) & 0xFF00000F;
	gstate.projmtxnum = (*cmds++) & 0xFF00000F;
	gstate.texmtxnum = (*cmds++) & 0xFF00000F;
	ResetVisibleMatrices();
}

}  // namespace gpu
