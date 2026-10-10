// SPDX-License-Identifier: GPL-2.0-or-later
// The client side of pspc-decrypt's kirk-service (see kirk_client.h and
// tools/pspc-decrypt/src/kirk_service.h for the protocol).
#include "runtime/hle/modules/kirk_client.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "common/log.h"
#include "runtime/hle/modules/console_id.h"
#include "runtime/kernel/module.h"

namespace kirk {

namespace {

constexpr const char *LOG = "KIRK";
constexpr u32 kRequest = 0x5152494B;   // 'KIRQ'
constexpr u32 kResponse = 0x5352494B;  // 'KIRS'
constexpr u32 kInPlace = 1;
constexpr u32 kMaxBuffer = 16u << 20;

std::mutex g_lock;
HANDLE g_process = nullptr;
HANDLE g_toService = nullptr;    // our end of its stdin
HANDLE g_fromService = nullptr;  // our end of its stdout
bool g_failed = false;           // couldn't start it: don't retry every call

void Put32(u8 *p, u32 v) { memcpy(p, &v, 4); }
u32 Get32(const u8 *p) {
	u32 v;
	memcpy(&v, p, 4);
	return v;
}

void Stop() {
	if (g_toService) CloseHandle(g_toService);
	if (g_fromService) CloseHandle(g_fromService);
	if (g_process) {
		if (WaitForSingleObject(g_process, 2000) != WAIT_OBJECT_0) TerminateProcess(g_process, 1);
		CloseHandle(g_process);
	}
	g_toService = g_fromService = g_process = nullptr;
}

bool Start() {
	const std::string &tool = kernel::DecryptToolPath();
	std::error_code ec;
	if (tool.empty() || !std::filesystem::exists(std::filesystem::u8path(tool), ec)) {
		LOGW(LOG, "pspc-decrypt.exe not found: no KIRK engine (savedata crypto and hashes fail)");
		return false;
	}
	// Pipes: only the child's ends are inheritable, and only they are handed to it.
	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	HANDLE childIn = nullptr, childOut = nullptr, ourIn = nullptr, ourOut = nullptr;
	if (!CreatePipe(&childIn, &ourOut, &sa, 1 << 16) || !CreatePipe(&ourIn, &childOut, &sa, 1 << 16)) {
		LOGE(LOG, "CreatePipe failed (%lu)", GetLastError());
		for (HANDLE h : {childIn, childOut, ourIn, ourOut})
			if (h) CloseHandle(h);
		return false;
	}
	SetHandleInformation(ourIn, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(ourOut, HANDLE_FLAG_INHERIT, 0);
	HANDLE inherit[2] = {childIn, childOut};
	SIZE_T attrSize = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
	std::vector<u8> attrBuf(attrSize);
	auto *attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
	bool attrsOk = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
	               UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit),
	                                         nullptr, nullptr);
	STARTUPINFOEXW si{};
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = childIn;
	si.StartupInfo.hStdOutput = childOut;
	si.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	si.lpAttributeList = attrsOk ? attrs : nullptr;
	char fuse[32];
	snprintf(fuse, sizeof(fuse), "%016llX", (unsigned long long)console::Get().fuseId);
	std::wstring cmd = L"\"" + std::filesystem::u8path(tool).wstring() + L"\" kirk-service --fuse " +
	                   std::filesystem::path(fuse).wstring();
	PROCESS_INFORMATION pi{};
	const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
	                               CREATE_NO_WINDOW | (attrsOk ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, nullptr,
	                               &si.StartupInfo, &pi);
	if (attrsOk) DeleteProcThreadAttributeList(attrs);
	CloseHandle(childIn);
	CloseHandle(childOut);
	if (!ok) {
		LOGE(LOG, "Cannot start pspc-decrypt kirk-service (%lu)", GetLastError());
		CloseHandle(ourIn);
		CloseHandle(ourOut);
		return false;
	}
	CloseHandle(pi.hThread);
	g_process = pi.hProcess;
	g_toService = ourOut;
	g_fromService = ourIn;
	LOGI(LOG, "KIRK service started (pspc-decrypt kirk-service, pid %lu)", pi.dwProcessId);
	return true;
}

bool WriteAll(const void *data, size_t size) {
	const u8 *p = (const u8 *)data;
	while (size) {
		DWORD n = 0;
		if (!WriteFile(g_toService, p, (DWORD)std::min<size_t>(size, 1u << 20), &n, nullptr) || n == 0) return false;
		p += n;
		size -= n;
	}
	return true;
}

bool ReadAll(void *data, size_t size) {
	u8 *p = (u8 *)data;
	while (size) {
		DWORD n = 0;
		if (!ReadFile(g_fromService, p, (DWORD)std::min<size_t>(size, 1u << 20), &n, nullptr) || n == 0) return false;
		p += n;
		size -= n;
	}
	return true;
}

// One request/response; false if the pipe broke.
bool Exchange(int cmd, u8 *out, int outSize, const u8 *in, int inSize, int *result) {
	const bool inPlace = out == in;
	u8 head[20];
	Put32(head, kRequest);
	Put32(head + 4, (u32)cmd);
	Put32(head + 8, inPlace ? kInPlace : 0);
	Put32(head + 12, (u32)inSize);
	Put32(head + 16, (u32)outSize);
	if (!WriteAll(head, sizeof(head)) || !WriteAll(in, (size_t)inSize)) return false;
	if (!inPlace && !WriteAll(out, (size_t)outSize)) return false;
	u8 reply[12];
	if (!ReadAll(reply, sizeof(reply)) || Get32(reply) != kResponse || Get32(reply + 8) != (u32)outSize) return false;
	if (!ReadAll(out, (size_t)outSize)) return false;
	*result = (int)Get32(reply + 4);
	return true;
}

}  // namespace

bool Available() {
	std::lock_guard<std::mutex> guard(g_lock);
	if (g_process) return true;
	if (g_failed) return false;
	if (!Start()) g_failed = true;
	return g_process != nullptr;
}

int Command(int cmd, u8 *out, int outSize, const u8 *in, int inSize) {
	if (outSize < 0 || inSize < 0 || (u32)outSize > kMaxBuffer || (u32)inSize > kMaxBuffer) return -1;
	if ((outSize && !out) || (inSize && !in)) return -1;
	// The PSP's in-place commands pass the same buffer; a shorter output then fits in the input.
	if (out == in && outSize > inSize) return -1;
	std::lock_guard<std::mutex> guard(g_lock);
	for (int attempt = 0; attempt < 2; attempt++) {
		if (!g_process) {
			if (g_failed || !Start()) {
				g_failed = true;
				return -1;
			}
		}
		int result = -1;
		if (Exchange(cmd, out, outSize, in, inSize, &result)) return result;
		// The service died (or answered nonsense): restart it once.
		LOGW(LOG, "KIRK service connection lost; restarting it");
		Stop();
	}
	g_failed = true;
	return -1;
}

}  // namespace kirk
