// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native reimplementation of the 4.01 M33 custom-firmware layer (systemctrl, vshctrl, KUBridge).
// Specification: docs/m33-behaviors.md. M33 works by patching Sony modules in RAM; here the same
// behaviours are provided by the native kernel instead, so no patching happens.
#pragma once

#include <string>
#include <vector>

#include "common/common.h"

namespace m33 {

// SEConfig (systemctrl_se.h): 0x58 bytes stored in flash1:/config.se.
struct SEConfig {
	s32 magic;  // 0x47434553 "SECG"
	s32 hidecorrupt;
	s32 skiplogo;
	s32 umdactivatedplaincheck;
	s32 gamekernel150;
	s32 executebootbin;
	s32 startupprog;
	s32 umdmode;
	s32 useisofsonumdinserted;
	s32 vshcpuspeed;
	s32 vshbusspeed;
	s32 umdisocpuspeed;
	s32 umdisobusspeed;
	s32 fakeregion;
	s32 freeumdregion;
	s32 hardresetHB;
	s32 usbdevice;
	s32 novshmenu;
	s32 usbcharge;
	s32 notusedaxupd;
	s32 reserved[2];
};
static_assert(sizeof(SEConfig) == 0x58, "SEConfig layout");

enum Apitype : u32 {
	APITYPE_VSH = 0x100,       // XMB
	APITYPE_GAME_UMD = 0x120,  // disc / ISO game
	APITYPE_GAME_MS = 0x141,   // memory stick game (homebrew, PSN)
	APITYPE_POPS = 0x144,
};

struct BootInfo {
	u32 apitype = APITYPE_GAME_MS;
	std::string initFileName;  // what kuKernelInitFileName reports
	bool fromDisc = false;
};

void Init(const std::string &flash1HostDir, const BootInfo &boot);
const SEConfig &Config();
void SetConfig(const SEConfig &c);

// Loads and starts the plugins listed for the current boot type (seplugins/game.txt etc.).
// Called before the main module starts. Before the first plugin it publishes the firmware
// modules listed in `bootModulesCsv` (data/fw_boot_modules.csv) for plugins to find.
void LoadPlugins(const std::string &bootModulesCsv);

// The firmware's module descriptor (SceModule2) for a loaded module, in kernel memory; 0 if `uid`
// isn't one. LoadCoreForKernel's module lookups return it.
u32 ModuleDescriptor(int uid);

// NID aliases from data/m33_nid_resolver.csv: old (1.50-era) kernel NIDs -> 4.01 NIDs.
void RegisterNidResolver(const std::string &csvPath);

// A sctrlKernelLoadExecVSH* call asked for a relaunch: the host executable to run next.
bool TakeRelaunch(std::string &hostFile);

// The XMB launches `file` (LoadExecVSH* through sceVshBridge or SystemCtrl): the runtime restarts
// with it, as the PSP reboots into the game. Returns 0, or an error when the file can't be reached.
int LoadExecFromVsh(const char *file, u32 apitype, u32 paramPtr);
// The XMB restarts itself (sceKernelExitVSHVSH), or a game launched from it exits to it.
int ExitToVsh();
// After each controller read of the XMB (sceVshBridge): the VSH menu (satelite.prx) on SELECT.
void OnVshCtrlRead(u32 padPtr, int count);
// After the run: the program asked to return to the XMB.
bool TakeExitToVsh();
// After an XMB launch: the vshmain_args block it passed (vshmain gets it back when the game
// exits, see vsh::SetVshmainArgs).
bool TakeVshmainArgs(std::vector<u8> &out);

}  // namespace m33
