// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/host.h"
#include "common/fileutil.h"
#include "common/log.h"
#include <chrono>
#include <thread>
#include <cfenv>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <ucontext.h>
extern char **environ;
#endif

namespace hostsys {
int ProcessId() {
#ifdef _WIN32
	return (int)GetCurrentProcessId();
#else
	return (int)getpid();
#endif
}
u64 MonotonicUs() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void SleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
bool LocalTime(time_t value, std::tm &out) {
#ifdef _WIN32
	return localtime_s(&out, &value) == 0;
#else
	return localtime_r(&value, &out) != nullptr;
#endif
}
TimeZone LocalTimeZone() {
	TimeZone out;
#ifdef _WIN32
	TIME_ZONE_INFORMATION tzi{};
	DWORD r = GetTimeZoneInformation(&tzi);
	out.daylight = r == TIME_ZONE_ID_DAYLIGHT;
	out.standardMinutes = -(int)tzi.Bias;
	out.offsetMinutes = -(int)(tzi.Bias + (out.daylight ? tzi.DaylightBias : tzi.StandardBias));
#else
	time_t now = time(nullptr);
	std::tm local{};
	LocalTime(now, local);
	out.daylight = local.tm_isdst > 0;
	out.offsetMinutes = (int)(local.tm_gmtoff / 60);
	// Let libc determine the actual standard-time offset (not all DST adjustments are an hour).
	auto standard = local;
	standard.tm_isdst = 0;
	time_t asStandard = mktime(&standard);
	out.standardMinutes = out.offsetMinutes - (int)((asStandard - now) / 60);
#endif
	return out;
}
int RunProcess(const std::vector<std::string> &args) {
	if (args.empty()) return -1;
#ifdef _WIN32
	std::wstring cmd;
	for (const auto &s : args) {
		if (!cmd.empty()) cmd += L' ';
		cmd += L'"';
		size_t slashes = 0;
		for (wchar_t c : fs::u8path(s).wstring()) {
			if (c == L'\\') { ++slashes; continue; }
			cmd.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
			slashes = 0; cmd += c;
		}
		cmd.append(slashes * 2, L'\\'); cmd += L'"';
	}
	STARTUPINFOW si{}; si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return -1;
	CloseHandle(pi.hThread);
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD result = 1; GetExitCodeProcess(pi.hProcess, &result); CloseHandle(pi.hProcess);
	return (int)result;
#else
	std::vector<char *> argv;
	for (const auto &s : args) argv.push_back(const_cast<char *>(s.c_str()));
	argv.push_back(nullptr);
	pid_t pid;
	int error = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
	if (error) { errno = error; return -1; }
	int status;
	while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}
void *OpenModule(const fs::path &path) {
#ifdef _WIN32
	return LoadLibraryW(path.c_str());
#else
	return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
void *ModuleSymbol(void *module, const char *name) {
#ifdef _WIN32
	return (void *)GetProcAddress((HMODULE)module, name);
#else
	return dlsym(module, name);
#endif
}
void CloseModule(void *module) {
#ifdef _WIN32
	FreeLibrary((HMODULE)module);
#else
	dlclose(module);
#endif
}

#ifdef _WIN32
struct FiberStart { void (*entry)(void *); void *arg; };
static void WINAPI FiberEntry(void *p) {
	auto start = *(FiberStart *)p;
	delete (FiberStart *)p;
	start.entry(start.arg);
	Fatal("Guest fiber returned to host");
}
void *EnterFibers() { return ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH); }
void LeaveFibers() { ConvertFiberToThread(); }
void *NewFiber(size_t size, void (*entry)(void *), void *arg) {
	auto *start = new FiberStart{entry, arg};
	void *f = CreateFiberEx(64 * 1024, size, FIBER_FLAG_FLOAT_SWITCH, FiberEntry, start);
	if (!f) delete start;
	return f;
}
void FreeFiber(void *f) { DeleteFiber(f); }
void SwitchFiber(void *f) { SwitchToFiber(f); }
void *CurrentFiber() { return GetCurrentFiber(); }
#else
struct Fiber {
	ucontext_t context{};
	fenv_t fp{};
	void *mapping = nullptr;
	size_t mappingSize = 0;
	void (*entry)(void *) = nullptr;
	void *arg = nullptr;
};
static thread_local Fiber mainFiber;
static thread_local Fiber *current = nullptr;
static void FiberEntry() {
	current->entry(current->arg);
	Fatal("Guest fiber returned to host");
}
void *EnterFibers() {
	if (getcontext(&mainFiber.context)) return nullptr;
	fegetenv(&mainFiber.fp);
	current = &mainFiber;
	return current;
}
void LeaveFibers() { current = nullptr; }
void *NewFiber(size_t size, void (*entry)(void *), void *arg) {
	auto *f = new Fiber;
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	size = (size + page - 1) / page * page;
	f->mappingSize = size + 2 * page;
	f->mapping = mmap(nullptr, f->mappingSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (f->mapping == MAP_FAILED) { delete f; return nullptr; }
	void *stack = (char *)f->mapping + page;
	if (mprotect(stack, size, PROT_READ | PROT_WRITE) || getcontext(&f->context)) {
		munmap(f->mapping, f->mappingSize); delete f; return nullptr;
	}
	f->context.uc_stack.ss_sp = stack; f->context.uc_stack.ss_size = size;
	f->context.uc_link = nullptr; f->entry = entry; f->arg = arg;
	fegetenv(&f->fp);
	makecontext(&f->context, FiberEntry, 0);
	return f;
}
void FreeFiber(void *p) {
	auto *f = (Fiber *)p;
	if (!f) return;
	munmap(f->mapping, f->mappingSize); delete f;
}
void SwitchFiber(void *p) {
	auto *from = current;
	auto *to = (Fiber *)p;
	if (!from || !to) Fatal("Invalid guest fiber switch");
	fegetenv(&from->fp);
	current = to;
	if (swapcontext(&from->context, &to->context)) Fatal("swapcontext failed");
	fesetenv(&from->fp);
}
void *CurrentFiber() { return current; }
#endif

void WakeEvent::Signal() {
	std::lock_guard lock(mutex_);
	signalled_ = true; cv_.notify_one();
}
void WakeEvent::WaitUs(u64 us) {
	std::unique_lock lock(mutex_);
	cv_.wait_for(lock, std::chrono::microseconds(us), [&] { return signalled_; });
	signalled_ = false;
}
}
