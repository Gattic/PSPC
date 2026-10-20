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
#include <fcntl.h>
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
#ifdef _WIN32
// A command line that CreateProcess hands to the program as exactly these arguments.
static std::wstring CommandLine(const std::vector<std::string> &args) {
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
	return cmd;
}
#endif
int RunProcess(const std::vector<std::string> &args) {
	if (args.empty()) return -1;
#ifdef _WIN32
	std::wstring cmd = CommandLine(args);
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
bool StartProcess(const std::vector<std::string> &args, const fs::path &workingDir, const fs::path &log) {
	if (args.empty()) return false;
	std::error_code ec;
	fs::create_directories(log.parent_path(), ec);
#ifdef _WIN32
	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	HANDLE out = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
	                         FILE_ATTRIBUTE_NORMAL, nullptr);
	HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	if (out == INVALID_HANDLE_VALUE || in == INVALID_HANDLE_VALUE) {
		if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
		if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
		return false;
	}
	// Only these two handles go to the program: not this process's sockets, files or pipes.
	HANDLE inherit[2] = {in, out};
	SIZE_T attrSize = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
	std::vector<u8> attrBuf(attrSize);
	auto *attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
	const bool attrsOk = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
	                     UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
	STARTUPINFOEXW si{};
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = in;
	si.StartupInfo.hStdOutput = out;
	si.StartupInfo.hStdError = out;
	si.lpAttributeList = attrsOk ? attrs : nullptr;
	std::wstring cmd = CommandLine(args);
	PROCESS_INFORMATION pi{};
	const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
	                               CREATE_NO_WINDOW | (attrsOk ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
	                               workingDir.empty() ? nullptr : workingDir.c_str(), &si.StartupInfo, &pi);
	if (attrsOk) DeleteProcThreadAttributeList(attrs);
	CloseHandle(in);
	CloseHandle(out);
	if (!ok) return false;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
#else
	std::vector<char *> argv;
	for (const auto &s : args) argv.push_back(const_cast<char *>(s.c_str()));
	argv.push_back(nullptr);
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
	posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
	// Only the standard streams go to the program: guest sockets and files here may not be close-on-exec.
	std::vector<int> open;
	for (fs::directory_iterator it("/proc/self/fd", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
		const int fd = atoi(it->path().filename().c_str());
		if (fd > STDERR_FILENO) open.push_back(fd);
	}
	for (int fd : open)
		if (fcntl(fd, F_GETFD) >= 0) posix_spawn_file_actions_addclose(&actions, fd);  // not the directory iterator's, closed by now
	if (!workingDir.empty()) posix_spawn_file_actions_addchdir_np(&actions, workingDir.c_str());
	pid_t pid;
	const int error = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	return error == 0;
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
