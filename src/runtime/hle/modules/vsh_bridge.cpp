// SPDX-License-Identifier: GPL-2.0-or-later
//
// The libraries only the VSH (XMB) uses:
//  - sceVshBridge: flash0:/kd/vshbridge.prx, the kernel module through which the user-mode VSH
//    reaches kernel functions. Each of its exports checks the caller (user level 4) and calls one
//    kernel function with the same arguments; the table below records which (read from 4.01's
//    vshbridge.prx, kernel NIDs named through data/m33_nid_resolver.csv), and forwards to PSPC's
//    implementation of that function.
//  - sceResmgr: decrypts the XMB's index (flash0:/vsh/etc/index*.dat) through pspc-decrypt.
//
// The sceImpose_driver functions the bridge reaches are in system.cpp, sceKernelLoadModuleVSH in
// modulemgr.cpp, the LoadExec family in m33.cpp.
#include <map>
#include <vector>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/console_id.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/kernel_hle.h"
#include "runtime/kernel/module.h"
#include "runtime/m33/m33.h"

namespace modulemgr {
int LoadModuleVsh(const char *path, u32 flags, u32 optPtr);
}
namespace ctrl {
int ReadBufferPositiveKernel(u32 ptr, u32 n);
}

namespace {

constexpr const char *LOG = "VSHBRIDGE";

// ---- vshbridge's own state ----

// A 4-slot table of guest pointers the VSH registers (vshbridge keeps it for the kernel side;
// nothing native reads it). Registering an occupied slot fails with 0x80000020, unregistering an
// empty one with 0x80000025, a slot past 3 with 0x80000102 (vshbridge.prx 4.01).
u32 g_slots[4] = {};
int vshBridgeRegisterSlot(u32 slot, u32 value) {
	if (slot >= 4) return (int)0x80000102;
	if (g_slots[slot]) return (int)0x80000020;
	g_slots[slot] = value;
	return 0;
}
int vshBridgeUnregisterSlot(u32 slot) {
	if (slot >= 4) return (int)0x80000102;
	if (!g_slots[slot]) return (int)0x80000025;
	g_slots[slot] = 0;
	return 0;
}

// Sets bit `bit` of an event flag vshbridge owns (its kernel threads wait on it; none here).
int vshBridgeSetEventBit(int bit) {
	LOGD(LOG, "event bit %d set", bit);
	return 0;
}

int vshKernelLoadModuleVSH(const char *path, u32 flags, u32 optPtr) { return modulemgr::LoadModuleVsh(path, flags, optPtr); }

// sceCtrl_driver BA664B5E (sceCtrlReadBufferPositive, a kernel-mode read: every button plus the
// media bits), then M33 vshctrl's hook on it (the VSH menu on SELECT, m33.cpp).
int vshCtrlReadBufferPositive(u32 pad, int count) {
	int r = ctrl::ReadBufferPositiveKernel(pad, (u32)count);
	if (r > 0) m33::OnVshCtrlRead(pad, count);
	return r;
}

// sceIdStorageLookup(key, offset, buf, len): the console's factory data, PSPC's per-installation
// identity (console_id.h): the MAC address and the few other leaves the XMB reads.
int vshIdStorageLookup(u32 key, u32 offset, u32 buf, u32 len) {
	if (!mem::IsValidRange(buf, len)) return (int)0x80000023;  // vshbridge: a bad user pointer
	int r = console::IdStorageLookup((u16)key, offset, mem::GetPointer(buf, len), len);
	if (r < 0) mem::Memset(buf, 0, len);
	LOGD(LOG, "sceIdStorageLookup(key %04x, offset %x, %u bytes) -> %08x", key, offset, len, (u32)r);
	return r;
}

// The GE's EDRAM: PSPC models the 2 MB of the PSP-1000 (sceGeEdramGetHwSize).
u32 vshGeEdramGetHwSize() { return 0x00200000; }
int vshGeEdramSetSize(u32 size) {
	if (size != 0x00200000 && size != 0x00400000) return (int)0x80000104;
	return size == 0x00200000 ? 0 : (int)0x80000104;
}

// MagicGate memory stick audio (protected ATRAC on the memory stick): not supported.
int vshMSAudioInit(u32, u32) { return 0; }
int vshMSAudioEnd() { return 0; }
int vshMSAudioUnsupported() { return (int)SCE_KERNEL_ERROR_ERROR; }

// Formatting flash1 from System Settings: refused (PSPC's flash1 is a host folder).
int vshLflashFatfmtStartFatfmt(int, u32) {
	LOGW(LOG, "Formatting flash1 is not supported");
	return (int)SCE_KERNEL_ERROR_ERROR;
}

// Calls with no visible effect in PSPC (TV out, ME power, display/LCD tuning, debug flags...).
int vshBridgeNop() { return 0; }

// ---- LoadExec from the XMB ----
int vshKernelLoadExecVSHDisc(const char *file, u32 param) { return m33::LoadExecFromVsh(file, 0x120, param); }
int vshKernelLoadExecVSHDiscUpdater(const char *file, u32 param) { return m33::LoadExecFromVsh(file, 0x121, param); }
int vshKernelLoadExecVSHMs1(const char *file, u32 param) { return m33::LoadExecFromVsh(file, 0x140, param); }
int vshKernelLoadExecVSHMs2(const char *file, u32 param) { return m33::LoadExecFromVsh(file, 0x141, param); }
int vshKernelLoadExecVSHMs3(const char *file, u32 param) { return m33::LoadExecFromVsh(file, 0x142, param); }
int vshKernelLoadExecVSHMs4(const char *file, u32 param) { return m33::LoadExecFromVsh(file, 0x143, param); }
int vshKernelLoadExecVSHMs5(const char *file, u32 param) { return m33::LoadExecFromVsh(file, 0x144, param); }
int vshKernelLoadExecBufferVSHUsbWlan(u32 size, u32 buf, u32 param) {
	LOGW(LOG, "LoadExec of a %u-byte buffer from the XMB is not supported", size);
	return (int)SCE_KERNEL_ERROR_ERROR;
}
int vshKernelExitVSHVSH(u32 param) { return m33::ExitToVsh(); }
int vshLoadExec_6D8D3A3A(u32 a0, u32 a1) {
	LOGW(LOG, "LoadExecForKernel_6D8D3A3A(%08x, %08x) not supported", a0, a1);
	return (int)SCE_KERNEL_ERROR_ERROR;
}

// ---- sceResmgr ----

// Decrypts a buffer in place: the XMB's index file (flash0:/vsh/etc/index*.dat, "PSPsysGP" + KIRK
// type 2/9 like a module), which says what the XMB shows. *resultLength receives the decrypted
// size. The decryption runs in pspc-decrypt, as for encrypted modules. Behaviour after PPSSPP's
// sceResmgr.cpp (Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later) and JPCSP: an already
// plain buffer ("release:...") is left alone.
int sceResmgr_9DC14891(u32 buf, int size, u32 resultLengthPtr) {
	if (size < 0 || !mem::IsValidRange(buf, (u32)size)) return -1;
	const u8 *p = GPtr(buf);
	if (size >= 8 && memcmp(p, "release:", 8) == 0) {
		if (mem::IsValidRange(resultLengthPtr, 4)) GWrite32(resultLengthPtr, (u32)size);
		return 0;
	}
	std::vector<u8> in(p, p + size), out;
	std::string err;
	if (!kernel::DecryptWithTool(in, out, &err) || out.empty() || out.size() > (size_t)size) {
		LOGE(LOG, "sceResmgr: cannot decrypt %d bytes: %s", size, err.c_str());
		return -1;
	}
	mem::WriteBytes(buf, out.data(), (u32)out.size());
	if (mem::IsValidRange(resultLengthPtr, 4)) GWrite32(resultLengthPtr, (u32)out.size());
	const bool plausible = out.size() >= 8 && memcmp(out.data(), "release:", 8) == 0;
	LOGI(LOG, "sceResmgr: decrypted %d bytes to %zu%s", size, out.size(), plausible ? "" : " (not a release: index?)");
	return 0;
}

// A bridge export that forwards to an existing native function.
struct Forward {
	u32 nid;
	const char *name;
	const char *lib;
	u32 targetNid;
};

// sceVshBridge NID -> the kernel function vshbridge.prx calls (comment), implemented by PSPC as
// (library, NID).
const Forward kForwards[] = {
	{0x7EB0BE48, "vshCtrlGetSamplingMode", "sceCtrl", 0xDA6B76A1},          // sceCtrl_driver 4CEB8CC4
	{0x56099C98, "vshDisplayGetMode", "sceDisplay", 0xDEA197D4},            // sceDisplay_driver DEA197D4
	{0x07BF376F, "vshDisplaySetHoldMode", "sceDisplay_driver", 0x7ED59BC4}, // sceDisplay_driver 2E6AA0AB
	{0x2380DC08, "vshIoDevctl", "IoFileMgrForUser", 0x54F5FB11},            // IoFileMgrForKernel 54F5FB11
	{0x4A596D2D, "vshImposeSetParam", "sceImpose_driver", 0x810FB7FB},      // sceImpose_driver 9EB8A57B
	{0x639C3CB3, "vshImposeGetParam", "sceImpose_driver", 0x531C9778},      // sceImpose_driver 6F502C0A
	{0xAF7FF0A4, "vshImposeCheckVideoOut", "sceImpose_driver", 0x116CFF64}, // sceImpose_driver 4318B120
	{0xC1B2013D, "vshImposeSetVideoOutMode", "sceImpose_driver", 0x116DDED6}, // sceImpose_driver 63483FA5
	{0xF4DFFBBE, "vshImpose_FF1A2F07", "sceImpose_driver", 0xFF1A2F07},     // sceImpose_driver CCEAA9CD
	{0xC6722C70, "vshKernelGetModel", "SysMemForKernel", 0x6373995D},       // SysMemForKernel 4823B9D9
	{0xCC690947, "vshRtcSetAlarmTick", "sceRtc", 0x7D1FBED3},               // sceRtc_driver 55AC1C23
	{0xF01B78BB, "vshHprmReadLatch", "sceHprm", 0x40D2F9F0},                // sceHprm_driver A6E8D4F0
	{0x5C2983C2, "vshChkregCheckRegion", "sceChkreg_driver", 0x54495B19},   // sceChkreg_driver 54495B19
	{0x61001D64, "vshChkregGetPsCode", "sceChkreg_driver", 0x59F8491D},     // sceChkreg_driver 59F8491D
	{0xC949966C, "vshPowerSetWakeupCondition", "scePower_driver", 0xBA566CD0}, // scePower_driver 21246F84
	{0x5894C339, "vshImposeChanges", "sceImpose_driver", 0xB415FC59},       // sceImpose_driver CD1291FD
	{0xE47EA094, "vshIoIoctl", "IoFileMgrForUser", 0x63632449},             // IoFileMgrForKernel 63632449
	{0x4D4CB4F3, "vshVaudioChReserve", "sceVaudio", 0x03B6807D},            // sceVaudio_driver 03B6807D
	{0xB56B0B9A, "vshVaudioChRelease", "sceVaudio", 0x67585DFD},            // sceVaudio_driver 67585DFD
	{0xE61AA670, "vshVaudioOutputBlocking", "sceVaudio", 0x8986295E},       // sceVaudio_driver 8986295E
	// System Settings > Date & Time (rtc.cpp keeps the clock's offset from the host's).
	{0x06559655, "vshRtcSetCurrentTick", "sceRtc_driver", 0x38D062F0},      // sceRtc_driver 38D062F0
	{0xA0D8C9A9, "vshRtcSetConf", "sceRtc_driver", 0x8EB1DEAC},             // sceRtc_driver 8EB1DEAC
	{0x5AD46F41, "vshRtcGetCurrentSecureTick", "sceRtc_driver", 0x8D9645A2}, // sceRtc_driver 8D9645A2
};

}  // namespace

void Register_VshLibraries() {
	for (u32 &s : g_slots) s = 0;
	static const hle::HleFunction bridge[] = {
		HLEFN(0xA5628F0D, vshKernelLoadModuleVSH),             // ModuleMgrForKernel FE586962
		HLEFN(0xC6395C03, vshCtrlReadBufferPositive),          // sceCtrl_driver BA664B5E
		HLEFN(0xC1AC3EC5, vshBridgeRegisterSlot),
		HLEFN(0x9B273734, vshBridgeUnregisterSlot),
		HLEFN(0x3DA2D6F9, vshBridgeSetEventBit),               // ThreadManForKernel sceKernelSetEventFlag
		HLEFN(0x4DB43867, vshIdStorageLookup),                 // sceIdStorage_driver 6FE062D1
		HLEFN(0x3C15EDF7, vshGeEdramGetHwSize),                // sceGe_driver B5891BE0
		HLEFN(0x0576495B, vshGeEdramSetSize),                  // sceGe_driver 1DDA69CE
		HLEFN(0xCE32CBEF, vshMSAudioInit),                     // sceMSAudio_driver 10444C89
		HLEFN(0xE5DA5E95, vshMSAudioEnd),                      // sceMSAudio_driver BB5C735F
		HLEFN(0x6CAEB765, vshMSAudioUnsupported),              // sceMSAudio_driver D9759AAA (auth)
		HLEFN(0x53BFD101, vshMSAudioUnsupported),              // sceMSAudio_driver 7E4B6AB5 (check ICV)
		HLEFN(0xE174218C, vshMSAudioUnsupported),              // sceMSAudio_driver 36813DEA (check ICVn)
		HLEFN(0x119DBA5A, vshMSAudioUnsupported),              // sceMSAudio_driver E4FE1707
		HLEFN(0x74DA9D25, vshLflashFatfmtStartFatfmt),         // LflashFatfmt B7A424A4
		HLEFN(0x09D045AB, vshKernelLoadExecVSHDisc),           // LoadExecForKernel 6331FC3A
		HLEFN(0xDA65D079, vshKernelLoadExecVSHDiscUpdater),    // LoadExecForKernel 02320D56
		HLEFN(0x99E52885, vshKernelLoadExecVSHMs1),            // LoadExecForKernel 4D6C5A67
		HLEFN(0x721A112C, vshKernelLoadExecVSHMs2),            // LoadExecForKernel 5F2B2E14
		HLEFN(0xCCF3F8B2, vshKernelLoadExecVSHMs3),            // LoadExecForKernel 9DA0BBBD
		HLEFN(0x7170A3F9, vshKernelLoadExecVSHMs4),            // LoadExecForKernel 71528FED
		HLEFN(0xE9E9CC88, vshKernelLoadExecVSHMs5),            // LoadExecForKernel 2D00AF8E
		HLEFN(0xD59AD8B6, vshKernelLoadExecBufferVSHUsbWlan),  // LoadExecForKernel 578AE50B
		HLEFN(0x1D922BF8, vshKernelExitVSHVSH),                // LoadExecForKernel CA8011A2
		HLEFN(0x5821EDDD, vshLoadExec_6D8D3A3A),               // LoadExecForKernel 7973C886
		// No visible effect here.
		HLEFN(0x4ABE30D6, vshBridgeNop),  // sceCtrl_driver 9A0923A5 (one argument)
		HLEFN(0x0CC334AD, vshBridgeNop),  // sceDisplay_driver EE7FEFBE (a flag, 0)
		HLEFN(0xAE743835, vshBridgeNop),  // sceDisplay_driver 3A0E2277 (LCD setting)
		HLEFN(0xF0ABE363, vshBridgeNop),  // sceDisplay_driver 6E57C773 (a state word, 0)
		HLEFN(0xAE29207D, vshBridgeNop),  // SysMemForKernel 615B1CE4
		HLEFN(0xD86174C6, vshBridgeNop),  // SysMemForKernel C3A6C02A
		HLEFN(0x52D8F9F2, vshBridgeNop),  // sceHprm_driver 0E3E22E0
		HLEFN(0x2B38FDCE, vshBridgeNop),  // KDebugForKernel 00E6D91D
		HLEFN(0x14641922, vshBridgeNop),  // sceMePower_driver A765D5D4
		HLEFN(0xC396D3C7, vshBridgeNop),  // sceMePower_driver A21DBE51
		HLEFN(0xD4F70D80, vshBridgeNop),  // sceMePower_driver E2FC58E9
		HLEFN(0x34E8CD12, vshBridgeNop),  // sceMeCore_driver 8988AD49
		HLEFN(0x0D6E3AFD, vshBridgeNop),  // sceDve_driver 1C820E9E (TV out)
		HLEFN(0x167F86F3, vshBridgeNop),  // sceDve_driver 89CCB808
		HLEFN(0x3219A23C, vshBridgeNop),  // sceDve_driver 738FE169
		HLEFN(0x9DE0434C, vshBridgeNop),  // sceImpose_driver FDBE164F
		HLEFN(0x6CF81E5F, vshBridgeNop),  // sceAudio_driver CD272A5D
		HLEFN(0x1C022452, vshBridgeNop),  // scePower_driver 95204444
		HLEFN(0xEAED8B3A, vshBridgeNop),  // scePspNpDrm_driver 9A34AC9F sceNpDrmCheckRifTimeLimit
		// USB mass storage mode (no cable): nothing to start.
		HLEFN(0x0313836F, vshBridgeNop),  // sceUSB_Stor_Ms_driver 56AA41EA
		HLEFN(0x040C23BC, vshBridgeNop),  // sceUSB_Stor_Ms_driver 16173D42
		HLEFN(0x0D5B48FF, vshBridgeNop),  // sceUSB_Stor_Ms_driver 18EB011A
		HLEFN(0x25311177, vshBridgeNop),  // sceUSB_Stor_Ms_driver 4B10A7F5
		HLEFN(0x3F557935, vshBridgeNop),  // sceUSB_Stor_Ms_driver 762F7FDF
		HLEFN(0x4F052052, vshBridgeNop),  // sceUSB_Stor_Ms_driver 7B810720
		HLEFN(0x58757E07, vshBridgeNop),  // sceUSB_Stor_Ms_driver 576E7F6F
		HLEFN(0x84E21C00, vshBridgeNop),  // sceUSB_Stor_Ms_driver 9569F268
		HLEFN(0xC1997ADA, vshBridgeNop),  // sceUSB_Stor_Ms_driver ABE9F2C7
		HLEFN(0xD86CD00A, vshBridgeNop),  // sceUSB_Stor_Ms_driver FF0C3873
		HLEFN(0xE5360509, vshBridgeNop),  // sceUSB_Stor_Ms_driver 9C029B16
		// More MagicGate memory stick audio.
		HLEFN(0x7EA32357, vshMSAudioUnsupported),  // sceMSAudio_driver 9C3D547C (deauth)
		HLEFN(0x14C38DED, vshMSAudioUnsupported),  // sceMSAudio_driver E14AA83C
		HLEFN(0x233E5DEE, vshMSAudioUnsupported),  // sceMSAudio_driver 1B95B134
		HLEFN(0x27CD0D47, vshMSAudioUnsupported),  // sceMSAudio_driver C6730411
		HLEFN(0x31FA349F, vshMSAudioUnsupported),  // sceMSAudio_driver 1C0DB5A2
		HLEFN(0x431D76EE, vshMSAudioUnsupported),  // sceMSAudio_driver 8B5E5FAA
		HLEFN(0x841884A0, vshMSAudioUnsupported),  // sceMSAudio_driver F10BB4C4
		HLEFN(0xB3B00353, vshMSAudioUnsupported),  // sceMSAudio_driver F6157008
		HLEFN(0xBEB62554, vshMSAudioUnsupported),  // sceMSAudio_driver F987978D
		HLEFN(0xC48E36AC, vshMSAudioUnsupported),  // sceMSAudio_driver 51ACA4D0
		HLEFN(0xD3CE3DC0, vshMSAudioUnsupported),  // sceMSAudio_driver 6501A23C
		HLEFN(0xE9CF0AC2, vshMSAudioUnsupported),  // sceMSAudio_driver BA76AE40
		HLEFN(0xEE6A0501, vshMSAudioUnsupported),  // sceMSAudio_driver C78796D1
		HLEFN(0xFAEA0019, vshMSAudioUnsupported),  // sceMSAudio_driver F8A9FA0A
	};
	hle::RegisterModule("sceVshBridge", bridge);
	for (const Forward &f : kForwards) {
		const hle::HleFunction *t = hle::FindFunction(f.lib, f.targetNid);
		if (!t || !t->fn) {
			LOGW(LOG, "%s: %s::%08x is not implemented", f.name, f.lib, f.targetNid);
			continue;
		}
		hle::HleFunction fwd = *t;
		fwd.nid = f.nid;
		fwd.name = f.name;
		hle::RegisterModule("sceVshBridge", &fwd, 1);
	}

	static const hle::HleFunction resmgr[] = {
		HLEFN(0x9DC14891, sceResmgr_9DC14891),
	};
	hle::RegisterModule("sceResmgr", resmgr);
}
