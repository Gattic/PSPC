// SPDX-License-Identifier: GPL-2.0-or-later
// The layout follows the message dialog's (PPSSPP Core/Dialog/PSPMsgDialog.cpp, Copyright (c)
// 2012- PPSSPP Project, GPL-2.0-or-later).
#include "runtime/dialogs/home_menu.h"

#include <algorithm>

#include "common/log.h"
#include "runtime/boot.h"
#include "runtime/dialogs/dialog.h"
#include "runtime/dialogs/ppge.h"
#include "runtime/hle/display.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/m33/m33.h"
#include "runtime/vsh.h"

namespace ctrl {
void SetHomeHandler(void (*fn)());
u32 RawButtons();
}  // namespace ctrl

namespace homemenu {

namespace {

enum class State { Closed, Open, Quitting };

// A game whose exit callback doesn't end it within this long is stopped anyway.
constexpr u64 kQuitTimeoutUs = 5000000;
constexpr int kFadeFrames = 12;  // 0.2 s, like the dialogs' fades

State g_state = State::Closed;
int g_choice = 0;  // 1 = Yes, 0 = No
int g_frames = 0;  // since it opened
u32 g_lastButtons = 0;
u32 g_suppressed = 0;

void ExitNow() {
	if (vsh::ReturnToXmb()) {
		LOGI("HOME", "Quit game: back to the XMB");
		m33::ExitToVsh();
	} else {
		LOGI("HOME", "Quit game");
		sched::RequestStop(0);
	}
}

// As the PSP's "Quit game": the game's exit callback runs, and its sceKernelExitGame ends it.
void QuitGame() {
	SceUID cb = boot::ExitCallback();
	if (cb > 0 && kernel::NotifyCallback(cb, 0) >= 0) {
		LOGI("HOME", "Quit game: running the game's exit callback");
		return;
	}
	ExitNow();
}

u32 ConfirmButton() { return utility::SystemButtonPreference() == 1 ? utility::CTRL_CROSS : utility::CTRL_CIRCLE; }
u32 CancelButton() { return utility::SystemButtonPreference() == 1 ? utility::CTRL_CIRCLE : utility::CTRL_CROSS; }

void Open() {
	g_state = State::Open;
	display::ShowSystemScreen(true);
	g_choice = 0;
	g_frames = 0;
	// What is held now (HOME itself) isn't a press for the menu.
	g_lastButtons = ctrl::RawButtons();
	LOGI("HOME", "HOME menu");
}

void Close() {
	g_state = State::Closed;
	display::ShowSystemScreen(false);
	// The game doesn't get the buttons that closed the menu.
	g_suppressed = ctrl::RawButtons();
	ppge::Clear(ppge::Layer::System);
}

void Draw() {
	const u32 fade = (u32)std::min(255, (g_frames + 1) * 255 / kFadeFrames);
	auto faded = [fade](u32 c) { return (c & 0x00FFFFFF) | (((c >> 24) * fade / 255) << 24); };
	ppge::Style text;
	text.align = ppge::BOX_HCENTER;
	text.scale = 0.55f;
	text.color = faded(0xFFFFFFFF);
	text.hasShadow = true;
	text.shadowColor = faded(0x80000000);

	ppge::Begin(ppge::Layer::System);
	ppge::DrawRect(0, 0, 480, 272, faded(0xD8100C0C));

	const bool quitting = g_state == State::Quitting;
	const char *message = quitting ? "Please wait..." : "Quit the game?";
	float h = 0;
	ppge::MeasureString(nullptr, &h, message, text.scale, ppge::LINE_WRAP_WORD, 340.0f);
	float centerY = 135.0f;
	float h2 = h / 2.0f;
	float sy = centerY - h2 - 15.0f, ey = centerY + h2 + 20.0f;
	if (!quitting) {
		// The Yes/No row and its selection box, as the message dialog lays them out.
		const float buttonY = centerY + h2 + 5.0f, bw = 15.0f, bh = 8.0f;
		const float bx = g_choice == 1 ? 204.0f : 273.0f;
		ppge::DrawRect(bx - bw, buttonY - bh, bx + bw, buttonY + bh, faded(0x6DCFCFCF));
		centerY -= bh + 5.0f;
		sy -= bh + 5.0f;
		ey = buttonY + bh * 2.0f + 5.0f;
		ppge::Style button = text;
		button.align = ppge::BOX_CENTER;
		ppge::DrawString("Yes", 203.0f, buttonY - 1.0f, button);
		ppge::DrawString("No", 272.0f, buttonY - 1.0f, button);
	}
	ppge::DrawString(message, 240.0f, centerY - h2, text);
	ppge::DrawRect(40.0f, sy, 440.0f, sy + 1.0f, faded(0xFFFFFFFF));
	ppge::DrawRect(40.0f, ey, 440.0f, ey + 1.0f, faded(0xFFFFFFFF));

	if (!quitting) {
		// The button guide, as the dialogs show it (the confirm button on the left with X).
		ppge::Style guide = text;
		guide.align = ppge::BOX_LEFT;
		const bool crossConfirms = ConfirmButton() == utility::CTRL_CROSS;
		const float okX = crossConfirms ? 183.5f : 261.5f, backX = crossConfirms ? 261.5f : 183.5f;
		ppge::DrawIcon(crossConfirms ? ppge::Icon::Cross : ppge::Icon::Circle, okX, 256, 11.5f, 11.5f, guide);
		ppge::DrawString("Enter", okX + 14.5f, 252, guide);
		ppge::DrawIcon(crossConfirms ? ppge::Icon::Circle : ppge::Icon::Cross, backX, 256, 11.5f, 11.5f, guide);
		ppge::DrawString("Back", backX + 14.5f, 252, guide);
	}
	ppge::End();
}

}  // namespace

void Install(bool enable) {
	g_state = State::Closed;
	g_suppressed = 0;
	// Without overlays HOME only matters to a game the XMB launched.
	enable = enable && (ppge::Enabled() || vsh::ReturnToXmb());
	ctrl::SetHomeHandler(enable ? &OnHomePressed : nullptr);
}

void OnHomePressed() {
	if (!ppge::Enabled()) {
		// No menu to show: a game the XMB launched quits at once.
		if (vsh::ReturnToXmb()) QuitGame();
		return;
	}
	if (g_state == State::Open) {
		Close();
	} else if (g_state == State::Closed) {
		Open();
	}
}

void OnVblank() {
	if (g_state == State::Closed) return;
	const u32 raw = ctrl::RawButtons();
	const u32 pressed = raw & ~g_lastButtons;
	g_lastButtons = raw;
	if (g_state == State::Open) {
		if (pressed & utility::CTRL_LEFT) g_choice = 1;
		if (pressed & utility::CTRL_RIGHT) g_choice = 0;
		if (pressed & CancelButton()) {
			Close();
			return;
		}
		if (pressed & ConfirmButton()) {
			if (g_choice != 1) {
				Close();
				return;
			}
			g_state = State::Quitting;
			QuitGame();
			sched::ScheduleEvent(sched::NowUs() + kQuitTimeoutUs, []() {
				if (g_state != State::Quitting) return;
				LOGW("HOME", "The game didn't exit after its exit callback; stopping it");
				ExitNow();
			});
		}
	}
	g_frames++;
	Draw();
}

bool IsOpen() { return g_state != State::Closed; }

u32 FilterButtons(u32 raw) {
	if (g_state != State::Closed) return 0;
	g_suppressed &= raw;
	return raw & ~g_suppressed;
}

bool InputBlocked() { return g_state != State::Closed; }

}  // namespace homemenu
