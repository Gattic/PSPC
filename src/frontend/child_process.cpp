// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontend/child_process.h"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace frontend {
namespace {
void Close(int &fd) {
	if (fd >= 0)
		close(fd);
	fd = -1;
}
std::string Executable(const std::string &name) {
	if (name.find('/') != std::string::npos)
		return name;
	std::string paths = getenv("PATH") ? getenv("PATH") : "";
	size_t start = 0;
	do {
		size_t end = paths.find(':', start);
		auto path = std::filesystem::path(paths.substr(start, end - start)) / name;
		if (access(path.c_str(), X_OK) == 0)
			return std::filesystem::absolute(path).string();
		if (end == std::string::npos)
			break;
		start = end + 1;
	} while (start <= paths.size());
	return {};
}
} // namespace
ChildProcess::~ChildProcess() {
	if (pid_ > 0) {
		kill(-pid_, SIGKILL);
		while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
		}
	}
	Close(read_);
	Close(write_);
}
bool ChildProcess::Start(const std::vector<std::string> &args, const std::filesystem::path &cwd,
						 const std::filesystem::path &log, std::string *error, const std::string &input) {
	if (Running() || args.empty()) {
		*error = "Invalid process start";
		return false;
	}
	auto executable = Executable(args.front());
	if (executable.empty()) {
		*error = "Executable not found: " + args.front();
		return false;
	}
	std::vector<char *> argv;
	for (const auto &arg : args)
		argv.push_back(const_cast<char *>(arg.c_str()));
	argv.push_back(nullptr);
	int out[2] = {-1, -1}, in[2] = {-1, -1}, failure[2] = {-1, -1};
	auto cleanup = [&] {
		for (auto *pair : {out, in, failure}) {
			Close(pair[0]);
			Close(pair[1]);
		}
	};
	if (pipe2(out, O_CLOEXEC) || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, in) || pipe2(failure, O_CLOEXEC)) {
		*error = strerror(errno);
		cleanup();
		return false;
	}
	pid_t parent = getpid();
	pid_t pid = fork();
	if (pid < 0) {
		*error = strerror(errno);
		cleanup();
		return false;
	}
	if (pid == 0) {
		// Only async-signal-safe calls between fork and exec: GTK may own other threads.
		Close(failure[0]);
		if (setsid() >= 0 && prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && getppid() == parent && chdir(cwd.c_str()) == 0 &&
			dup2(in[0], STDIN_FILENO) >= 0 && dup2(out[1], STDOUT_FILENO) >= 0 && dup2(out[1], STDERR_FILENO) >= 0) {
			for (auto *pair : {out, in}) {
				Close(pair[0]);
				Close(pair[1]);
			}
			execv(executable.c_str(), argv.data());
		}
		int code = errno;
		(void)!write(failure[1], &code, sizeof(code));
		_exit(127);
	}
	Close(out[1]);
	Close(in[0]);
	Close(failure[1]);
	int code = 0;
	ssize_t bytes;
	do {
		bytes = read(failure[0], &code, sizeof(code));
	} while (bytes < 0 && errno == EINTR);
	Close(failure[0]);
	if (bytes != 0) {
		*error = strerror(bytes > 0 ? code : errno);
		kill(-pid, SIGKILL);
		while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
		}
		cleanup();
		return false;
	}
	pid_ = pid;
	read_ = out[0];
	write_ = in[1];
	fcntl(read_, F_SETFL, O_NONBLOCK);
	fcntl(write_, F_SETFL, O_NONBLOCK);
	input_ = input;
	output_.clear();
	log_ = log;
	cancelled_ = false;
	exitCode_ = -1;
	std::error_code ec;
	if (!log_.empty()) {
		std::filesystem::create_directories(log_.parent_path(), ec);
		logBytes_ = std::filesystem::exists(log_, ec) ? std::filesystem::file_size(log_, ec) : 0;
	}
	Log("\n--- process started ---\n", 25);
	Poll();
	return true;
}
void ChildProcess::Append(const char *data, size_t size) {
	output_.append(data, size);
	if (output_.size() > 256 * 1024)
		output_.erase(0, output_.size() - 256 * 1024);
	Log(data, size);
}
void ChildProcess::Log(const char *data, size_t size) {
	if (log_.empty())
		return;
	if (logBytes_ + size > 2 * 1024 * 1024) {
		std::error_code ec;
		std::filesystem::rename(log_, log_.string() + ".old", ec);
		if (ec) {
			log_.clear();
			output_ += "\nCannot rotate process log: " + ec.message();
			return;
		}
		logBytes_ = 0;
	}
	std::ofstream file(log_, std::ios::binary | std::ios::app);
	file.write(data, size);
	if (!file) {
		output_ += "\nCannot write process log";
		log_.clear();
	} else
		logBytes_ += size;
}
void ChildProcess::Drain() {
	char buffer[4096];
	for (int i = 0; read_ >= 0 && i < 64; ++i) {
		ssize_t n = read(read_, buffer, sizeof(buffer));
		if (n > 0)
			Append(buffer, (size_t)n);
		else if (n < 0 && errno == EINTR)
			continue;
		else {
			if (!n || (errno != EAGAIN && errno != EWOULDBLOCK))
				Close(read_);
			break;
		}
	}
}
void ChildProcess::Poll() {
	if (!Running())
		return;
	Drain();
	if (write_ >= 0) {
		if (!input_.empty()) {
			ssize_t n = send(write_, input_.data(), input_.size(), MSG_NOSIGNAL);
			if (n > 0)
				input_.erase(0, (size_t)n);
			else if (n < 0 && errno != EINTR && errno != EAGAIN)
				Close(write_);
		}
		if (input_.empty())
			Close(write_);
	}
	if (cancelled_ && std::chrono::steady_clock::now() >= deadline_)
		kill(-pid_, SIGKILL);
	siginfo_t exited{};
	if (waitid(P_PID, pid_, &exited, WEXITED | WNOHANG | WNOWAIT) == 0 && exited.si_pid == pid_) {
		// Keep the zombie leader's PID reserved until its remaining group has been killed.
		kill(-pid_, SIGKILL);
		int status = 0;
		while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
		}
		exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
		pid_ = -1;
		Drain();
		Close(read_);
		Close(write_);
		std::string end = "\n--- exit " + std::to_string(exitCode_) + (cancelled_ ? " (cancelled)" : "") + " ---\n";
		Log(end.data(), end.size());
	}
}
void ChildProcess::Cancel() {
	if (!Running() || cancelled_)
		return;
	cancelled_ = true;
	deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	kill(-pid_, SIGTERM);
}
} // namespace frontend
