// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc-launcher's dialogs are built in code: an empty in-memory template, controls created in
// WM_INITDIALOG at positions in dialog units.
#pragma once

#include <windows.h>

#include <vector>

namespace dlgutil {

// A child control at a position in dialog units.
inline HWND Control(HWND dlg, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id,
                    DWORD exStyle = 0) {
	RECT r{x, y, x + w, y + h};
	MapDialogRect(dlg, &r);
	HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left,
	                         r.bottom - r.top, dlg, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
	SendMessageW(c, WM_SETFONT, (WPARAM)SendMessageW(dlg, WM_GETFONT, 0, 0), FALSE);
	return c;
}

// An empty dialog template (DLGTEMPLATE + title + font) of cx x cy dialog units.
inline std::vector<WORD> Template(const wchar_t *title, short cx, short cy) {
	std::vector<WORD> t;
	auto dword = [&](DWORD v) {
		t.push_back(LOWORD(v));
		t.push_back(HIWORD(v));
	};
	auto str = [&](const wchar_t *s) {
		for (; *s; s++) t.push_back(*s);
		t.push_back(0);
	};
	dword(DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
	dword(0);        // extended style
	t.push_back(0);  // no items
	t.push_back(0);  // x
	t.push_back(0);  // y
	t.push_back((WORD)cx);
	t.push_back((WORD)cy);
	t.push_back(0);  // no menu
	t.push_back(0);  // default class
	str(title);
	t.push_back(9);  // point size
	str(L"Segoe UI");
	return t;
}

}  // namespace dlgutil
