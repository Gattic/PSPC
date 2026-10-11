// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Online page's dialogs: where a game goes online, the PSPC Network server's accounts, the
// Combat Ops and GameSpy servers' settings and contents, and a server's log.
#pragma once

#include <windows.h>

#include <string>

#include "common/settings.h"
#include "frontend/servers.h"

namespace serverdlg {

// Where a game (or ad hoc play) goes online. Public: a public revival service (publicOption).
enum class Target { ThisPc, Other, None, Public };

// Asks for the target; allowNone offers "nowhere", publicOption (when given) a public service, with
// that text. *address: the other server's address (in and out). False if cancelled.
bool ChooseServer(HWND owner, const wchar_t *title, const wchar_t *intro, bool allowNone, Target *target, std::wstring *address,
                  const wchar_t *publicOption = nullptr);

// The PSPC Network server's accounts: ban, unban, set a new password.
void NetworkAccounts(HWND owner);

// The Combat Ops server: the address players use and who may sign in (saved into s, true if they
// changed), and its players, shared missions, news and message board.
bool ManageSfco(HWND owner, settings::Settings &s);

// The GameSpy server: the address players on other PCs use (saved into s, true if it changed), and
// its players (ban, unban, new password), their career totals and the last games.
bool ManageGameSpy(HWND owner, settings::Settings &s);

// A server's log, following it as it grows.
void ShowLog(HWND owner, servers::Kind k, const wchar_t *title);

}  // namespace serverdlg
