// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal host prompts for sceUtility dialogs in windowed mode: a native message box or text input
// box on its own host thread, polled from the dialog's Update. (The dialogs don't draw a PSP-style
// overlay yet.)
#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace utility::prompt {

enum class Buttons { Ok, OkCancel, YesNo };
enum Answer : int { PENDING = 0, OK = 1, CANCEL = 2, YES = 3, NO = 4 };

struct State {
	std::atomic<int> answer{PENDING};
	std::string text;  // UTF-8, for text input (valid once answer != PENDING)
};

// Strings are UTF-8.
std::shared_ptr<State> ShowMessage(const std::string &title, const std::string &text, Buttons buttons);
std::shared_ptr<State> AskText(const std::string &title, const std::string &description, const std::string &initial);

}  // namespace utility::prompt
