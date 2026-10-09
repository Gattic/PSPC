// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/dialogs/host_prompt.h"

#include <windows.h>

#include <thread>
#include <vector>

namespace utility::prompt {

namespace {

std::wstring Widen(const std::string &s) {
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string Narrow(const std::wstring &w) {
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

// ---- In-memory dialog template for the text input box.

struct TemplateWriter {
	std::vector<WORD> w;
	void Align() {
		if (w.size() & 1) w.push_back(0);
	}
	void Str(const wchar_t *s) {
		for (; *s; ++s) w.push_back((WORD)*s);
		w.push_back(0);
	}
	void Dword(DWORD v) {
		w.push_back(LOWORD(v));
		w.push_back(HIWORD(v));
	}
	void Item(DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t *text) {
		Align();
		Dword(style | WS_CHILD | WS_VISIBLE);
		Dword(0);  // exStyle
		w.push_back((WORD)x);
		w.push_back((WORD)y);
		w.push_back((WORD)cx);
		w.push_back((WORD)cy);
		w.push_back(id);
		w.push_back(0xFFFF);
		w.push_back(cls);
		Str(text);
		w.push_back(0);  // no creation data
	}
};

constexpr WORD kIdText = 100, kIdEdit = 101;

struct TextBoxArgs {
	std::wstring description;
	std::wstring initial;
	std::wstring result;
};

INT_PTR CALLBACK TextBoxProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_INITDIALOG: {
		auto *args = (TextBoxArgs *)lp;
		SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)args);
		SetDlgItemTextW(dlg, kIdText, args->description.c_str());
		SetDlgItemTextW(dlg, kIdEdit, args->initial.c_str());
		HWND edit = GetDlgItem(dlg, kIdEdit);
		SendMessageW(edit, EM_SETSEL, 0, -1);
		SetFocus(edit);
		SetForegroundWindow(dlg);
		return FALSE;
	}
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK) {
			auto *args = (TextBoxArgs *)GetWindowLongPtrW(dlg, DWLP_USER);
			HWND edit = GetDlgItem(dlg, kIdEdit);
			int len = GetWindowTextLengthW(edit);
			std::wstring s(len + 1, L'\0');
			GetWindowTextW(edit, s.data(), len + 1);
			s.resize(len);
			args->result = s;
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

}  // namespace

std::shared_ptr<State> ShowMessage(const std::string &title, const std::string &text, Buttons buttons) {
	auto st = std::make_shared<State>();
	std::wstring wt = Widen(title), wx = Widen(text);
	std::thread([st, wt, wx, buttons]() {
		UINT type = MB_TOPMOST | MB_SETFOREGROUND;
		type |= buttons == Buttons::YesNo ? MB_YESNO : buttons == Buttons::OkCancel ? MB_OKCANCEL : MB_OK;
		int r = MessageBoxW(nullptr, wx.c_str(), wt.c_str(), type);
		int a = r == IDYES ? YES : r == IDNO ? NO : r == IDOK ? OK : CANCEL;
		st->answer.store(a);
	}).detach();
	return st;
}

std::shared_ptr<State> AskText(const std::string &title, const std::string &description, const std::string &initial) {
	auto st = std::make_shared<State>();
	auto args = std::make_shared<TextBoxArgs>();
	args->description = Widen(description);
	args->initial = Widen(initial);
	std::wstring wt = Widen(title);
	std::thread([st, args, wt]() {
		TemplateWriter t;
		t.Dword(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
		t.Dword(0);   // exStyle
		t.w.push_back(4);  // items
		t.w.push_back(0);
		t.w.push_back(0);
		t.w.push_back(240);
		t.w.push_back(70);
		t.w.push_back(0);  // menu
		t.w.push_back(0);  // class
		t.Str(wt.c_str());
		t.w.push_back(9);  // font size
		t.Str(L"Segoe UI");
		t.Item(SS_LEFT, 7, 7, 226, 18, kIdText, 0x0082, L"");
		t.Item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 7, 28, 226, 14, kIdEdit, 0x0081, L"");
		t.Item(BS_DEFPUSHBUTTON | WS_TABSTOP, 129, 49, 50, 14, IDOK, 0x0080, L"OK");
		t.Item(BS_PUSHBUTTON | WS_TABSTOP, 183, 49, 50, 14, IDCANCEL, 0x0080, L"Cancel");
		INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (LPCDLGTEMPLATEW)t.w.data(), nullptr,
		                                    &TextBoxProc, (LPARAM)args.get());
		if (r == IDOK) {
			st->text = Narrow(args->result);
			st->answer.store(OK);
		} else {
			st->answer.store(CANCEL);
		}
	}).detach();
	return st;
}

}  // namespace utility::prompt
