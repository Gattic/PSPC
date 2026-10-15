// SPDX-License-Identifier: GPL-2.0-or-later
//
// The servers pspc-launcher runs (see servers.h).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include "frontend/child_process.h"
#include <glib.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#endif

#include "frontend/servers.h"
#include "common/app_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "common/json.h"
#include "common/host.h"
#include "net/host_net.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

namespace servers {

namespace {

fs::path g_exeDir;
#ifdef _WIN32
HANDLE g_job = nullptr;
struct Proc {
	HANDLE process = nullptr;
	State state = State::Stopped;
	unsigned exitCode = 0;
	int port = 0;
};
#else
struct Proc {
	std::unique_ptr<frontend::ChildProcess> process;
	State state = State::Stopped;
	unsigned exitCode = 0;
	int port = 0;
};
#endif
Proc g_procs[kKinds];

struct Python {
	fs::path exe;
	std::wstring prefix;  // arguments before the script's (the py launcher: -3)
};
Python g_python;
fs::path g_serverDir;

std::wstring Widen(const std::string &s) {
	if (s.empty()) return {};
#ifndef _WIN32
	auto *ucs = g_utf8_to_ucs4(s.c_str(), -1, nullptr, nullptr, nullptr);
	if (!ucs) return L"Invalid UTF-8";
	std::wstring w;
	for (auto *p = ucs; *p; ++p) w.push_back((wchar_t)*p);
	g_free(ucs);
	return w;
#else
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
#endif
}

std::string Narrow(const std::wstring &w) {
	if (w.empty()) return {};
#ifndef _WIN32
	std::vector<gunichar> ucs(w.begin(), w.end());
	ucs.push_back(0);
	auto *utf8 = g_ucs4_to_utf8(ucs.data(), -1, nullptr, nullptr, nullptr);
	std::string s = utf8 ? utf8 : "Invalid Unicode";
	g_free(utf8);
	return s;
#else
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
#endif
}

// A command-line argument in quotes (the paths here have no quotes or trailing backslashes).
std::wstring Q(const std::wstring &s) { return L"\"" + s + L"\""; }
std::wstring Q(const fs::path &p) { return Q(Widen(PathToUtf8(p))); }

const wchar_t *FileName(Kind k) {
	return k == Kind::Network ? L"pspc_network" : k == Kind::Sfco ? L"sfco" : k == Kind::GameSpy ? L"gamespy" : L"adhoc";
}
const wchar_t *Module(Kind k) { return k == Kind::Network ? L"pspc_network" : k == Kind::GameSpy ? L"pspc_gamespy" : L"pspc_medius"; }
fs::path Database(Kind k) { return DataDir() / (std::wstring(FileName(k)) + L".sqlite"); }
fs::path StatusPath(Kind k) { return DataDir() / (std::wstring(FileName(k)) + L".status.json"); }

// The Python that comes with PSPC, else the newest Python 3.9+ installed for this user or for all,
// else the py launcher, else python.exe on PATH (not the Microsoft Store's stand-in).
Python FindPython() {
#ifndef _WIN32
	char *python = g_find_program_in_path("python3");
	Python result;
	if (python) result.exe = python;
	g_free(python);
	return result;
#else
	std::error_code ec;
	fs::path bundled = g_exeDir / "python" / "python.exe";
	if (fs::exists(bundled, ec)) return {bundled, L""};
	wchar_t buf[MAX_PATH];
	std::vector<fs::path> roots;
	if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) roots.push_back(fs::path(buf) / "Programs" / "Python");
	if (GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH)) roots.push_back(fs::path(buf));
	fs::path best;
	int bestMinor = -1;
	for (const fs::path &root : roots) {
		for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
			std::wstring name = it->path().filename().wstring();
			if (name.size() < 8 || _wcsnicmp(name.c_str(), L"Python3", 7) != 0) continue;
			int minor = _wtoi(name.c_str() + 7);
			if (minor >= 9 && minor > bestMinor && fs::exists(it->path() / "python.exe", ec)) {
				best = it->path() / "python.exe";
				bestMinor = minor;
			}
		}
	}
	if (!best.empty()) return {best, L""};
	if (GetWindowsDirectoryW(buf, MAX_PATH) && fs::exists(fs::path(buf) / "py.exe", ec)) return {fs::path(buf) / "py.exe", L"-3"};
	wchar_t found[MAX_PATH];
	if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, found, nullptr) && !wcsstr(found, L"WindowsApps")) return {found, L""};
	return {};
#endif
}

fs::path FindServerDir() {
#ifndef _WIN32
	auto path = apppaths::ResourceRoot(g_exeDir) / "server";
	return fs::is_regular_file(path / "pspc_network/__main__.py") ? path : fs::path();
#else
	std::error_code ec;
	for (fs::path p = g_exeDir; !p.empty(); p = p.parent_path()) {
		if (fs::exists(p / "server" / "pspc_network" / "__main__.py", ec)) return p / "server";
		if (p == p.root_path()) break;
	}
	return {};
#endif
}

// Whether something on this PC accepts TCP connections on the port.
bool PortOpen(int port) {
#ifndef _WIN32
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return false;
	sockaddr_in a{};
	a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	connect(fd, (sockaddr *)&a, sizeof(a));
	pollfd p{fd, POLLOUT, 0};
	int error = 1; socklen_t size = sizeof(error);
	bool open = poll(&p, 1, 100) > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
	close(fd);
	return open;
#else
	SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET) return false;
	u_long nonBlocking = 1;
	ioctlsocket(sock, FIONBIO, &nonBlocking);
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_port = htons((u_short)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	connect(sock, (sockaddr *)&a, sizeof(a));
	fd_set writable, failed;
	FD_ZERO(&writable);
	FD_ZERO(&failed);
	FD_SET(sock, &writable);
	FD_SET(sock, &failed);
	timeval tv{0, 100000};
	bool open = select(0, nullptr, &writable, &failed, &tv) > 0 && FD_ISSET(sock, &writable);
	closesocket(sock);
	return open;
#endif
}

std::wstring ComputerName() {
#ifndef _WIN32
	char name[256]{};
	return gethostname(name, sizeof(name) - 1) == 0 ? Widen(name) : L"this PC";
#else
	wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
	DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
	return GetComputerNameW(name, &n) ? std::wstring(name) : L"this PC";
#endif
}

#ifdef _WIN32
void AppendLog(Kind k, const std::string &line) {
	if (FILE *f = _wfopen(LogPath(k).c_str(), L"ab")) {
		fwrite(line.data(), 1, line.size(), f);
		fclose(f);
	}
}

std::string Now() {
	time_t t = time(nullptr);
	tm local{};
	localtime_s(&local, &t);
	char buf[32];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
	return buf;
}

// The PSPC Network server whose tickets the game server checks: the one this launcher runs, else the
// one the player signs in to; none: everyone signs in locally.
std::wstring TicketServer(const settings::Settings &s) {
	State st = g_procs[Index(Kind::Network)].state;
	if (st == State::Running || st == State::Starting) return NetworkUrl(s, true);
	return Widen(s.npServer);
}

#endif

// ---- JSON out (infrastructure.json)

std::string Dump(const json::Value &v, int indent) {
	std::string pad((size_t)indent * 2, ' '), inner((size_t)(indent + 1) * 2, ' ');
	switch (v.type()) {
	case json::Value::Type::Null: return "null";
	case json::Value::Type::Bool: return v.Bool() ? "true" : "false";
	case json::Value::Type::Number: {
		char buf[40];
		double n = v.Number();
		if (n == std::floor(n) && std::fabs(n) < 1e15) snprintf(buf, sizeof(buf), "%lld", (long long)n);
		else snprintf(buf, sizeof(buf), "%.17g", n);
		return buf;
	}
	case json::Value::Type::String: return json::Quote(v.String());
	case json::Value::Type::Array: {
		if (v.Array().empty()) return "[]";
		std::string out = "[";
		for (size_t i = 0; i < v.Array().size(); i++) out += (i ? ", " : "") + Dump(v.Array()[i], indent + 1);
		return out + "]";
	}
	case json::Value::Type::Object: {
		if (v.Members().empty()) return "{}";
		std::string out = "{\n";
		bool first = true;
		for (const auto &m : v.Members()) {
			out += (first ? "" : ",\n") + inner + json::Quote(m.first) + ": " + Dump(m.second, indent + 1);
			first = false;
		}
		return out + "\n" + pad + "}";
	}
	}
	return "null";
}

bool ListsId(const json::Value &game, const std::string &discId) {
	for (const char *key : {"ids", "known_working_ids", "other_ids", "not_working_ids"})
		for (const json::Value &id : game[key].Array())
			if (id.String("") == discId) return true;
	return false;
}

fs::path RedirectFile() {
#ifdef _WIN32
	return g_exeDir / "infrastructure.json";
#else
	return apppaths::ConfigRoot() / "infrastructure.json";
#endif
}

std::string ReadText(const fs::path &p) {
	std::vector<u8> d;
	if (!ReadWholeFile(p, d)) return "";
	return std::string(d.begin(), d.end());
}

}  // namespace

void Init(const fs::path &exeDir) {
	g_exeDir = exeDir;
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	g_python = FindPython();
	g_serverDir = FindServerDir();
#ifdef _WIN32
	// Children: unbuffered UTF-8 output for the logs, and no __pycache__ in PSPC's folder (also given
	// on their command lines: the Python that comes with PSPC ignores these, isolated by its ._pth file).
	SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
	SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
	SetEnvironmentVariableW(L"PYTHONDONTWRITEBYTECODE", L"1");
	// The servers end with the launcher, even if it crashes.
	g_job = CreateJobObjectW(nullptr, nullptr);
	if (g_job) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
		info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
	}
#endif
}

std::wstring PythonCommand() {
	if (g_python.exe.empty()) return L"";
	return Q(g_python.exe) + (g_python.prefix.empty() ? L"" : L" " + g_python.prefix);
}

std::wstring Missing(Kind k) {
	std::error_code ec;
	if (k == Kind::Adhoc) return fs::exists(g_exeDir / (std::string("pspc") + hostsys::ExecutableSuffix), ec) ? L"" : L"The PSPC runtime isn't next to the launcher.";
	if (g_serverDir.empty()) return L"The server's files (the server folder) aren't with this copy of PSPC.";
	if (g_python.exe.empty())
		return L"This server needs Python 3, which isn't installed. The PSPC installer brings its own; or install Python from "
		       L"python.org.";
	return L"";
}

fs::path DataDir() {
#ifdef _WIN32
	return g_exeDir / "servers";
#else
	return apppaths::DataRoot() / "servers";
#endif
}
fs::path LogPath(Kind k) {
#ifdef _WIN32
	return DataDir() / (std::wstring(FileName(k)) + L".log");
#else
	return apppaths::StateRoot() / "servers" / (std::wstring(FileName(k)) + L".log");
#endif
}

int Port(Kind k, const settings::Settings &s) {
	return k == Kind::Network ? s.hostNetworkPort : k == Kind::Sfco ? 10071 : k == Kind::GameSpy ? 29900 : kAdhocPort;
}

std::string Status(Kind k) {
	const Proc &p = g_procs[Index(k)];
	if (k != Kind::GameSpy || !p.process || p.state != State::Running) return "";
	return ReadText(StatusPath(k));
}

#ifdef _WIN32
bool Start(Kind k, const settings::Settings &s, const std::wstring &umd, std::wstring *error) {
	Proc &p = g_procs[Index(k)];
	Poll(k);
	if (p.process) return true;
	std::wstring missing = Missing(k);
	if (!missing.empty()) {
		*error = missing;
		return false;
	}
	const int port = Port(k, s);
	if (PortOpen(port)) {
		*error = L"Port " + std::to_wstring(port) +
		         L" on this PC is already in use. Is this server already running (perhaps started from a command prompt)?";
		return false;
	}
	std::error_code ec;
	fs::create_directories(DataDir(), ec);
	std::wstring cmd, cwd;
	if (k == Kind::Adhoc) {
		cmd = Q(g_exeDir / "pspc.exe") + L" --adhoc-server --adhoc-port " + std::to_wstring(kAdhocPort);
		cwd = g_exeDir.wstring();
	} else {
		cmd = Q(g_python.exe) + (g_python.prefix.empty() ? L"" : L" " + g_python.prefix) + L" -u -B -X utf8 -m " + Module(k) + L" --db " +
		      Q(Database(k)) + L" serve";
		if (k == Kind::Network) {
			cmd += L" --listen " + std::wstring(s.hostNetworkLan ? L"0.0.0.0:" : L"127.0.0.1:") + std::to_wstring(port) + L" --name " +
			       Q(L"PSPC Network on " + ComputerName());
		} else if (k == Kind::GameSpy) {
			cmd += L" --public-host " + (s.hostGameSpyAddress.empty() ? LanAddress() : Widen(s.hostGameSpyAddress)) + L" --status-file " +
			       Q(StatusPath(k));
			fs::remove(StatusPath(k), ec);
		} else {
			cmd += L" --public-host " + (s.hostSfcoAddress.empty() ? LanAddress() : Widen(s.hostSfcoAddress));
			if (!umd.empty()) cmd += L" --umd " + Q(umd);
			std::wstring tickets = TicketServer(s);
			if (!tickets.empty()) cmd += L" --network " + Q(tickets) + (s.hostSfcoAllowLocal ? L" --allow-local" : L"");
		}
		cwd = g_serverDir.wstring();
	}
	// The log: the previous one is kept once it gets large.
	if (fs::file_size(LogPath(k), ec) > (8u << 20)) fs::rename(LogPath(k), fs::path(LogPath(k)).replace_extension(L".old.log"), ec);
	AppendLog(k, "\r\n==== " + Now() + " started by the PSPC launcher: " + Narrow(cmd) + "\r\n");
	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	HANDLE log = CreateFileW(LogPath(k).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
	                         FILE_ATTRIBUTE_NORMAL, nullptr);
	HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = nul;
	si.hStdOutput = log;
	si.hStdError = log;
	PROCESS_INFORMATION pi{};
	BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi);
	DWORD err = GetLastError();
	if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
	if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
	if (!ok) {
		*error = L"Couldn't start the server (error " + std::to_wstring(err) + L"):\n" + cmd;
		return false;
	}
	if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	p.process = pi.hProcess;
	p.state = State::Starting;
	p.exitCode = 0;
	p.port = port;
	return true;
}

void Stop(Kind k) {
	Proc &p = g_procs[Index(k)];
	if (p.process) {
		TerminateProcess(p.process, 0);
		WaitForSingleObject(p.process, 5000);
		CloseHandle(p.process);
		p.process = nullptr;
		AppendLog(k, "==== " + Now() + " stopped by the PSPC launcher\r\n");
	}
	p.state = State::Stopped;
}

void StopAll() {
	for (int i = 0; i < kKinds; i++) Stop((Kind)i);
}

State Poll(Kind k) {
	Proc &p = g_procs[Index(k)];
	if (!p.process) return p.state;
	if (WaitForSingleObject(p.process, 0) == WAIT_OBJECT_0) {
		DWORD exitCode = 0;
		GetExitCodeProcess(p.process, &exitCode);
		p.exitCode = exitCode;
		CloseHandle(p.process);
		p.process = nullptr;
		p.state = State::Failed;
		AppendLog(k, "==== " + Now() + " the server exited (code " + std::to_string(p.exitCode) + ")\r\n");
		return p.state;
	}
	if (p.state == State::Starting && PortOpen(p.port)) p.state = State::Running;
	return p.state;
}

bool AnyRunning() {
	for (const Proc &p : g_procs)
		if (p.process) return true;
	return false;
}

unsigned ExitCode(Kind k) { return g_procs[Index(k)].exitCode; }

std::wstring LogTail(Kind k, int lines) {
	std::string text = ReadText(LogPath(k));
	if (text.size() > 16384) text.erase(0, text.size() - 16384);
	size_t pos = text.size();
	while (pos > 0 && (text[pos - 1] == '\n' || text[pos - 1] == '\r')) pos--;
	size_t start = pos;
	for (int n = 0; n < lines && start > 0; n++) {
		size_t nl = text.rfind('\n', start - 1);
		start = nl == std::string::npos ? 0 : nl;
	}
	std::string tail = text.substr(start, pos - start);
	while (!tail.empty() && (tail[0] == '\n' || tail[0] == '\r')) tail.erase(0, 1);
	return Widen(tail);
}

bool Admin(Kind k, const std::vector<std::wstring> &args, std::string *out, const std::string &input) {
	out->clear();
	std::wstring missing = k == Kind::Adhoc ? L"The ad hoc server has no admin commands." : Missing(k);
	if (!missing.empty()) {
		*out = Narrow(missing);
		return false;
	}
	std::error_code ec;
	fs::create_directories(DataDir(), ec);
	std::wstring cmd = Q(g_python.exe) + (g_python.prefix.empty() ? L"" : L" " + g_python.prefix) + L" -B -X utf8 -m " + Module(k) +
	                   L" --db " + Q(Database(k));
	for (const std::wstring &a : args) cmd += L" " + Q(a);
	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
	if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&inRead, &inWrite, &sa, 0)) {
		*out = "Couldn't create pipes";
		return false;
	}
	SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = inRead;
	si.hStdOutput = outWrite;
	si.hStdError = outWrite;
	PROCESS_INFORMATION pi{};
	BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g_serverDir.wstring().c_str(), &si, &pi);
	CloseHandle(outWrite);
	CloseHandle(inRead);
	if (!ok) {
		CloseHandle(outRead);
		CloseHandle(inWrite);
		*out = "Couldn't run " + Narrow(cmd);
		return false;
	}
	if (!input.empty()) {
		std::string line = input + "\n";
		DWORD written = 0;
		WriteFile(inWrite, line.data(), (DWORD)line.size(), &written, nullptr);
	}
	CloseHandle(inWrite);
	// Read until it exits (or 30 s pass).
	ULONGLONG deadline = GetTickCount64() + 30000;
	char buf[4096];
	for (;;) {
		DWORD avail = 0;
		if (PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
			DWORD got = 0;
			if (ReadFile(outRead, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) && got > 0) out->append(buf, got);
			continue;
		}
		if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
			// What's left after it exited.
			DWORD got = 0;
			while (PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
			       ReadFile(outRead, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) && got > 0)
				out->append(buf, got);
			break;
		}
		if (GetTickCount64() > deadline) {
			TerminateProcess(pi.hProcess, 1);
			*out += "\n(timed out)";
			break;
		}
	}
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(outRead);
	return code == 0;
}

#else
bool Start(Kind k, const settings::Settings &s, const std::wstring &umd, std::wstring *error) {
	auto &p = g_procs[Index(k)];
	Poll(k);
	if (p.process) return true;
	*error = Missing(k);
	if (!error->empty()) return false;
	p.port = Port(k, s);
	if (PortOpen(p.port)) { *error = L"Port " + std::to_wstring(p.port) + L" is already in use."; return false; }
	std::error_code ec;
	fs::create_directories(DataDir(), ec);
	if (ec) { *error = Widen(ec.message()); return false; }
	std::vector<std::string> args;
	fs::path cwd;
	if (k == Kind::Adhoc) {
		args = {(g_exeDir / "pspc").string(), "--adhoc-server", "--adhoc-port", std::to_string(kAdhocPort)};
		cwd = g_exeDir;
	} else {
		args = {g_python.exe.string(), "-u", "-B", "-X", "utf8", "-m", Narrow(Module(k)), "--db", Database(k).string(), "serve"};
		cwd = g_serverDir;
		if (k == Kind::Network) {
			args.insert(args.end(), {"--listen", std::string(s.hostNetworkLan ? "0.0.0.0:" : "127.0.0.1:") + std::to_string(p.port),
			                         "--name", "PSPC Network on " + Narrow(ComputerName())});
		} else {
			const auto &address = k == Kind::GameSpy ? s.hostGameSpyAddress : s.hostSfcoAddress;
			args.insert(args.end(), {"--public-host", address.empty() ? Narrow(LanAddress()) : address});
			if (k == Kind::GameSpy) {
				args.insert(args.end(), {"--status-file", StatusPath(k).string()});
				std::error_code ec; fs::remove(StatusPath(k), ec);
			} else {
				if (!umd.empty()) args.insert(args.end(), {"--umd", Narrow(umd)});
				bool localNetwork = g_procs[Index(Kind::Network)].process != nullptr;
				std::string tickets = localNetwork ? Narrow(NetworkUrl(s, true)) : s.npServer;
				if (!tickets.empty()) {
					args.insert(args.end(), {"--network", tickets});
					if (s.hostSfcoAllowLocal) args.push_back("--allow-local");
				}
			}
		}
	}
	auto child = std::make_unique<frontend::ChildProcess>();
	std::string why;
	if (!child->Start(args, cwd, LogPath(k), &why)) { *error = Widen(why); return false; }
	p.process = std::move(child);
	p.state = State::Starting;
	p.exitCode = 0;
	return true;
}
void Stop(Kind k) {
	auto &p = g_procs[Index(k)];
	if (p.process) p.process->Cancel();
}
void StopAll() {
	for (auto &p : g_procs) { p.process.reset(); p.state = State::Stopped; }
}
State Poll(Kind k) {
	auto &p = g_procs[Index(k)];
	if (!p.process) return p.state;
	p.process->Poll();
	if (!p.process->Running()) {
		p.exitCode = p.process->ExitCode();
		p.state = p.process->Cancelled() ? State::Stopped : State::Failed;
		p.process.reset();
	} else if (p.state == State::Starting && PortOpen(p.port)) p.state = State::Running;
	return p.state;
}
bool AnyRunning() {
	for (auto &p : g_procs) if (p.process) return true;
	return false;
}
unsigned ExitCode(Kind k) { return g_procs[Index(k)].exitCode; }
std::wstring LogTail(Kind k, int lines) {
	std::string text = ReadText(LogPath(k));
	size_t start = text.size();
	for (int i = 0; i < lines && start; ++i) {
		size_t pos = text.rfind('\n', start - 1);
		start = pos == std::string::npos ? 0 : pos;
	}
	text = text.substr(start);
	char *valid = g_utf8_make_valid(text.data(), text.size());
	auto result = Widen(valid);
	g_free(valid);
	return result;
}
bool Admin(Kind k, const std::vector<std::wstring> &args, std::string *out, const std::string &input) {
	if (k == Kind::Adhoc) { *out = "The ad hoc server has no administration commands."; return false; }
	auto missing = Missing(k);
	if (!missing.empty()) { *out = Narrow(missing); return false; }
	std::error_code ec;
	fs::create_directories(DataDir(), ec);
	frontend::ChildProcess process;
	std::vector<std::string> command{g_python.exe.string(), "-B", "-X", "utf8", "-m", Narrow(Module(k)), "--db", Database(k).string()};
	for (const auto &arg : args) command.push_back(Narrow(arg));
	if (!process.Start(command, g_serverDir, {}, out, input.empty() ? "" : input + "\n")) return false;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (process.Running()) {
		process.Poll();
		if (std::chrono::steady_clock::now() > deadline) process.Cancel();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	*out = process.Output();
	return process.ExitCode() == 0;
}
#endif

std::wstring LanAddress() {
	hostnet::HostConfig hc = hostnet::QueryHostConfig();
	return hc.valid && hc.ip ? Widen(hostnet::Ipv4ToString(hc.ip)) : L"127.0.0.1";
}

std::wstring NetworkUrl(const settings::Settings &s, bool forThisPc) {
	std::wstring host = forThisPc || !s.hostNetworkLan ? L"127.0.0.1" : LanAddress();
	return L"http://" + host + L":" + std::to_wstring(s.hostNetworkPort);
}

std::string Redirect(const std::string &discId, const std::string &domain) {
	json::Value root = json::Value::Parse(ReadText(RedirectFile()));
	for (const json::Value &g : root["games"].Array())
		if (ListsId(g, discId)) return g["domains"][domain].String("");
	return "";
}

bool SetRedirect(const std::string &gameName, const std::string &discId, const std::string &domain, const std::string &address,
                 std::wstring *error) {
	std::string old = ReadText(RedirectFile());
	json::Value root = json::Value::Parse(old);
	if (!old.empty() && root.IsNull()) {
		*error = L"The user infrastructure.json isn't valid JSON; fix it before changing routes.";
		return false;
	}
	std::vector<std::string> members;
	if (!root.Has("comment"))
		members.push_back("  \"comment\": " + json::Quote("Where games' online services go (docs/pspc-network.md). The launcher's Online page "
		                                                  "writes the redirects to servers; other entries are kept."));
	for (const auto &m : root.Members())
		if (m.first != "games") members.push_back("  " + json::Quote(m.first) + ": " + Dump(m.second, 1));
	std::vector<std::string> games;
	bool found = false;
	for (const json::Value &g : root["games"].Array()) {
		if (found || !ListsId(g, discId)) {
			games.push_back("    " + Dump(g, 2));
			continue;
		}
		found = true;
		// This game's entry: its domains with this one replaced; the entry goes when nothing's left.
		std::vector<std::string> domains;
		for (const auto &d : g["domains"].Members())
			if (d.first != domain) domains.push_back(json::Quote(d.first) + ": " + Dump(d.second, 3));
		if (!address.empty()) domains.push_back(json::Quote(domain) + ": " + json::Quote(address));
		if (domains.empty() && !g.Has("dns")) continue;
		std::string entry = "    {";
		bool first = true;
		for (const auto &m : g.Members()) {
			if (m.first == "domains") continue;
			entry += std::string(first ? "\n" : ",\n") + "      " + json::Quote(m.first) + ": " + Dump(m.second, 3);
			first = false;
		}
		std::string d;
		for (size_t i = 0; i < domains.size(); i++) d += (i ? ", " : "") + domains[i];
		entry += std::string(first ? "\n" : ",\n") + "      \"domains\": {" + d + "}\n    }";
		games.push_back(entry);
	}
	if (!found && !address.empty())
		games.push_back("    {\n      \"name\": " + json::Quote(gameName) + ",\n      \"ids\": [" + json::Quote(discId) +
		                "],\n      \"domains\": {" + json::Quote(domain) + ": " + json::Quote(address) + "}\n    }");
	std::string g = "  \"games\": [";
	for (size_t i = 0; i < games.size(); i++) g += (i ? ",\n" : "\n") + games[i];
	g += games.empty() ? "]" : "\n  ]";
	members.push_back(g);
	std::string text = "{\n";
	for (size_t i = 0; i < members.size(); i++) text += members[i] + (i + 1 < members.size() ? ",\n" : "\n");
	text += "}\n";
	if (!WriteWholeFile(RedirectFile(), text.data(), text.size())) {
		*error = L"Couldn't write " + Widen(PathToUtf8(RedirectFile()));
		return false;
	}
	return true;
}

}  // namespace servers
