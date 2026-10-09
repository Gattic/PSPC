// SPDX-License-Identifier: GPL-2.0-or-later
// Sub-interrupt handlers registered through InterruptManager.
#pragma once

#include "common/common.h"

namespace intr {

enum : int {
	PSP_GPIO_INT = 4,
	PSP_ATA_INT = 5,
	PSP_UMD_INT = 6,
	PSP_MSCM0_INT = 7,
	PSP_WLAN_INT = 8,
	PSP_AUDIO_INT = 10,
	PSP_I2C_INT = 12,
	PSP_SIRCS_INT = 14,
	PSP_SYSTIMER0_INT = 15,
	PSP_DMACPLUS_INT = 22,
	PSP_GE_INT = 25,
	PSP_VBLANK_INT = 30,
	PSP_MECODEC_INT = 31,
	PSP_HPREMOTE_INT = 36,
	PSP_NUMBER_INTERRUPTS = 67,
};

// Runs every enabled sub-handler of `intrNo` (in interrupt context, from the scheduler).
// Handlers receive (subIntrNo, arg).
void TriggerAll(int intrNo);
// Runs a specific sub handler with an explicit second argument (GE signal/finish).
void TriggerSub(int intrNo, int subIntrNo, u32 arg);
bool HasEnabledHandler(int intrNo, int subIntrNo);

}  // namespace intr
