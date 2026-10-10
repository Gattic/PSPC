// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Plugins dialog. On a PSP, M33 plugins are copied to ms0:/seplugins, listed in game.txt /
// vsh.txt / pops.txt and switched on in the recovery menu. pspc lays its plugins folder over
// ms0:, and this dialog is the recovery menu's plugin page: one row per listed PRX with a
// checkbox (written back as "path 1" / "path 0" on its line), "Add" to copy a PRX into
// seplugins/ and list it, "Remove" to unlist one (the file stays).
#undef WIN32_LEAN_AND_MEAN
#include "frontend/plugins_dialog.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "common/seplugins.h"
#include "common/settings.h"
#include "frontend/dialog_util.h"

namespace {

using dlgutil::Control;

enum : int {
	IDC_ENABLE = 1001,
	IDC_LIST,
	IDC_ADD,
	IDC_REMOVE,
	IDC_OPEN,
	IDC_TARGET,
	IDC_INFO,
};

struct Row {
	seplugins::List list;
	seplugins::Entry entry;
	bool removed = false;
};

struct Dialog {
	fs::path ini, root;
	settings::Settings s;
	std::vector<Row> rows;
	HWND list = nullptr;
	bool filling = false;
};
Dialog *g_dlg = nullptr;

std::wstring Widen(const std::string &s) {
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

const wchar_t *ListLabel(seplugins::List l) {
	switch (l) {
	case seplugins::List::Vsh: return L"XMB";
	case seplugins::List::Pops: return L"PS1 games";
	default: return L"Games";
	}
}

void Fill(Dialog &d) {
	d.filling = true;
	ListView_DeleteAllItems(d.list);
	int item = 0;
	for (size_t i = 0; i < d.rows.size(); i++) {
		const Row &r = d.rows[i];
		if (r.removed) continue;
		std::wstring path = Widen(r.entry.path);
		LVITEMW it{};
		it.mask = LVIF_TEXT | LVIF_PARAM;
		it.iItem = item;
		it.pszText = path.data();
		it.lParam = (LPARAM)i;
		ListView_InsertItem(d.list, &it);
		ListView_SetItemText(d.list, item, 1, (LPWSTR)ListLabel(r.list));
		const bool found = !seplugins::HostPath(d.root, r.entry.path).empty();
		ListView_SetItemText(d.list, item, 2, (LPWSTR)(found ? L"" : L"file missing"));
		ListView_SetCheckState(d.list, item, r.entry.enabled);
		item++;
	}
	d.filling = false;
}

int SelectedRow(Dialog &d) {
	int item = ListView_GetNextItem(d.list, -1, LVNI_SELECTED);
	if (item < 0) return -1;
	LVITEMW it{};
	it.mask = LVIF_PARAM;
	it.iItem = item;
	ListView_GetItem(d.list, &it);
	return (int)it.lParam;
}

void Init(HWND dlg, Dialog &d) {
	const DWORD tab = WS_TABSTOP, group = WS_GROUP;
	Control(dlg, L"BUTTON", L"Load plugins when a game or the XMB starts", BS_AUTOCHECKBOX | tab | group, 7, 7, 250, 10,
	        IDC_ENABLE);
	CheckDlgButton(dlg, IDC_ENABLE, d.s.plugins ? BST_CHECKED : BST_UNCHECKED);
	d.list = Control(dlg, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | tab, 7, 22, 336,
	                 140, IDC_LIST);
	ListView_SetExtendedListViewStyle(d.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	RECT lr;
	GetClientRect(d.list, &lr);
	const int cw = lr.right - GetSystemMetrics(SM_CXVSCROLL) - 2;
	const wchar_t *cols[] = {L"Plugin", L"Loaded for", L""};
	const int widths[] = {cw * 62 / 100, cw * 20 / 100, cw - cw * 62 / 100 - cw * 20 / 100};
	for (int c = 0; c < 3; c++) {
		LVCOLUMNW col{};
		col.mask = LVCF_TEXT | LVCF_WIDTH;
		col.pszText = (LPWSTR)cols[c];
		col.cx = widths[c];
		ListView_InsertColumn(d.list, c, &col);
	}
	Fill(d);
	Control(dlg, L"BUTTON", L"Add PRX...", BS_PUSHBUTTON | tab | group, 7, 168, 60, 14, IDC_ADD);
	Control(dlg, L"STATIC", L"for", SS_CENTER, 69, 171, 14, 9, -1);
	HWND target = Control(dlg, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | tab, 85, 169, 62, 60, IDC_TARGET);
	for (seplugins::List l : {seplugins::List::Game, seplugins::List::Vsh, seplugins::List::Pops})
		SendMessageW(target, CB_ADDSTRING, 0, (LPARAM)ListLabel(l));
	SendMessageW(target, CB_SETCURSEL, 0, 0);
	Control(dlg, L"BUTTON", L"Remove", BS_PUSHBUTTON | tab, 153, 168, 50, 14, IDC_REMOVE);
	Control(dlg, L"BUTTON", L"Open plugins folder", BS_PUSHBUTTON | tab, 263, 168, 80, 14, IDC_OPEN);
	const std::wstring info = L"The plugins folder (" + d.root.wstring() +
	                          L") is part of the memory stick (ms0:). Plugins' own files (codes, screenshots...) are "
	                          L"kept there. No recovery menu needed: a checked plugin loads.";
	Control(dlg, L"STATIC", info.c_str(), SS_LEFT, 7, 188, 336, 26, IDC_INFO);
	Control(dlg, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | tab | group, 239, 218, 50, 14, IDOK);
	Control(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | tab, 293, 218, 50, 14, IDCANCEL);
}

void SyncChecks(Dialog &d);

void Add(HWND dlg, Dialog &d) {
	wchar_t file[MAX_PATH] = L"";
	OPENFILENAMEW ofn{sizeof(ofn)};
	ofn.hwndOwner = dlg;
	ofn.lpstrFilter = L"PSP plugins (*.prx)\0*.prx\0All files\0*.*\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
	if (!GetOpenFileNameW(&ofn)) return;
	const fs::path src(file);
	std::error_code ec;
	fs::path dst = d.root / "seplugins" / src.filename();
	// A PRX already inside the plugins folder is listed where it is; others are copied in.
	const fs::path rel = fs::relative(src, d.root, ec);
	const bool inside = !ec && !rel.empty() && rel.native().rfind(L"..", 0) != 0;
	if (inside) {
		dst = src;
	} else {
		fs::create_directories(dst.parent_path(), ec);
		if (!fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec)) {
			MessageBoxW(dlg, (L"Could not copy the plugin to " + dst.wstring()).c_str(), L"Plugins", MB_ICONERROR);
			return;
		}
	}
	std::string ms0 = "ms0:/" + PathToUtf8(fs::relative(dst, d.root, ec));
	for (char &c : ms0)
		if (c == '\\') c = '/';
	const int pick = (int)SendDlgItemMessageW(dlg, IDC_TARGET, CB_GETCURSEL, 0, 0);
	const seplugins::List list = pick == 1 ? seplugins::List::Vsh : pick == 2 ? seplugins::List::Pops : seplugins::List::Game;
	Row r;
	r.list = list;
	r.entry.path = ms0;
	r.entry.enabled = true;
	r.entry.line = -1;
	SyncChecks(d);
	d.rows.push_back(r);
	Fill(d);
}

// The checkboxes into the rows (before the list is refilled or saved).
void SyncChecks(Dialog &d) {
	for (int item = 0, n = ListView_GetItemCount(d.list); item < n; item++) {
		LVITEMW it{};
		it.mask = LVIF_PARAM;
		it.iItem = item;
		ListView_GetItem(d.list, &it);
		d.rows[(size_t)it.lParam].entry.enabled = ListView_GetCheckState(d.list, item) != 0;
	}
}

bool Apply(HWND dlg, Dialog &d) {
	SyncChecks(d);
	for (seplugins::List l : seplugins::kLists) {
		std::vector<seplugins::Entry> keep;
		bool any = false, removedAny = false;
		for (const Row &r : d.rows) {
			if (r.list != l) continue;
			any = true;
			if (r.removed) removedAny = true;
			else keep.push_back(r.entry);
		}
		if (!any) continue;
		bool ok;
		if (removedAny) {
			// Rewrite from scratch: removed lines go, the rest keep their order.
			std::error_code ec;
			fs::remove(d.root / "seplugins" / seplugins::FileName(l), ec);
			for (seplugins::Entry &e : keep) e.line = -1;
			ok = seplugins::WriteList(d.root, l, keep);
		} else {
			ok = seplugins::WriteList(d.root, l, keep);
		}
		if (!ok) {
			MessageBoxW(dlg, (L"Could not write " + (d.root / "seplugins" / seplugins::FileName(l)).wstring()).c_str(),
			            L"Plugins", MB_ICONERROR);
			return false;
		}
	}
	const bool enable = IsDlgButtonChecked(dlg, IDC_ENABLE) == BST_CHECKED;
	if (enable != d.s.plugins) {
		settings::Settings s;
		settings::Load(d.ini, s);  // the file as it is now: only this setting changes
		s.plugins = enable;
		if (!settings::Save(d.ini, s)) {
			MessageBoxW(dlg, (L"Could not write " + d.ini.wstring()).c_str(), L"Plugins", MB_ICONERROR);
			return false;
		}
	}
	return true;
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	Dialog *d = g_dlg;
	switch (msg) {
	case WM_INITDIALOG:
		Init(dlg, *d);
		SetFocus(d->list);
		return FALSE;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK:
			if (Apply(dlg, *d)) EndDialog(dlg, IDOK);
			return TRUE;
		case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
		case IDC_ADD: Add(dlg, *d); return TRUE;
		case IDC_REMOVE: {
			const int row = SelectedRow(*d);
			if (row >= 0) {
				SyncChecks(*d);
				d->rows[(size_t)row].removed = true;
				Fill(*d);
			}
			return TRUE;
		}
		case IDC_OPEN: ShellExecuteW(nullptr, L"open", d->root.wstring().c_str(), nullptr, nullptr, SW_SHOW); return TRUE;
		}
		break;
	}
	return FALSE;
}

}  // namespace

bool ShowPluginsDialog(HWND owner, const fs::path &ini, const fs::path &pluginsDir) {
	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
	InitCommonControlsEx(&icc);
	Dialog d;
	d.ini = ini;
	d.root = pluginsDir;
	std::error_code ec;
	fs::create_directories(pluginsDir / "seplugins", ec);
	settings::Load(ini, d.s);
	for (seplugins::List l : seplugins::kLists)
		for (const seplugins::Entry &e : seplugins::ReadList(pluginsDir, l)) d.rows.push_back({l, e, false});
	g_dlg = &d;
	std::vector<WORD> tmpl = dlgutil::Template(L"M33 Plugins", 350, 239);
	INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (LPCDLGTEMPLATEW)tmpl.data(), owner, DialogProc, 0);
	g_dlg = nullptr;
	return r == IDOK;
}
