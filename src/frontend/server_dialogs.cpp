// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Online page's dialogs (see server_dialogs.h). Built in code like the launcher's other dialogs;
// the server's admin commands run with servers::Admin (a fraction of a second each).
#undef WIN32_LEAN_AND_MEAN
#include "frontend/server_dialogs.h"

#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <ctime>
#include <string>
#include <vector>

#include "common/json.h"
#include "frontend/dialog_util.h"
#include "frontend/ui_theme.h"

namespace serverdlg {

namespace {

using dlgutil::Control;
using dlgutil::Template;

enum : int {
	IDC_THISPC = 1001,
	IDC_OTHER,
	IDC_NONE,
	IDC_ADDRESS,
	IDC_LIST,
	IDC_A,
	IDC_B,
	IDC_C,
	IDC_REFRESH,
	IDC_VIEW,
	IDC_ALLOW_LOCAL,
	IDC_LOG,
	IDC_FOLDER,
	IDC_TEXT,
	IDC_TEXT2,
};

std::wstring Widen(const std::string &s) {
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string Narrow(const std::wstring &w) {
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring Trim(std::wstring s) {
	while (!s.empty() && iswspace(s.back())) s.pop_back();
	while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
	return s;
}

std::wstring ItemText(HWND dlg, int id) {
	wchar_t buf[1024] = {};
	GetDlgItemTextW(dlg, id, buf, 1024);
	return Trim(buf);
}

std::wstring Date(double epoch) {
	if (epoch <= 0) return L"";
	time_t t = (time_t)epoch;
	tm local{};
	localtime_s(&local, &t);
	wchar_t buf[32];
	wcsftime(buf, 32, L"%Y-%m-%d", &local);
	return buf;
}

// Runs a modal dialog built in code; proc gets `state` as WM_INITDIALOG's lParam.
INT_PTR Run(HWND owner, const wchar_t *title, short cx, short cy, DLGPROC proc, void *state) {
	std::vector<WORD> tmpl = Template(title, cx, cy);
	return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (LPCDLGTEMPLATEW)tmpl.data(), owner, proc, (LPARAM)state);
}

template <class T>
T *State(HWND dlg, UINT msg, LPARAM lp) {
	if (msg == WM_INITDIALOG) SetWindowLongPtrW(dlg, DWLP_USER, lp);
	return (T *)GetWindowLongPtrW(dlg, DWLP_USER);
}

void Columns(HWND list, std::initializer_list<std::pair<const wchar_t *, int>> cols) {
	HWND header = ListView_GetHeader(list);
	while (header && Header_GetItemCount(header) > 0) ListView_DeleteColumn(list, 0);
	RECT r;
	GetClientRect(list, &r);
	int total = 0, i = 0;
	for (const auto &c : cols) total += c.second;
	for (const auto &c : cols) {
		LVCOLUMNW col{};
		col.mask = LVCF_TEXT | LVCF_WIDTH;
		col.pszText = (LPWSTR)c.first;
		col.cx = (r.right - GetSystemMetrics(SM_CXVSCROLL)) * c.second / total;
		ListView_InsertColumn(list, i++, &col);
	}
}

void Row(HWND list, const std::vector<std::wstring> &cells, LPARAM param) {
	LVITEMW item{};
	item.mask = LVIF_TEXT | LVIF_PARAM;
	item.iItem = ListView_GetItemCount(list);
	item.pszText = (LPWSTR)cells[0].c_str();
	item.lParam = param;
	int row = ListView_InsertItem(list, &item);
	for (size_t c = 1; c < cells.size(); c++) ListView_SetItemText(list, row, (int)c, (LPWSTR)cells[c].c_str());
}

int SelectedParam(HWND list) {
	int i = ListView_GetNextItem(list, -1, LVNI_SELECTED);
	if (i < 0) return -1;
	LVITEMW item{};
	item.mask = LVIF_PARAM;
	item.iItem = i;
	ListView_GetItem(list, &item);
	return (int)item.lParam;
}

HWND MakeList(HWND dlg, int x, int y, int w, int h) {
	HWND list = Control(dlg, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_BORDER | WS_TABSTOP, x,
	                    y, w, h, IDC_LIST);
	ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	return list;
}

// Runs an admin command; shows its error. Returns its output in *out (when given).
bool AdminOrSay(HWND dlg, servers::Kind k, const std::vector<std::wstring> &args, std::string *out = nullptr, const std::string &input = {}) {
	std::string text;
	HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
	bool ok = servers::Admin(k, args, &text, input);
	SetCursor(old);
	if (!ok) {
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
		MessageBoxW(dlg, (L"The server's command failed:\n\n" + Widen(text)).c_str(), L"PSPC", MB_ICONWARNING);
	}
	if (out) *out = text;
	return ok;
}

// ---- where a game goes online

struct ChooseState {
	const wchar_t *intro;
	bool allowNone;
	Target target;
	std::wstring address;
};

void SyncChoose(HWND dlg) { EnableWindow(GetDlgItem(dlg, IDC_ADDRESS), IsDlgButtonChecked(dlg, IDC_OTHER) == BST_CHECKED); }

INT_PTR CALLBACK ChooseProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	ChooseState *st = State<ChooseState>(dlg, msg, lp);
	switch (msg) {
	case WM_INITDIALOG: {
		Control(dlg, L"STATIC", st->intro, SS_LEFT, 10, 8, 250, 28, -1);
		Control(dlg, L"BUTTON", L"This PC's server (start it on the Online page)", BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 14, 40, 246, 11,
		        IDC_THISPC);
		Control(dlg, L"BUTTON", L"A server on another PC, or on the internet, at:", BS_AUTORADIOBUTTON, 14, 56, 246, 11, IDC_OTHER);
		int y = 90;
		if (st->allowNone) {
			Control(dlg, L"BUTTON", L"Nowhere (the game's own servers, which Sony closed)", BS_AUTORADIOBUTTON, 14, 90, 246, 11, IDC_NONE);
			y = 106;
		}
		// (Created after the radio buttons, so they stay one group.)
		Control(dlg, L"EDIT", st->address.c_str(), ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP, 26, 70, 234, 13, IDC_ADDRESS);
		SendDlgItemMessageW(dlg, IDC_ADDRESS, EM_SETCUEBANNER, TRUE, (LPARAM)L"192.168.1.20, or a host name such as sfco.example.org");
		Control(dlg, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 156, y + 8, 50, 14, IDOK);
		Control(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 210, y + 8, 50, 14, IDCANCEL);
		CheckDlgButton(dlg, st->target == Target::ThisPc ? IDC_THISPC : st->target == Target::Other ? IDC_OTHER : IDC_NONE, BST_CHECKED);
		SyncChoose(dlg);
		ui::ThemeDialog(dlg);
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDC_THISPC:
		case IDC_OTHER:
		case IDC_NONE:
			SyncChoose(dlg);
			if (LOWORD(wp) == IDC_OTHER) SetFocus(GetDlgItem(dlg, IDC_ADDRESS));
			return TRUE;
		case IDOK: {
			std::wstring address = ItemText(dlg, IDC_ADDRESS);
			Target t = IsDlgButtonChecked(dlg, IDC_THISPC) == BST_CHECKED ? Target::ThisPc
			           : IsDlgButtonChecked(dlg, IDC_OTHER) == BST_CHECKED ? Target::Other
			                                                               : Target::None;
			if (t == Target::Other) {
				// Accept "http://host:port/..." pasted from somewhere: keep the host.
				size_t scheme = address.find(L"://");
				if (scheme != std::wstring::npos) address.erase(0, scheme + 3);
				size_t end = address.find_first_of(L"/:");
				if (end != std::wstring::npos) address.erase(end);
				if (address.empty() || address.find_first_of(L" \t") != std::wstring::npos) {
					MessageBoxW(dlg, L"Enter the server's address: an IP address such as 192.168.1.20, or a host name.", L"PSPC",
					            MB_ICONINFORMATION);
					SetFocus(GetDlgItem(dlg, IDC_ADDRESS));
					return TRUE;
				}
			}
			st->target = t;
			st->address = address;
			EndDialog(dlg, IDOK);
			return TRUE;
		}
		case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
		}
		break;
	}
	return FALSE;
}

// ---- a short text (a password, a news item)

struct AskState {
	std::wstring prompt, prompt2, value;
	bool password = false;
};

INT_PTR CALLBACK AskProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	AskState *st = State<AskState>(dlg, msg, lp);
	switch (msg) {
	case WM_INITDIALOG: {
		DWORD pw = st->password ? ES_PASSWORD : 0;
		Control(dlg, L"STATIC", st->prompt.c_str(), SS_LEFT, 10, 8, 230, 10, -1);
		Control(dlg, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | pw, 10, 20, 230, 13, IDC_TEXT);
		int y = 40;
		if (!st->prompt2.empty()) {
			Control(dlg, L"STATIC", st->prompt2.c_str(), SS_LEFT, 10, 40, 230, 10, -1);
			Control(dlg, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | pw, 10, 52, 230, 13, IDC_TEXT2);
			y = 72;
		}
		Control(dlg, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 136, y + 4, 50, 14, IDOK);
		Control(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 190, y + 4, 50, 14, IDCANCEL);
		ui::ThemeDialog(dlg);
		SetFocus(GetDlgItem(dlg, IDC_TEXT));
		return FALSE;
	}
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK) {
			wchar_t buf[512] = {};
			GetDlgItemTextW(dlg, IDC_TEXT, buf, 512);
			std::wstring value = st->password ? std::wstring(buf) : Trim(buf);
			if (!st->prompt2.empty()) {
				wchar_t again[512] = {};
				GetDlgItemTextW(dlg, IDC_TEXT2, again, 512);
				if (value != again) {
					MessageBoxW(dlg, L"The two passwords differ.", L"PSPC", MB_ICONINFORMATION);
					return TRUE;
				}
			}
			if (st->password && value.size() < 8) {
				MessageBoxW(dlg, L"Passwords need at least 8 characters.", L"PSPC", MB_ICONINFORMATION);
				return TRUE;
			}
			if (value.empty()) return TRUE;
			st->value = value;
			EndDialog(dlg, IDOK);
			return TRUE;
		}
		if (LOWORD(wp) == IDCANCEL) {
			EndDialog(dlg, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

bool Ask(HWND owner, const wchar_t *title, AskState &st) {
	return Run(owner, title, 250, st.prompt2.empty() ? 62 : 94, AskProc, &st) == IDOK;
}

// ---- PSPC Network accounts

struct AccountsState {
	HWND list = nullptr;
	std::vector<std::wstring> ids;
};

void LoadAccounts(HWND dlg, AccountsState &st) {
	ListView_DeleteAllItems(st.list);
	st.ids.clear();
	std::string out;
	if (!AdminOrSay(dlg, servers::Kind::Network, {L"--json", L"accounts"}, &out)) return;
	for (const json::Value &a : json::Value::Parse(out).Array()) {
		std::wstring id = Widen(a["online_id"].String(""));
		Row(st.list, {id, Widen(a["country"].String("")), Date(a["created"].Number()), a["banned"].Bool() ? L"Banned" : L""}, (LPARAM)st.ids.size());
		st.ids.push_back(id);
	}
}

INT_PTR CALLBACK AccountsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	AccountsState *st = State<AccountsState>(dlg, msg, lp);
	switch (msg) {
	case WM_INITDIALOG:
		Control(dlg, L"STATIC",
		        L"The accounts on this PC's PSPC Network server. Players create their own, with Account... in their PSPC "
		        L"launcher; here you can ban them or set a new password for someone who forgot theirs.",
		        SS_LEFT, 10, 8, 300, 26, -1);
		st->list = MakeList(dlg, 10, 38, 300, 140);
		Columns(st->list, {{L"Online ID", 40}, {L"Country", 15}, {L"Created", 25}, {L"Status", 20}});
		Control(dlg, L"BUTTON", L"Ban", BS_PUSHBUTTON | WS_TABSTOP, 10, 184, 50, 14, IDC_A);
		Control(dlg, L"BUTTON", L"Unban", BS_PUSHBUTTON | WS_TABSTOP, 64, 184, 50, 14, IDC_B);
		Control(dlg, L"BUTTON", L"New password...", BS_PUSHBUTTON | WS_TABSTOP, 118, 184, 70, 14, IDC_C);
		Control(dlg, L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 192, 184, 50, 14, IDC_REFRESH);
		Control(dlg, L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 260, 184, 50, 14, IDOK);
		ui::ThemeDialog(dlg);
		LoadAccounts(dlg, *st);
		return TRUE;
	case WM_COMMAND: {
		int sel = SelectedParam(st->list);
		std::wstring id = sel >= 0 && (size_t)sel < st->ids.size() ? st->ids[sel] : L"";
		switch (LOWORD(wp)) {
		case IDC_A:
		case IDC_B:
			if (id.empty()) return TRUE;
			if (AdminOrSay(dlg, servers::Kind::Network, {LOWORD(wp) == IDC_A ? L"ban" : L"unban", id})) LoadAccounts(dlg, *st);
			return TRUE;
		case IDC_C: {
			if (id.empty()) return TRUE;
			AskState ask;
			ask.prompt = L"New password for " + id + L":";
			ask.prompt2 = L"The same again:";
			ask.password = true;
			if (Ask(dlg, L"New password", ask) &&
			    AdminOrSay(dlg, servers::Kind::Network, {L"passwd", id, L"--password-stdin"}, nullptr, Narrow(ask.value)))
				MessageBoxW(dlg, (L"The password of " + id + L" is changed; they sign in again with it.").c_str(), L"PSPC", MB_ICONINFORMATION);
			return TRUE;
		}
		case IDC_REFRESH: LoadAccounts(dlg, *st); return TRUE;
		case IDOK:
		case IDCANCEL: EndDialog(dlg, IDOK); return TRUE;
		}
		break;
	}
	}
	return FALSE;
}

// ---- the Combat Ops server

enum View { kPlayers, kMissions, kNews, kBoard };

struct SfcoState {
	settings::Settings *s = nullptr;
	HWND list = nullptr;
	View view = kPlayers;
	std::vector<std::wstring> ids;  // per row: the name or id the actions take
	bool changed = false;
};

void LoadView(HWND dlg, SfcoState &st) {
	ListView_DeleteAllItems(st.list);
	st.ids.clear();
	static const wchar_t *const kA[] = {L"Ban", L"Hide", L"Add...", L"Hide"}, *const kB[] = {L"Unban", L"Show", L"Delete", L"Show"};
	SetDlgItemTextW(dlg, IDC_A, kA[st.view]);
	SetDlgItemTextW(dlg, IDC_B, kB[st.view]);
	ShowWindow(GetDlgItem(dlg, IDC_C), st.view == kMissions ? SW_SHOW : SW_HIDE);
	std::string out;
	switch (st.view) {
	case kPlayers:
		Columns(st.list, {{L"Player", 45}, {L"First seen", 30}, {L"Status", 25}});
		if (!AdminOrSay(dlg, servers::Kind::Sfco, {L"--json", L"accounts"}, &out)) return;
		for (const json::Value &a : json::Value::Parse(out).Array()) {
			std::wstring name = Widen(a["name"].String(""));
			Row(st.list, {name, Date(a["created"].Number()), a["banned"].Bool() ? L"Banned" : L""}, (LPARAM)st.ids.size());
			st.ids.push_back(name);
		}
		break;
	case kMissions:
		Columns(st.list, {{L"Mission", 34}, {L"By", 22}, {L"Plays", 11}, {L"Downloads", 15}, {L"", 18}});
		if (!AdminOrSay(dlg, servers::Kind::Sfco, {L"--json", L"missions", L"list"}, &out)) return;
		for (const json::Value &m : json::Value::Parse(out).Array()) {
			std::wstring flags = m["official"].Bool() ? L"Official" : L"";
			if (m["hidden"].Bool()) flags += flags.empty() ? L"Hidden" : L", hidden";
			Row(st.list,
			    {Widen(m["name"].String("")), Widen(m["author"].String("")), std::to_wstring((long long)m["plays"].Number()),
			     std::to_wstring((long long)m["downloads"].Number()), flags},
			    (LPARAM)st.ids.size());
			st.ids.push_back(std::to_wstring((long long)m["id"].Number()));
		}
		break;
	case kNews:
		Columns(st.list, {{L"Date", 22}, {L"News", 78}});
		if (!AdminOrSay(dlg, servers::Kind::Sfco, {L"--json", L"news", L"list"}, &out)) return;
		for (const json::Value &n : json::Value::Parse(out).Array()) {
			Row(st.list, {Date(n["created"].Number()), Widen(n["text"].String(""))}, (LPARAM)st.ids.size());
			st.ids.push_back(std::to_wstring((long long)n["id"].Number()));
		}
		break;
	case kBoard:
		Columns(st.list, {{L"Board", 18}, {L"Author", 20}, {L"Post", 47}, {L"", 15}});
		if (!AdminOrSay(dlg, servers::Kind::Sfco, {L"--json", L"board", L"list"}, &out)) return;
		for (const json::Value &p : json::Value::Parse(out).Array()) {
			std::string text = p["subject"].String("");
			if (text.empty()) text = p["body"].String("");
			if (p["reply_to"].Number() > 0) text = "Re: " + text;
			Row(st.list, {Widen(p["board"].String("")), Widen(p["author"].String("")), Widen(text), p["hidden"].Bool() ? L"Hidden" : L""},
			    (LPARAM)st.ids.size());
			st.ids.push_back(std::to_wstring((long long)p["id"].Number()));
		}
		break;
	}
}

void SfcoAction(HWND dlg, SfcoState &st, int button) {
	int sel = SelectedParam(st.list);
	std::wstring id = sel >= 0 && (size_t)sel < st.ids.size() ? st.ids[sel] : L"";
	std::vector<std::wstring> args;
	switch (st.view) {
	case kPlayers:
		if (id.empty()) return;
		args = {button == IDC_A ? L"ban" : L"unban", id};
		break;
	case kMissions:
		if (id.empty()) return;
		if (button == IDC_C &&
		    MessageBoxW(dlg, L"Delete this mission from the server for good?", L"PSPC", MB_YESNO | MB_ICONQUESTION) != IDYES)
			return;
		args = {L"missions", button == IDC_A ? L"hide" : button == IDC_B ? L"unhide" : L"delete", id};
		break;
	case kNews:
		if (button == IDC_A) {
			AskState ask;
			ask.prompt = L"News for the game's Community pages:";
			if (!Ask(dlg, L"Add news", ask)) return;
			args = {L"news", L"add", ask.value};
		} else {
			if (id.empty()) return;
			args = {L"news", L"delete", id};
		}
		break;
	case kBoard:
		if (id.empty()) return;
		args = {L"board", button == IDC_A ? L"hide" : L"unhide", id};
		break;
	}
	if (AdminOrSay(dlg, servers::Kind::Sfco, args)) LoadView(dlg, st);
}

INT_PTR CALLBACK SfcoProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	SfcoState *st = State<SfcoState>(dlg, msg, lp);
	switch (msg) {
	case WM_INITDIALOG: {
		Control(dlg, L"BUTTON", L"Server", BS_GROUPBOX, 7, 5, 346, 92, -1);
		Control(dlg, L"STATIC", L"Players reach it at", SS_LEFT, 15, 21, 70, 10, -1);
		Control(dlg, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP, 88, 19, 180, 13, IDC_ADDRESS);
		SetDlgItemTextW(dlg, IDC_ADDRESS, Widen(st->s->hostSfcoAddress).c_str());
		std::wstring cue = servers::LanAddress() + L" (this PC on your network)";
		SendDlgItemMessageW(dlg, IDC_ADDRESS, EM_SETCUEBANNER, TRUE, (LPARAM)cue.c_str());
		Control(dlg, L"STATIC",
		        L"Leave it empty for players on your network. For players over the internet: your public address, with ports "
		        L"10070-10079 and 10443 forwarded to this PC on your router.",
		        SS_LEFT, 15, 37, 330, 18, -1);
		Control(dlg, L"BUTTON", L"Let in players who aren't signed in to a PSPC Network server", BS_AUTOCHECKBOX | WS_TABSTOP, 15, 60, 330, 11,
		        IDC_ALLOW_LOCAL);
		CheckDlgButton(dlg, IDC_ALLOW_LOCAL, st->s->hostSfcoAllowLocal ? BST_CHECKED : BST_UNCHECKED);
		Control(dlg, L"STATIC", L"These apply the next time the server starts.", SS_LEFT, 15, 78, 200, 10, -1);
		Control(dlg, L"STATIC", L"Show", SS_LEFT, 10, 107, 24, 10, -1);
		HWND view = Control(dlg, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, 36, 105, 100, 80, IDC_VIEW);
		for (const wchar_t *v : {L"Players", L"Shared missions", L"News", L"Message board"}) SendMessageW(view, CB_ADDSTRING, 0, (LPARAM)v);
		SendMessageW(view, CB_SETCURSEL, 0, 0);
		st->list = MakeList(dlg, 10, 122, 340, 112);
		Control(dlg, L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 10, 240, 50, 14, IDC_A);
		Control(dlg, L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 64, 240, 50, 14, IDC_B);
		Control(dlg, L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, 118, 240, 50, 14, IDC_C);
		Control(dlg, L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 172, 240, 50, 14, IDC_REFRESH);
		Control(dlg, L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 300, 240, 50, 14, IDOK);
		ui::ThemeDialog(dlg);
		LoadView(dlg, *st);
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDC_VIEW:
			if (HIWORD(wp) == CBN_SELCHANGE) {
				st->view = (View)SendDlgItemMessageW(dlg, IDC_VIEW, CB_GETCURSEL, 0, 0);
				LoadView(dlg, *st);
			}
			return TRUE;
		case IDC_A:
		case IDC_B:
		case IDC_C: SfcoAction(dlg, *st, LOWORD(wp)); return TRUE;
		case IDC_REFRESH: LoadView(dlg, *st); return TRUE;
		case IDOK:
		case IDCANCEL: {
			std::string address = Narrow(ItemText(dlg, IDC_ADDRESS));
			std::string err;
			settings::Settings s = *st->s;
			if (!settings::Set(s, "host_sfco_address", address, &err)) {
				MessageBoxW(dlg, L"The address is a host name or an IP address, without spaces or slashes.", L"PSPC", MB_ICONINFORMATION);
				SetFocus(GetDlgItem(dlg, IDC_ADDRESS));
				return TRUE;
			}
			s.hostSfcoAllowLocal = IsDlgButtonChecked(dlg, IDC_ALLOW_LOCAL) == BST_CHECKED;
			st->changed = s.hostSfcoAddress != st->s->hostSfcoAddress || s.hostSfcoAllowLocal != st->s->hostSfcoAllowLocal;
			*st->s = s;
			EndDialog(dlg, IDOK);
			return TRUE;
		}
		}
		break;
	}
	return FALSE;
}

// ---- a server's log

struct LogState {
	servers::Kind kind;
	HWND edit = nullptr;
	ULONGLONG lastSize = ~0ull;
	ULONGLONG lastWrite = 0;
	HFONT font = nullptr;
};

void RefreshLog(LogState &st) {
	WIN32_FILE_ATTRIBUTE_DATA fa{};
	ULONGLONG size = 0, written = 0;
	if (GetFileAttributesExW(servers::LogPath(st.kind).c_str(), GetFileExInfoStandard, &fa)) {
		size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
		written = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
	}
	if (size == st.lastSize && written == st.lastWrite) return;
	st.lastSize = size;
	st.lastWrite = written;
	std::string text;
	if (FILE *f = _wfopen(servers::LogPath(st.kind).c_str(), L"rb")) {
		const long keep = 256 * 1024;
		if (size > (ULONGLONG)keep) _fseeki64(f, (long long)size - keep, SEEK_SET);
		char buf[65536];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
		fclose(f);
		if (size > (ULONGLONG)keep) {
			size_t nl = text.find('\n');
			text.erase(0, nl == std::string::npos ? 0 : nl + 1);
		}
	}
	if (text.empty()) text = "(Nothing yet: the log fills in when the server starts.)";
	// The edit control wants CRLF.
	std::wstring w;
	for (wchar_t c : Widen(text)) {
		if (c == L'\r') continue;
		if (c == L'\n') w += L'\r';
		w += c;
	}
	SetWindowTextW(st.edit, w.c_str());
	int len = GetWindowTextLengthW(st.edit);
	SendMessageW(st.edit, EM_SETSEL, len, len);
	SendMessageW(st.edit, EM_SCROLLCARET, 0, 0);
}

// The log reads light on dark (the dialog theme dims read-only edits).
LRESULT CALLBACK LogDialogSubclass(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
	if (msg == WM_CTLCOLORSTATIC && GetDlgCtrlID((HWND)lp) == IDC_LOG) {
		static HBRUSH bg = CreateSolidBrush(ui::kBg);
		SetTextColor((HDC)wp, ui::kTextDim);
		SetBkColor((HDC)wp, ui::kBg);
		return (LRESULT)bg;
	}
	if (msg == WM_NCDESTROY) RemoveWindowSubclass(dlg, LogDialogSubclass, 0);
	return DefSubclassProc(dlg, msg, wp, lp);
}

INT_PTR CALLBACK LogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	LogState *st = State<LogState>(dlg, msg, lp);
	switch (msg) {
	case WM_INITDIALOG: {
		st->edit = Control(dlg, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL | WS_BORDER,
		                   7, 7, 446, 226, IDC_LOG);
		int dpi = ui::DpiOf(dlg);
		st->font = CreateFontW(-ui::Px(12, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH,
		                       L"Consolas");
		SendMessageW(st->edit, WM_SETFONT, (WPARAM)st->font, FALSE);
		SendMessageW(st->edit, EM_SETLIMITTEXT, 0, 0);
		Control(dlg, L"BUTTON", L"Show the file", BS_PUSHBUTTON | WS_TABSTOP, 7, 240, 70, 14, IDC_FOLDER);
		Control(dlg, L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 403, 240, 50, 14, IDOK);
		ui::ThemeDialog(dlg);
		SetWindowTheme(st->edit, L"DarkMode_Explorer", nullptr);  // dark scroll bars
		SetWindowSubclass(dlg, LogDialogSubclass, 0, 0);
		RefreshLog(*st);
		SetTimer(dlg, 1, 1000, nullptr);
		SetFocus(GetDlgItem(dlg, IDOK));
		return FALSE;
	}
	case WM_TIMER: RefreshLog(*st); return TRUE;
	case WM_COMMAND:
		if (LOWORD(wp) == IDC_FOLDER) {
			std::wstring args = L"/select,\"" + servers::LogPath(st->kind).wstring() + L"\"";
			ShellExecuteW(dlg, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOW);
			return TRUE;
		}
		if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
			KillTimer(dlg, 1);
			EndDialog(dlg, IDOK);
			return TRUE;
		}
		break;
	case WM_DESTROY:
		if (st && st->font) DeleteObject(st->font);
		break;
	}
	return FALSE;
}

}  // namespace

bool ChooseServer(HWND owner, const wchar_t *title, const wchar_t *intro, bool allowNone, Target *target, std::wstring *address) {
	ChooseState st{intro, allowNone, *target, *address};
	if (Run(owner, title, 270, allowNone ? 132 : 116, ChooseProc, &st) != IDOK) return false;
	*target = st.target;
	*address = st.address;
	return true;
}

void NetworkAccounts(HWND owner) {
	AccountsState st;
	Run(owner, L"PSPC Network accounts", 320, 206, AccountsProc, &st);
}

bool ManageSfco(HWND owner, settings::Settings &s) {
	SfcoState st;
	st.s = &s;
	Run(owner, L"Syphon Filter: Combat Ops server", 360, 262, SfcoProc, &st);
	return st.changed;
}

void ShowLog(HWND owner, servers::Kind k, const wchar_t *title) {
	LogState st;
	st.kind = k;
	Run(owner, title, 460, 262, LogProc, &st);
}

}  // namespace serverdlg
