// SPDX-License-Identifier: GPL-3.0-or-later
// pspc-decrypt kirk-service: the KIRK crypto engine as a long-running service on stdin/stdout.
//
// PSPC's runtime (GPL-2.0-or-later) may not link libkirk (GPL-3.0-or-later), so the libraries that
// need KIRK at run time (sceChnnlsv, the "semaphore" library's sceUtilsBufferCopyWithRange) send
// each command to this process through a pipe. The protocol is binary, little-endian:
//
//   request:  u32 magic 'KIRQ' (0x5152494B), u32 command, u32 flags, u32 inSize, u32 outSize,
//             inSize bytes of input, then (unless flags & KIRK_SERVICE_IN_PLACE) outSize bytes,
//             the output buffer's current contents
//   response: u32 magic 'KIRS' (0x5352494B), s32 result (the KIRK return code; 0 = success),
//             u32 outSize, outSize bytes: the output buffer after the command
//
// With KIRK_SERVICE_IN_PLACE the output buffer is the input buffer (outSize <= inSize), as when a
// PSP program passes the same address for both. Command 0 is a ping (result 0, no output).
// Commands 1-17 are KIRK's. 5 and 8 (AES with the console's own key, which libkirk lacks) use a
// key derived from the fuse ID given on the command line, so data bound to "this console" stays
// readable by the same PSPC installation.
//
// The service exits when stdin reaches end of file (the runtime closed its end of the pipe).
#pragma once

#include "common.h"

constexpr u32 KIRK_SERVICE_REQUEST = 0x5152494B;
constexpr u32 KIRK_SERVICE_RESPONSE = 0x5352494B;
constexpr u32 KIRK_SERVICE_IN_PLACE = 1;
constexpr u32 KIRK_SERVICE_MAX_BUFFER = 16u << 20;

// Runs one KIRK command on caller-owned buffers (out may equal in). Shared with the tests.
struct KirkServiceState;
KirkServiceState *KirkServiceCreate(u64 fuseId);
void KirkServiceDestroy(KirkServiceState *s);
int KirkServiceCommand(KirkServiceState *s, int cmd, u8 *out, int outSize, u8 *in, int inSize);

// The request loop on the process's stdin/stdout. Returns the exit code.
int RunKirkService(u64 fuseId);
