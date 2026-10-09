// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <vector>

#include "common/common.h"

namespace display {

void Init();
u32 Vcount();
bool InVblank();
u32 CurrentHcount();
// Called on the emulation thread at every VBlank (after the frame is presented).
void SetVblankHook(std::function<void()> fn);
// Blocks the current thread for `count` VBlanks.
int WaitVblankStart(int count, bool cb);
void ConvertFramebuffer(u32 addr, int stride, int format, int width, int height, std::vector<u32> &out);

}  // namespace display
