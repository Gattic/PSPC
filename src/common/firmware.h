// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/settings.h"

namespace firmware {
// Portable directory names, not arbitrary paths. "default" receives the previous installation.
bool ValidName(const std::string &name);
fs::path Profile(const fs::path &dataRoot, const std::string &name);
bool Installed(const fs::path &dataRoot, const std::string &name);
std::vector<std::string> List(const fs::path &dataRoot);
// Move only the requested default flash trees; explicitly overridden roots stay untouched.
// Never overwrite or discard a tree (an empty folder on either side is removed). A later default
// launch can migrate the remainder.
void Migrate(const fs::path &dataRoot, bool migrateFlash0 = true, bool migrateFlash1 = true);
// Save first, then update the caller's settings. All operations throw on failure.
void Select(const fs::path &dataRoot, const fs::path &ini, settings::Settings &config, const std::string &name);
// The caller must obtain confirmation and stop running guests/builds first.
void Delete(const fs::path &dataRoot, const std::string &active, const std::string &name);
} // namespace firmware
