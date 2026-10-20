// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "common/common.h"

namespace hostsys {
int ProcessId();
u64 MonotonicUs();
void SleepMs(unsigned ms);
bool LocalTime(time_t value, std::tm &out);
struct TimeZone { int standardMinutes = 0, offsetMinutes = 0; bool daylight = false; };
TimeZone LocalTimeZone();
#ifdef _WIN32
inline constexpr const char *ExecutableSuffix = ".exe";
inline constexpr const char *ModuleSuffix = ".dll";
#else
inline constexpr const char *ExecutableSuffix = "";
inline constexpr const char *ModuleSuffix = ".so";
#endif
// Runs an argv vector directly, never through a shell. Returns -1 on spawn/wait failure.
int RunProcess(const std::vector<std::string> &args);
// Starts a program without waiting for it: no input, its output appended to `log`, `workingDir` as
// its current folder. args[0] is the program's path. False if it could not be started.
bool StartProcess(const std::vector<std::string> &args, const std::filesystem::path &workingDir, const std::filesystem::path &log);
void *OpenModule(const std::filesystem::path &path);
void *ModuleSymbol(void *module, const char *name);
void CloseModule(void *module);

// Stackful guest threads run cooperatively on a single host thread. The floating-point
// environment must be preserved at each switch, just like general-purpose registers.
void *EnterFibers();
void LeaveFibers();
void *NewFiber(size_t stackSize, void (*entry)(void *), void *arg);
void FreeFiber(void *fiber);
void SwitchFiber(void *fiber);
void *CurrentFiber();

class WakeEvent {
public:
	void Signal();
	void WaitUs(u64 timeout);
private:
	std::mutex mutex_;
	std::condition_variable cv_;
	bool signalled_ = false;
};
}
