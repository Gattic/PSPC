// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace frontend {
// Linux launcher-owned process group. Poll on the UI thread; destruction reaps
// the leader and kills its compiler/server descendants, never unrelated jobs.
class ChildProcess {
  public:
	ChildProcess() = default;
	~ChildProcess();
	ChildProcess(const ChildProcess &) = delete;
	ChildProcess &operator=(const ChildProcess &) = delete;
	bool Start(const std::vector<std::string> &args, const std::filesystem::path &cwd, const std::filesystem::path &log,
			   std::string *error, const std::string &input = {});
	void Poll();
	void Cancel();
	bool Running() const { return pid_ > 0; }
	bool Cancelled() const { return cancelled_; }
	int ExitCode() const { return exitCode_; }
	const std::string &Output() const { return output_; }

  private:
	void Drain();
	void Append(const char *data, size_t size);
	void Log(const char *data, size_t size);
	int pid_ = -1, read_ = -1, write_ = -1, exitCode_ = -1;
	bool cancelled_ = false;
	std::chrono::steady_clock::time_point deadline_;
	std::string input_, output_;
	std::filesystem::path log_;
	size_t logBytes_ = 0;
};
} // namespace frontend
