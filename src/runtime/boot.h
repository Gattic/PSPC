// SPDX-License-Identifier: GPL-2.0-or-later
// Starting the main executable, and LoadExec-style restarts.
#pragma once

#include <string>
#include <vector>

#include "common/common.h"
#include "runtime/kernel/kobject.h"

namespace boot {

constexpr int kExitLoadExec = 0x4C4F4144;  // scheduler exit code meaning "restart with a new executable"

// Loads `image` as the main module and starts it. `pspPath` becomes argv[0].
SceUID BootModule(const std::vector<u8> &image, const std::string &pspPath);

void RequestLoadExec(const std::string &pspPath, const std::vector<u8> &args);
bool TakeLoadExecRequest(std::string &pspPath, std::vector<u8> &args);
SceUID ExitCallback();

}  // namespace boot
