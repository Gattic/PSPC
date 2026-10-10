// SPDX-License-Identifier: GPL-2.0-or-later
//
// KIRK, the PSP's crypto engine, through the pspc-decrypt "kirk-service" subprocess. KIRK's
// algorithms and keys are in libkirk (GPL-3.0-or-later), which PSPC's runtime may not link; the
// service (tools/pspc-decrypt/src/kirk_service.h) runs each command and sends the result back over
// a pipe. It is started on the first command and lives until pspc exits.
#pragma once

#include "common/common.h"

namespace kirk {

// KIRK commands (sceUtilsBufferCopyWithRange's last argument).
enum : int {
	CMD_DECRYPT_PRIVATE = 1,
	CMD_ENCRYPT_IV_0 = 4,
	CMD_ENCRYPT_IV_FUSE = 5,
	CMD_DECRYPT_IV_0 = 7,
	CMD_DECRYPT_IV_FUSE = 8,
	CMD_SHA1_HASH = 11,
	CMD_PRNG = 14,
};
// KIRK_AES128CBC_HEADER.mode for commands 4-9.
enum : u32 {
	MODE_ENCRYPT_CBC = 4,
	MODE_DECRYPT_CBC = 5,
};

// Runs one KIRK command: out (outSize bytes) and in (inSize bytes) may be the same buffer, as on
// the PSP. Returns KIRK's result (0 = success), or -1 when the service can't be reached.
int Command(int cmd, u8 *out, int outSize, const u8 *in, int inSize);

// True once the service answered (or can be started).
bool Available();

}  // namespace kirk
