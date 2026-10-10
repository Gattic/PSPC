// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Settings dialog: window, native code, firmware version, audio, and the keyboard/controller
// mapping. Built in code (an empty in-memory dialog template; controls created in WM_INITDIALOG).
// Rebinding a key: click an entry (or press Enter on it), then press the key.
#undef WIN32_LEAN_AND_MEAN
#include "frontend/settings_dialog.h"

#include <commctrl.h>

#include <string>
#include <vector>

#include "common/settings.h"
#include "frontend/dialog_util.h"

namespace {

using dlgutil::Control;
using dlgutil::Template;

enum : int {
	IDC_SCALE = 1001,
	IDC_FULLSCREEN,
	IDC_VSYNC,
	IDC_AOT,
	IDC_FW,
	IDC_VOLUME,
	IDC_VOLUME_VALUE,
	IDC_MUTE,
	IDC_CONTROLS,
	IDC_HINT,
	IDC_PAD,
	IDC_CLEAR,
	IDC_DEFAULTS,
};

const wchar_t *const kHint = L"Click an entry, then press the key to use for it. F11 (fullscreen) and F12 (screenshot) are reserved.";

struct Dialog {
	fs::path ini;
	settings::Settings s;
	HWND list = nullptr;
	int capturing = -1;  // control waiting for a key, or -1
	UINT capturedVk = 0;  // swallow the WM_CHAR/WM_SYSKEYUP that follow the captured key
	bool eatChar = false;
};
Dialog *g_dlg = nullptr;  // the dialog is modal: one at a time

std::wstring Widen(const std::string &s) {
	if (s.empty()) return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string Narrow(const std::wstring &w) {
	if (w.empty()) return "";
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

// The SDL key name (SDL_GetKeyName spelling) of a WM_KEYDOWN, "" for keys we can't bind.
std::string KeyName(UINT vk, LPARAM lp) {
	bool ext = (lp >> 24) & 1;
	UINT scan = (lp >> 16) & 0xFF;
	switch (vk) {
	case VK_SHIFT: return MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? "Right Shift" : "Left Shift";
	case VK_CONTROL: return ext ? "Right Ctrl" : "Left Ctrl";
	case VK_MENU: return ext ? "Right Alt" : "Left Alt";
	case VK_LWIN: return "Left GUI";
	case VK_RWIN: return "Right GUI";
	case VK_RETURN: return ext ? "Keypad Enter" : "Return";
	// Without the extended bit these come from the keypad (NumLock off); SDL names them by key.
	case VK_UP: return ext ? "Up" : "Keypad 8";
	case VK_DOWN: return ext ? "Down" : "Keypad 2";
	case VK_LEFT: return ext ? "Left" : "Keypad 4";
	case VK_RIGHT: return ext ? "Right" : "Keypad 6";
	case VK_HOME: return ext ? "Home" : "Keypad 7";
	case VK_END: return ext ? "End" : "Keypad 1";
	case VK_PRIOR: return ext ? "PageUp" : "Keypad 9";
	case VK_NEXT: return ext ? "PageDown" : "Keypad 3";
	case VK_INSERT: return ext ? "Insert" : "Keypad 0";
	case VK_DELETE: return ext ? "Delete" : "Keypad .";
	case VK_CLEAR: return "Keypad 5";
	case VK_MULTIPLY: return "Keypad *";
	case VK_ADD: return "Keypad +";
	case VK_SUBTRACT: return "Keypad -";
	case VK_DECIMAL: return "Keypad .";
	case VK_DIVIDE: return "Keypad /";
	case VK_SPACE: return "Space";
	case VK_TAB: return "Tab";
	case VK_BACK: return "Backspace";
	case VK_ESCAPE: return "Escape";
	case VK_CAPITAL: return "CapsLock";
	case VK_PAUSE: return "Pause";
	case VK_SCROLL: return "ScrollLock";
	case VK_NUMLOCK: return "Numlock";
	case VK_APPS: return "Application";
	default: break;
	}
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Keypad " + std::to_string(vk - VK_NUMPAD0);
	if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
	// Printable keys: SDL names them by the character they type unshifted (letters upper case).
	wchar_t ch = (wchar_t)(MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF);
	if (ch == L',') return "Comma";
	if (ch >= L'a' && ch <= L'z') ch = (wchar_t)(ch - L'a' + L'A');
	if (ch > L' ') return Narrow(std::wstring(1, ch));
	return "";
}

// "Space,Right Shift" -> "Space, Right Shift" for display.
std::wstring Pretty(const std::string &list) {
	std::wstring out;
	for (const std::string &n : settings::SplitList(list)) out += (out.empty() ? L"" : L", ") + Widen(n);
	return out.empty() ? L"-" : out;
}

void UpdateRow(Dialog &d, int i) {
	std::wstring keys = d.capturing == i ? L"Press a key..." : Pretty(d.s.keys[i]);
	std::wstring pads = Pretty(d.s.pads[i]);
	if (d.s.pads[i].empty() && settings::kControls[i].button == 0 && d.s.padStick != "none")
		pads = d.s.padStick == "both" ? std::wstring(L"either stick") : Widen(d.s.padStick) + L" stick";
	ListView_SetItemText(d.list, i, 1, keys.data());
	ListView_SetItemText(d.list, i, 2, pads.data());
}

int SelectedRow(const Dialog &d) { return ListView_GetNextItem(d.list, -1, LVNI_SELECTED); }

void StopCapture(Dialog &d) {
	int i = d.capturing;
	d.capturing = -1;
	if (i >= 0) UpdateRow(d, i);
}

void StartCapture(Dialog &d, int i) {
	if (i < 0) return;
	StopCapture(d);
	d.capturing = i;
	SetFocus(d.list);
	UpdateRow(d, i);
	SetDlgItemTextW(GetParent(d.list), IDC_HINT, kHint);
}

// The controller combo shows the first button bound to the selected entry.
void SyncPadCombo(Dialog &d) {
	HWND combo = GetDlgItem(GetParent(d.list), IDC_PAD);
	int row = SelectedRow(d);
	EnableWindow(combo, row >= 0);
	EnableWindow(GetDlgItem(GetParent(d.list), IDC_CLEAR), row >= 0);
	int pick = 0;
	if (row >= 0) {
		std::vector<std::string> names = settings::SplitList(d.s.pads[row]);
		for (size_t i = 0; !names.empty() && i < settings::kNumPadNames; i++)
			if (names[0] == settings::kPadNames[i]) pick = (int)i + 1;
	}
	SendMessageW(combo, CB_SETCURSEL, pick, 0);
}

LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
	Dialog &d = *(Dialog *)ref;
	switch (msg) {
	case WM_GETDLGCODE: {
		LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
		if (d.capturing >= 0 || d.eatChar) return r | DLGC_WANTALLKEYS;
		const MSG *m = (const MSG *)lp;
		if (m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN) return r | DLGC_WANTMESSAGE;
		return r;
	}
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (d.capturing >= 0) {
			if (wp == VK_F11 || wp == VK_F12) {
				MessageBeep(MB_ICONWARNING);
				SetDlgItemTextW(GetParent(hwnd), IDC_HINT, L"F11 and F12 are reserved for fullscreen and screenshots: pick another key.");
				return 0;
			}
			std::string name = KeyName((UINT)wp, lp);
			if (name.empty()) {
				MessageBeep(MB_ICONWARNING);
				return 0;
			}
			int i = d.capturing;
			d.s.keys[i] = name;
			d.capturedVk = (UINT)wp;
			d.eatChar = true;
			StopCapture(d);
			return 0;
		}
		if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_F2)) {
			StartCapture(d, SelectedRow(d));
			return 0;
		}
		break;
	case WM_CHAR:
	case WM_SYSCHAR:
	case WM_DEADCHAR:
	case WM_SYSDEADCHAR:
		if (d.capturing >= 0 || d.eatChar) {
			d.eatChar = false;
			return 0;
		}
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		// Releasing a captured Alt would otherwise open the system menu.
		if (d.capturing >= 0 || (d.capturedVk && wp == d.capturedVk)) {
			d.capturedVk = 0;
			d.eatChar = false;
			return 0;
		}
		break;
	case WM_KILLFOCUS: StopCapture(d); break;
	case WM_NCDESTROY: RemoveWindowSubclass(hwnd, ListProc, 0); break;
	}
	return DefSubclassProc(hwnd, msg, wp, lp);
}

void SetVolumeLabel(HWND dlg) {
	int v = (int)SendDlgItemMessageW(dlg, IDC_VOLUME, TBM_GETPOS, 0, 0);
	SetDlgItemTextW(dlg, IDC_VOLUME_VALUE, (std::to_wstring(v) + L"%").c_str());
}

void Init(HWND dlg, Dialog &d) {
	const DWORD tab = WS_TABSTOP, group = WS_GROUP;
	// Left column: window, emulation, audio.
	Control(dlg, L"BUTTON", L"Window", BS_GROUPBOX, 7, 5, 172, 62, -1);
	Control(dlg, L"STATIC", L"Scale", SS_LEFT, 15, 20, 50, 9, -1);
	HWND scale = Control(dlg, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | tab | group, 72, 18, 99, 80, IDC_SCALE);
	for (int n = 1; n <= 4; n++) {
		std::wstring item = std::to_wstring(n) + L"x  (" + std::to_wstring(480 * n) + L" x " + std::to_wstring(272 * n) + L")";
		SendMessageW(scale, CB_ADDSTRING, 0, (LPARAM)item.c_str());
	}
	SendMessageW(scale, CB_SETCURSEL, d.s.scale - 1, 0);
	Control(dlg, L"BUTTON", L"Start in fullscreen (F11 toggles)", BS_AUTOCHECKBOX | tab, 15, 36, 158, 10, IDC_FULLSCREEN);
	Control(dlg, L"BUTTON", L"Vertical sync", BS_AUTOCHECKBOX | tab, 15, 50, 158, 10, IDC_VSYNC);
	CheckDlgButton(dlg, IDC_FULLSCREEN, d.s.fullscreen ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(dlg, IDC_VSYNC, d.s.vsync ? BST_CHECKED : BST_UNCHECKED);

	Control(dlg, L"BUTTON", L"Emulation", BS_GROUPBOX, 7, 72, 172, 50, -1);
	Control(dlg, L"BUTTON", L"Use native code (AOT) when it is built", BS_AUTOCHECKBOX | tab | group, 15, 86, 158, 10, IDC_AOT);
	CheckDlgButton(dlg, IDC_AOT, d.s.aot ? BST_CHECKED : BST_UNCHECKED);
	Control(dlg, L"STATIC", L"Firmware version", SS_LEFT, 15, 104, 60, 9, -1);
	HWND fw = Control(dlg, L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL | tab, 92, 102, 79, 90, IDC_FW);
	for (const wchar_t *v : {L"6.60", L"6.61", L"6.39", L"6.20", L"5.50", L"5.00", L"4.01", L"3.90"})
		SendMessageW(fw, CB_ADDSTRING, 0, (LPARAM)v);
	SetWindowTextW(fw, Widen(d.s.fw).c_str());

	Control(dlg, L"BUTTON", L"Audio", BS_GROUPBOX, 7, 127, 172, 46, -1);
	Control(dlg, L"STATIC", L"Volume", SS_LEFT, 15, 143, 34, 9, -1);
	HWND vol = Control(dlg, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | tab | group, 48, 140, 98, 14, IDC_VOLUME);
	SendMessageW(vol, TBM_SETRANGE, FALSE, MAKELPARAM(0, 100));
	SendMessageW(vol, TBM_SETPAGESIZE, 0, 10);
	SendMessageW(vol, TBM_SETPOS, TRUE, d.s.volume);
	Control(dlg, L"STATIC", L"", SS_RIGHT, 147, 143, 24, 9, IDC_VOLUME_VALUE);
	SetVolumeLabel(dlg);
	Control(dlg, L"BUTTON", L"Mute", BS_AUTOCHECKBOX | tab, 15, 157, 100, 10, IDC_MUTE);
	CheckDlgButton(dlg, IDC_MUTE, d.s.mute ? BST_CHECKED : BST_UNCHECKED);

	Control(dlg, L"STATIC",
	        L"Saved to pspc.ini next to pspc.exe. A game picks the settings up when it starts; command-line "
	        L"options override them.",
	        SS_LEFT, 9, 180, 168, 36, -1);

	// Right column: the input mapping.
	Control(dlg, L"BUTTON", L"Controls", BS_GROUPBOX, 186, 5, 222, 228, -1);
	d.list = Control(dlg, WC_LISTVIEWW, L"",
	                 LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_BORDER | tab | group, 194,
	                 18, 206, 158, IDC_CONTROLS);
	ListView_SetExtendedListViewStyle(d.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	RECT lr;
	GetClientRect(d.list, &lr);
	int cw = lr.right - GetSystemMetrics(SM_CXVSCROLL) - 2;
	const wchar_t *cols[] = {L"PSP", L"Keyboard", L"Controller"};
	const int widths[] = {cw * 30 / 100, cw * 36 / 100, cw - cw * 30 / 100 - cw * 36 / 100};
	for (int c = 0; c < 3; c++) {
		LVCOLUMNW col{};
		col.mask = LVCF_TEXT | LVCF_WIDTH;
		col.pszText = (LPWSTR)cols[c];
		col.cx = widths[c];
		ListView_InsertColumn(d.list, c, &col);
	}
	for (size_t i = 0; i < settings::kNumControls; i++) {
		std::wstring label = Widen(settings::kControls[i].label);
		LVITEMW item{};
		item.mask = LVIF_TEXT;
		item.iItem = (int)i;
		item.pszText = label.data();
		ListView_InsertItem(d.list, &item);
		UpdateRow(d, (int)i);
	}
	SetWindowSubclass(d.list, ListProc, 0, (DWORD_PTR)&d);
	Control(dlg, L"STATIC", kHint, SS_LEFT, 194, 179, 206, 18, IDC_HINT);
	Control(dlg, L"STATIC", L"Controller", SS_LEFT, 194, 202, 40, 9, -1);
	HWND pad = Control(dlg, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | tab | group, 236, 200, 92, 140, IDC_PAD);
	SendMessageW(pad, CB_ADDSTRING, 0, (LPARAM)L"(none)");
	for (size_t i = 0; i < settings::kNumPadNames; i++)
		SendMessageW(pad, CB_ADDSTRING, 0, (LPARAM)Widen(settings::kPadNames[i]).c_str());
	Control(dlg, L"BUTTON", L"Clear key", BS_PUSHBUTTON | tab, 334, 199, 66, 14, IDC_CLEAR);
	Control(dlg, L"BUTTON", L"Reset controls to defaults", BS_PUSHBUTTON | tab, 194, 215, 112, 14, IDC_DEFAULTS);

	Control(dlg, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | tab | group, 304, 240, 50, 14, IDOK);
	Control(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | tab, 358, 240, 50, 14, IDCANCEL);
	SyncPadCombo(d);
}

bool Apply(HWND dlg, Dialog &d) {
	settings::Settings s = d.s;
	s.scale = (int)SendDlgItemMessageW(dlg, IDC_SCALE, CB_GETCURSEL, 0, 0) + 1;
	s.fullscreen = IsDlgButtonChecked(dlg, IDC_FULLSCREEN) == BST_CHECKED;
	s.vsync = IsDlgButtonChecked(dlg, IDC_VSYNC) == BST_CHECKED;
	s.aot = IsDlgButtonChecked(dlg, IDC_AOT) == BST_CHECKED;
	s.mute = IsDlgButtonChecked(dlg, IDC_MUTE) == BST_CHECKED;
	s.volume = (int)SendDlgItemMessageW(dlg, IDC_VOLUME, TBM_GETPOS, 0, 0);
	wchar_t fw[32] = {};
	GetDlgItemTextW(dlg, IDC_FW, fw, 32);
	std::string err;
	if (!settings::Set(s, "fw", Narrow(fw), &err)) {
		MessageBoxW(dlg, L"The firmware version should look like 6.60.", L"Settings", MB_ICONWARNING);
		SetFocus(GetDlgItem(dlg, IDC_FW));
		return false;
	}
	if (!settings::Save(d.ini, s)) {
		MessageBoxW(dlg, (L"Could not write " + d.ini.wstring()).c_str(), L"Settings", MB_ICONERROR);
		return false;
	}
	d.s = s;
	return true;
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	Dialog *d = g_dlg;
	switch (msg) {
	case WM_INITDIALOG:
		Init(dlg, *d);
		SetFocus(GetDlgItem(dlg, IDC_SCALE));
		return FALSE;  // focus set
	case WM_HSCROLL:
		if ((HWND)lp == GetDlgItem(dlg, IDC_VOLUME)) SetVolumeLabel(dlg);
		return TRUE;
	case WM_NOTIFY: {
		auto *nm = (NMHDR *)lp;
		if (nm->idFrom != IDC_CONTROLS) break;
		if (nm->code == NM_CLICK || nm->code == NM_DBLCLK) StartCapture(*d, ((NMITEMACTIVATE *)lp)->iItem);
		if (nm->code == LVN_ITEMCHANGED && (((NMLISTVIEW *)lp)->uChanged & LVIF_STATE)) SyncPadCombo(*d);
		break;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK:
			if (Apply(dlg, *d)) EndDialog(dlg, IDOK);
			return TRUE;
		case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
		case IDC_PAD:
			if (HIWORD(wp) == CBN_SELCHANGE) {
				int row = SelectedRow(*d);
				int pick = (int)SendDlgItemMessageW(dlg, IDC_PAD, CB_GETCURSEL, 0, 0);
				if (row >= 0 && pick >= 0) {
					d->s.pads[row] = pick == 0 ? "" : settings::kPadNames[pick - 1];
					UpdateRow(*d, row);
				}
			}
			return TRUE;
		case IDC_CLEAR: {
			int row = SelectedRow(*d);
			if (row >= 0) {
				StopCapture(*d);
				d->s.keys[row].clear();
				UpdateRow(*d, row);
			}
			return TRUE;
		}
		case IDC_DEFAULTS: {
			StopCapture(*d);
			settings::Settings defaults;
			d->s.keys = defaults.keys;
			d->s.pads = defaults.pads;
			for (size_t i = 0; i < settings::kNumControls; i++) UpdateRow(*d, (int)i);
			SyncPadCombo(*d);
			return TRUE;
		}
		}
		break;
	}
	return FALSE;
}

}  // namespace

bool ShowSettingsDialog(HWND owner, const fs::path &ini) {
	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
	InitCommonControlsEx(&icc);
	Dialog d;
	d.ini = ini;
	settings::Load(ini, d.s);
	g_dlg = &d;
	std::vector<WORD> tmpl = Template(L"PSPC Settings", 415, 261);
	INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (LPCDLGTEMPLATEW)tmpl.data(), owner, DialogProc, 0);
	g_dlg = nullptr;
	return r == IDOK;
}
