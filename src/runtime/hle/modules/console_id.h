// SPDX-License-Identifier: GPL-2.0-or-later
//
// The identity of "this PSP": what a real console keeps in its factory ID storage (the WLAN MAC
// address, ...) and in KIRK's fuses. PSPC generates one per installation, the first time it is
// needed, and keeps it in flash1:/pspc_idstorage.txt, so the XMB's System Information shows a
// stable MAC address and data bound to the console (savedata hashes made with KIRK's console key)
// stays valid across runs. Deleting the file makes a new identity.
#pragma once

#include "common/common.h"

namespace console {

struct Identity {
	u8 mac[6];   // IdStorage key 0x0044
	u64 fuseId;  // KIRK fuse ID (fuse registers 0x90/0x94)
};

// Loaded (or created) on first use; flash1: must be mounted by then.
const Identity &Get();

// sceIdStorageLookup(key, offset, buf, len) for the leaves PSPC models. Returns 0, or
// 0x80000025 for a key this console doesn't have.
int IdStorageLookup(u16 key, u32 offset, u8 *buf, u32 len);

}  // namespace console
