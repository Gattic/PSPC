// SPDX-License-Identifier: GPL-2.0-or-later
//
// The launcher's Online page: PSPC Network (sign-in and friends), the game servers and the ad hoc
// server, each as a card with where the player's games go online ("your games") and the server this
// PC can run for them and their friends ("this PC": start/stop, its log and admin).
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace online {

// Posted to the parent when a server's state changes (the sidebar shows how many run).
constexpr UINT WM_ONLINE_CHANGED = WM_APP + 41;

struct Hooks {
	// A library game's file for a disc ID ("" = not in the library): the Combat Ops server serves
	// the disc's official missions.
	std::function<std::wstring(const char *discId)> findGame;
	// Opens the Network dialog (the account); the launcher reloads its settings afterwards.
	std::function<void()> openAccount;
};

// The page, a child of the main window (hidden until shown).
HWND Create(HWND parent, const Hooks &hooks);
// Re-reads pspc.ini and redraws (after another dialog changed it).
void Refresh();
// Starts the servers set to start with the launcher.
void StartAutomatic();
// Closing the launcher stops the servers: asks first when any runs. True = close.
bool ConfirmClose(HWND owner);
// For the sidebar: how many servers run, or what the page is for.
std::wstring Summary();

}  // namespace online
