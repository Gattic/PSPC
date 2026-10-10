// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Network dialog (see network_dialog.h). Server calls block the dialog briefly (a few seconds
// at most, with the wait cursor): signing in is a single round trip.
#undef WIN32_LEAN_AND_MEAN
#include "frontend/network_dialog.h"

#include <commctrl.h>

#include <string>
#include <vector>

#include "common/settings.h"
#include "frontend/dialog_util.h"
#include "net/np_client.h"

namespace {

using dlgutil::Control;

enum : int {
	IDC_LOCAL = 1001,
	IDC_HOSTED,
	IDC_LOCAL_ID,
	IDC_SERVER,
	IDC_STATUS,
	IDC_ID,
	IDC_PASSWORD,
	IDC_SIGNIN,
	IDC_REGISTER,
	IDC_SIGNOUT,
	IDC_FRIENDS,
	IDC_FRIEND_ID,
	IDC_FRIEND_ADD,
	IDC_FRIEND_REMOVE,
};

struct Dialog {
	fs::path ini;
	settings::Settings s;
	HWND friends = nullptr;
	bool changed = false;
};
Dialog *g_dlg = nullptr;

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
std::string Text(HWND dlg, int id) {
	wchar_t buf[512] = {};
	GetDlgItemTextW(dlg, id, buf, 512);
	std::string s = Narrow(buf);
	while (!s.empty() && s.back() == ' ') s.pop_back();
	while (!s.empty() && s.front() == ' ') s.erase(s.begin());
	return s;
}

class WaitCursor {
public:
	WaitCursor() : old_(SetCursor(LoadCursor(nullptr, IDC_WAIT))) {}
	~WaitCursor() { SetCursor(old_); }

private:
	HCURSOR old_;
};

bool Hosted(HWND dlg) { return IsDlgButtonChecked(dlg, IDC_HOSTED) == BST_CHECKED; }
bool SignedIn(const Dialog &d) { return !d.s.npServer.empty() && !d.s.npToken.empty(); }

void FillFriends(HWND dlg, Dialog &d) {
	ListView_DeleteAllItems(d.friends);
	if (!SignedIn(d) || !Hosted(dlg)) return;
	std::vector<np::Friend> list;
	WaitCursor wait;
	np::NetworkClient client(d.s.npServer, d.s.npToken);
	client.timeoutMs = 5000;
	if (!client.Friends(&list)) return;
	int i = 0;
	for (const np::Friend &f : list) {
		std::wstring name = Widen(f.onlineId);
		LVITEMW it{};
		it.mask = LVIF_TEXT;
		it.iItem = i;
		it.pszText = name.data();
		ListView_InsertItem(d.friends, &it);
		const wchar_t *status = f.status == "friend" ? L"Friends" : f.status == "sent" ? L"Request sent" : L"Wants to be friends";
		ListView_SetItemText(d.friends, i, 1, (LPWSTR)status);
		i++;
	}
}

void UpdateUi(HWND dlg, Dialog &d) {
	const bool hosted = Hosted(dlg);
	const bool signedIn = SignedIn(d);
	EnableWindow(GetDlgItem(dlg, IDC_LOCAL_ID), !hosted);
	for (int id : {IDC_SERVER, IDC_ID, IDC_PASSWORD, IDC_SIGNIN, IDC_REGISTER}) EnableWindow(GetDlgItem(dlg, id), hosted);
	EnableWindow(GetDlgItem(dlg, IDC_SIGNOUT), hosted && signedIn);
	for (int id : {IDC_FRIENDS, IDC_FRIEND_ID, IDC_FRIEND_ADD, IDC_FRIEND_REMOVE}) EnableWindow(GetDlgItem(dlg, id), hosted && signedIn);
	std::wstring status;
	if (!hosted) status = L"Games sign in as the online ID above, on this PC only. Fine for playing with friends; a server is needed to keep names unique.";
	else if (signedIn) status = L"Signed in as " + Widen(d.s.npOnlineId) + L" on " + Widen(d.s.npServer) + L".";
	else status = L"Not signed in. Enter the server, then sign in, or create an account.";
	SetDlgItemTextW(dlg, IDC_STATUS, status.c_str());
}

void Init(HWND dlg, Dialog &d) {
	const DWORD tab = WS_TABSTOP, group = WS_GROUP;
	Control(dlg, L"STATIC",
	        L"Games that use the PlayStation Network sign in to PSPC Network instead. Its tickets let revival servers "
	        L"know who you are.",
	        SS_LEFT, 7, 7, 296, 18, -1);
	Control(dlg, L"BUTTON", L"No server: sign in locally as", BS_AUTORADIOBUTTON | tab | group, 7, 30, 118, 10, IDC_LOCAL);
	Control(dlg, L"EDIT", Widen(d.s.npOnlineId).c_str(), ES_AUTOHSCROLL | WS_BORDER | tab, 128, 28, 100, 13, IDC_LOCAL_ID);
	Control(dlg, L"BUTTON", L"PSPC Network server:", BS_AUTORADIOBUTTON | tab, 7, 48, 118, 10, IDC_HOSTED);
	Control(dlg, L"EDIT", Widen(d.s.npServer).c_str(), ES_AUTOHSCROLL | WS_BORDER | tab, 128, 46, 175, 13, IDC_SERVER);
	CheckDlgButton(dlg, d.s.npServer.empty() ? IDC_LOCAL : IDC_HOSTED, BST_CHECKED);

	Control(dlg, L"BUTTON", L"Account", BS_GROUPBOX, 7, 64, 296, 74, -1);
	Control(dlg, L"STATIC", L"", SS_LEFT, 15, 76, 280, 18, IDC_STATUS);
	Control(dlg, L"STATIC", L"Online ID", SS_LEFT, 15, 98, 40, 9, -1);
	Control(dlg, L"EDIT", Widen(d.s.npOnlineId).c_str(), ES_AUTOHSCROLL | WS_BORDER | tab | group, 60, 96, 90, 13, IDC_ID);
	Control(dlg, L"STATIC", L"Password", SS_LEFT, 158, 98, 36, 9, -1);
	Control(dlg, L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER | tab, 196, 96, 99, 13, IDC_PASSWORD);
	Control(dlg, L"BUTTON", L"Sign in", BS_PUSHBUTTON | tab, 60, 116, 56, 14, IDC_SIGNIN);
	Control(dlg, L"BUTTON", L"Create account", BS_PUSHBUTTON | tab, 120, 116, 64, 14, IDC_REGISTER);
	Control(dlg, L"BUTTON", L"Sign out", BS_PUSHBUTTON | tab, 239, 116, 56, 14, IDC_SIGNOUT);

	Control(dlg, L"BUTTON", L"Friends", BS_GROUPBOX, 7, 142, 296, 92, -1);
	d.friends = Control(dlg, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | tab | group, 15, 154,
	                    280, 54, IDC_FRIENDS);
	ListView_SetExtendedListViewStyle(d.friends, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	RECT lr;
	GetClientRect(d.friends, &lr);
	const wchar_t *cols[] = {L"Online ID", L""};
	const int widths[] = {lr.right * 55 / 100, lr.right - lr.right * 55 / 100 - 4};
	for (int c = 0; c < 2; c++) {
		LVCOLUMNW col{};
		col.mask = LVCF_TEXT | LVCF_WIDTH;
		col.pszText = (LPWSTR)cols[c];
		col.cx = widths[c];
		ListView_InsertColumn(d.friends, c, &col);
	}
	Control(dlg, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | tab, 15, 214, 100, 13, IDC_FRIEND_ID);
	Control(dlg, L"BUTTON", L"Add / accept", BS_PUSHBUTTON | tab, 119, 213, 60, 14, IDC_FRIEND_ADD);
	Control(dlg, L"BUTTON", L"Remove", BS_PUSHBUTTON | tab, 239, 213, 56, 14, IDC_FRIEND_REMOVE);

	Control(dlg, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | tab | group, 199, 240, 50, 14, IDOK);
	Control(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | tab, 253, 240, 50, 14, IDCANCEL);
	UpdateUi(dlg, d);
	FillFriends(dlg, d);
}

void Fail(HWND dlg, const np::Result &r) { MessageBoxW(dlg, Widen(r.message).c_str(), L"PSPC Network", MB_ICONWARNING); }

// Writes the network keys to pspc.ini (the rest of it stays as it is). Sign-ins and sign-outs are
// saved as they happen, so closing the dialog any way keeps them.
bool Save(HWND dlg, Dialog &d) {
	settings::Settings s;
	settings::Load(d.ini, s);
	s.npServer = d.s.npServer;
	s.npOnlineId = d.s.npOnlineId;
	s.npToken = d.s.npToken;
	if (!settings::Save(d.ini, s)) {
		MessageBoxW(dlg, (L"Could not write " + d.ini.wstring()).c_str(), L"PSPC Network", MB_ICONERROR);
		return false;
	}
	d.changed = true;
	return true;
}

void SignIn(HWND dlg, Dialog &d, bool create) {
	const std::string server = np::NormalizeServerUrl(Text(dlg, IDC_SERVER));
	const std::string id = Text(dlg, IDC_ID), password = Text(dlg, IDC_PASSWORD);
	if (server.empty() || id.empty() || password.empty()) {
		MessageBoxW(dlg, L"Enter the server, your online ID and your password.", L"PSPC Network", MB_ICONINFORMATION);
		return;
	}
	np::NetworkClient client(server);
	client.timeoutMs = 8000;
	np::Profile profile;
	std::string token;
	np::Result r;
	{
		WaitCursor wait;
		std::string name;
		bool registration = true;
		r = client.Info(&name, &registration);
		if (r && create && !registration) {
			r.ok = false;
			r.message = name + " doesn't accept new accounts.";
		}
		if (r) r = create ? client.Register(id, password, "us", &profile, &token) : client.SignIn(id, password, &profile, &token);
	}
	if (!r) return Fail(dlg, r);
	d.s.npServer = client.Server();
	d.s.npOnlineId = profile.onlineId;
	d.s.npToken = token;
	Save(dlg, d);
	SetDlgItemTextW(dlg, IDC_SERVER, Widen(d.s.npServer).c_str());
	SetDlgItemTextW(dlg, IDC_PASSWORD, L"");
	UpdateUi(dlg, d);
	FillFriends(dlg, d);
}

void SignOut(HWND dlg, Dialog &d) {
	{
		WaitCursor wait;
		np::NetworkClient client(d.s.npServer, d.s.npToken);
		client.timeoutMs = 5000;
		client.SignOut();  // forgetting the token is what matters
	}
	d.s.npToken.clear();
	Save(dlg, d);
	UpdateUi(dlg, d);
	FillFriends(dlg, d);
}

void FriendAction(HWND dlg, Dialog &d, bool add) {
	std::string name = Text(dlg, IDC_FRIEND_ID);
	if (!add || name.empty()) {
		const int sel = ListView_GetNextItem(d.friends, -1, LVNI_SELECTED);
		if (sel >= 0) {
			wchar_t buf[64] = {};
			ListView_GetItemText(d.friends, sel, 0, buf, 64);
			name = Narrow(buf);
		}
	}
	if (name.empty()) return;
	np::NetworkClient client(d.s.npServer, d.s.npToken);
	client.timeoutMs = 5000;
	np::Result r;
	{
		WaitCursor wait;
		np::Friend f;
		r = add ? client.AddFriend(name, &f) : client.RemoveFriend(name);
	}
	if (!r) return Fail(dlg, r);
	SetDlgItemTextW(dlg, IDC_FRIEND_ID, L"");
	FillFriends(dlg, d);
}

bool Apply(HWND dlg, Dialog &d) {
	bool changed = false;
	if (!Hosted(dlg)) {
		const std::string id = Text(dlg, IDC_LOCAL_ID);
		changed = !d.s.npServer.empty() || id != d.s.npOnlineId;
		d.s.npServer.clear();
		d.s.npToken.clear();
		d.s.npOnlineId = id;
	} else if (np::NormalizeServerUrl(Text(dlg, IDC_SERVER)) != d.s.npServer) {
		// A different server: its accounts are its own.
		d.s.npServer = np::NormalizeServerUrl(Text(dlg, IDC_SERVER));
		d.s.npToken.clear();
		changed = true;
	}
	return !changed || Save(dlg, d);
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	Dialog *d = g_dlg;
	switch (msg) {
	case WM_INITDIALOG:
		Init(dlg, *d);
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK:
			if (Apply(dlg, *d)) EndDialog(dlg, IDOK);
			return TRUE;
		case IDCANCEL: EndDialog(dlg, d->changed ? IDOK : IDCANCEL); return TRUE;  // sign-ins are saved already
		case IDC_LOCAL:
		case IDC_HOSTED:
			UpdateUi(dlg, *d);
			FillFriends(dlg, *d);
			return TRUE;
		case IDC_SIGNIN: SignIn(dlg, *d, false); return TRUE;
		case IDC_REGISTER: SignIn(dlg, *d, true); return TRUE;
		case IDC_SIGNOUT: SignOut(dlg, *d); return TRUE;
		case IDC_FRIEND_ADD: FriendAction(dlg, *d, true); return TRUE;
		case IDC_FRIEND_REMOVE: FriendAction(dlg, *d, false); return TRUE;
		}
		break;
	}
	return FALSE;
}

}  // namespace

bool ShowNetworkDialog(HWND owner, const fs::path &ini) {
	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
	InitCommonControlsEx(&icc);
	Dialog d;
	d.ini = ini;
	settings::Load(ini, d.s);
	g_dlg = &d;
	std::vector<WORD> tmpl = dlgutil::Template(L"PSPC Network", 310, 261);
	INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (LPCDLGTEMPLATEW)tmpl.data(), owner, DialogProc, 0);
	g_dlg = nullptr;
	return r == IDOK;
}
