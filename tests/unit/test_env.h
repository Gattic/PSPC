// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifndef _WIN32
#include <cstdlib>
#include <optional>
#include <string>
#include <stdexcept>

// Restore the caller's environment; tests must never write to its real XDG directories.
class ScopedEnv {
public:
	ScopedEnv(const char *name, const char *value) : name_(name) {
		if (const char *old = std::getenv(name)) old_ = old;
		if ((value ? setenv(name, value, 1) : unsetenv(name)) != 0)
			throw std::runtime_error("Cannot set test environment");
	}
	~ScopedEnv() {
		if (old_) setenv(name_.c_str(), old_->c_str(), 1);
		else unsetenv(name_.c_str());
	}
	ScopedEnv(const ScopedEnv &) = delete;
	ScopedEnv &operator=(const ScopedEnv &) = delete;
private:
	std::string name_;
	std::optional<std::string> old_;
};
#endif
