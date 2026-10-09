// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc-launcher: a game library window for the Games folder.
//   * lists UMD images (ISO/CSO/ZSO/DAX), EBOOT.PBP folders and PBP/PRX/ELF files,
//     with ICON0 and the PARAM.SFO title;
//   * Enter/double-click plays the game with pspc.exe;
//   * the context menu builds its native code (tools/pspc_build.py) or plays it interpreted.
//
//   pspc-launcher [games dir] [--list]   (--list prints the library to the console and exits)
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <commctrl.h>
#include <algorithm>
// GDI+ headers expect min/max, which NOMINMAX removes.
using std::max;
using std::min;
#include <gdiplus.h>
#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "common/fileutil.h"
#include "fs/vfs.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {

constexpr int kIconW = 144, kIconH = 80;
enum : int { ID_LIST = 100, ID_PLAY = 200, ID_PLAY_INTERP, ID_BUILD, ID_REFRESH, ID_OPEN_FOLDER };

struct Game {
	fs::path path;
	std::wstring title;
	std::wstring id;
	std::vector<u8> icon;  // PNG
};

HWND g_list = nullptr;
HIMAGELIST g_images = nullptr;
std::vector<Game> g_games;
fs::path g_gamesDir, g_exeDir;

std::wstring Widen(const std::string &s) {
	if (s.empty()) return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

void ReadSfo(const std::vector<u8> &sfo, Game &g) {
	vfs::ParamSfo p;
	if (!p.Parse(sfo)) return;
	std::string t = p.GetString("TITLE");
	if (!t.empty()) g.title = Widen(t);
	g.id = Widen(p.GetString("DISC_ID"));
}

bool Describe(const fs::path &p, Game &g) {
	g.path = p;
	g.title = p.stem().wstring();
	std::string ext = ToLower(p.extension().string());
	if (ext == ".iso" || ext == ".cso" || ext == ".zso" || ext == ".dax") {
		std::string err;
		auto bd = vfs::OpenBlockDevice(p, &err);
		if (!bd) return false;
		vfs::IsoFileSystem iso(std::shared_ptr<vfs::BlockDevice>(std::move(bd)));
		if (!iso.IsValid()) return false;
		std::vector<u8> sfo;
		if (iso.ReadWholeFile("/PSP_GAME/PARAM.SFO", sfo) == 0) ReadSfo(sfo, g);
		iso.ReadWholeFile("/PSP_GAME/ICON0.PNG", g.icon);
		return true;
	}
	if (ext == ".pbp") {
		vfs::PbpFile pbp;
		if (!pbp.Open(p)) return false;
		std::vector<u8> sfo;
		if (pbp.GetSection(vfs::PbpSection::ParamSfo, sfo)) ReadSfo(sfo, g);
		pbp.GetSection(vfs::PbpSection::Icon0Png, g.icon);
		if (p.filename() == "EBOOT.PBP" && g.title.empty()) g.title = p.parent_path().filename().wstring();
		return true;
	}
	return ext == ".prx" || ext == ".elf";
}

bool IsLooseModule(const fs::path &p) {
	std::string ext = ToLower(p.extension().string());
	return ext == ".prx" || ext == ".elf";
}

void Scan() {
	g_games.clear();
	std::error_code ec;
	if (!fs::exists(g_gamesDir, ec)) fs::create_directories(g_gamesDir, ec);
	for (auto it = fs::recursive_directory_iterator(g_gamesDir, fs::directory_options::skip_permission_denied, ec);
	     it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (ec || !it->is_regular_file(ec)) continue;
		if (it.depth() > 3) continue;
		// Modules inside a game's folders are its libraries, not games.
		if (it.depth() > 0 && IsLooseModule(it->path())) continue;
		Game g;
		if (Describe(it->path(), g)) g_games.push_back(std::move(g));
	}
}

HBITMAP IconBitmap(const std::vector<u8> &png) {
	HBITMAP out = nullptr;
	Gdiplus::Bitmap canvas(kIconW, kIconH, PixelFormat32bppARGB);
	Gdiplus::Graphics gfx(&canvas);
	gfx.Clear(Gdiplus::Color(255, 32, 32, 40));
	if (!png.empty()) {
		HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, png.size());
		memcpy(GlobalLock(mem), png.data(), png.size());
		GlobalUnlock(mem);
		IStream *stream = nullptr;
		if (CreateStreamOnHGlobal(mem, TRUE, &stream) == S_OK) {
			Gdiplus::Bitmap img(stream);
			if (img.GetLastStatus() == Gdiplus::Ok) gfx.DrawImage(&img, 0, 0, kIconW, kIconH);
			stream->Release();
		} else {
			GlobalFree(mem);
		}
	}
	canvas.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &out);
	return out;
}

void Populate() {
	ListView_DeleteAllItems(g_list);
	ImageList_RemoveAll(g_images);
	for (size_t i = 0; i < g_games.size(); i++) {
		HBITMAP bmp = IconBitmap(g_games[i].icon);
		int img = ImageList_Add(g_images, bmp, nullptr);
		DeleteObject(bmp);
		std::wstring label = g_games[i].title + (g_games[i].id.empty() ? L"" : L" [" + g_games[i].id + L"]");
		LVITEMW item{};
		item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
		item.iItem = (int)i;
		item.pszText = label.data();
		item.iImage = img;
		item.lParam = (LPARAM)i;
		ListView_InsertItem(g_list, &item);
	}
}

fs::path FindUp(const fs::path &start, const fs::path &rel) {
	for (fs::path p = start; !p.empty(); p = p.parent_path()) {
		if (fs::exists(p / rel)) return p / rel;
		if (p == p.root_path()) break;
	}
	return {};
}

void Launch(const std::wstring &exe, const std::wstring &args, bool console) {
	std::wstring cmd = L"\"" + exe + L"\" " + args;
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	DWORD flags = console ? CREATE_NEW_CONSOLE : 0;
	if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, g_exeDir.wstring().c_str(), &si,
	                   &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	} else {
		MessageBoxW(nullptr, (L"Could not start:\n" + cmd).c_str(), L"PSPC", MB_ICONERROR);
	}
}

int Selected() {
	int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
	if (i < 0) return -1;
	LVITEMW item{};
	item.mask = LVIF_PARAM;
	item.iItem = i;
	ListView_GetItem(g_list, &item);
	return (int)item.lParam;
}

void Play(int idx, bool interp) {
	if (idx < 0) return;
	std::wstring args = (interp ? L"--interp \"" : L"\"") + g_games[idx].path.wstring() + L"\"";
	Launch((g_exeDir / "pspc.exe").wstring(), args, false);
}

void Build(int idx) {
	if (idx < 0) return;
	fs::path script = FindUp(g_exeDir, fs::path("tools") / "pspc_build.py");
	if (script.empty()) {
		MessageBoxW(nullptr, L"tools\\pspc_build.py not found.", L"PSPC", MB_ICONERROR);
		return;
	}
	wchar_t local[MAX_PATH];
	GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
	fs::path py = fs::path(local) / "Programs" / "Python" / "Python312" / "python.exe";
	std::wstring exe = fs::exists(py) ? py.wstring() : L"python.exe";
	std::wstring args = L"-I \"" + script.wstring() + L"\" \"" + g_games[idx].path.wstring() + L"\" & pause";
	// Run through cmd so the window stays open with the result.
	Launch(L"C:\\Windows\\System32\\cmd.exe", L"/c \"\"" + exe + L"\" " + args + L"\"", true);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_CREATE: {
		g_list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_ICON | LVS_SINGLESEL | LVS_AUTOARRANGE,
		                         0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LIST, nullptr, nullptr);
		ListView_SetExtendedListViewStyle(g_list, LVS_EX_DOUBLEBUFFER);
		ListView_SetBkColor(g_list, RGB(24, 24, 30));
		ListView_SetTextBkColor(g_list, RGB(24, 24, 30));
		ListView_SetTextColor(g_list, RGB(230, 230, 235));
		g_images = ImageList_Create(kIconW, kIconH, ILC_COLOR32, 16, 16);
		ListView_SetImageList(g_list, g_images, LVSIL_NORMAL);
		ListView_SetIconSpacing(g_list, kIconW + 24, kIconH + 48);
		Scan();
		Populate();
		return 0;
	}
	case WM_SIZE: MoveWindow(g_list, 0, 0, LOWORD(lp), HIWORD(lp), TRUE); return 0;
	case WM_NOTIFY: {
		auto *nm = (NMHDR *)lp;
		if (nm->idFrom == ID_LIST && nm->code == NM_DBLCLK) Play(Selected(), false);
		if (nm->idFrom == ID_LIST && nm->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lp)->wVKey == VK_RETURN) Play(Selected(), false);
		if (nm->idFrom == ID_LIST && nm->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lp)->wVKey == VK_F5) {
			Scan();
			Populate();
		}
		return 0;
	}
	case WM_CONTEXTMENU: {
		HMENU m = CreatePopupMenu();
		bool sel = Selected() >= 0;
		AppendMenuW(m, sel ? 0 : MF_GRAYED, ID_PLAY, L"&Play");
		AppendMenuW(m, sel ? 0 : MF_GRAYED, ID_PLAY_INTERP, L"Play (&interpreter only)");
		AppendMenuW(m, sel ? 0 : MF_GRAYED, ID_BUILD, L"&Build native code (AOT)");
		AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(m, 0, ID_REFRESH, L"&Refresh (F5)");
		AppendMenuW(m, 0, ID_OPEN_FOLDER, L"&Open Games folder");
		POINT pt;
		GetCursorPos(&pt);
		TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
		DestroyMenu(m);
		return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case ID_PLAY: Play(Selected(), false); break;
		case ID_PLAY_INTERP: Play(Selected(), true); break;
		case ID_BUILD: Build(Selected()); break;
		case ID_REFRESH:
			Scan();
			Populate();
			break;
		case ID_OPEN_FOLDER: ShellExecuteW(nullptr, L"open", g_gamesDir.wstring().c_str(), nullptr, nullptr, SW_SHOW); break;
		}
		return 0;
	case WM_DESTROY: PostQuitMessage(0); return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
	g_exeDir = ExecutableDir();
	int argc = 0;
	wchar_t **argv = cmdLine && *cmdLine ? CommandLineToArgvW(cmdLine, &argc) : nullptr;
	bool list = false;
	for (int i = 0; i < argc; i++) {
		if (std::wstring(argv[i]) == L"--list") list = true;
		else g_gamesDir = argv[i];
	}
	if (argv) LocalFree(argv);
	if (g_gamesDir.empty()) {
		fs::path found = FindUp(g_exeDir, "Games");
		g_gamesDir = found.empty() ? g_exeDir / "Games" : found;
	}
	if (list) {
		// Redirected output (a pipe or file) is already stdout; otherwise borrow the parent's console.
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		if (h == nullptr || h == INVALID_HANDLE_VALUE) {
			if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
			FILE *out = nullptr;
			freopen_s(&out, "CONOUT$", "w", stdout);
		}
		Scan();
		for (const Game &g : g_games)
			printf("%s\t%s\t%s\ticon=%zu bytes\n", PathToUtf8(g.path).c_str(), PathToUtf8(fs::path(g.id)).c_str(),
			       PathToUtf8(fs::path(g.title)).c_str(), g.icon.size());
		fflush(stdout);
		return 0;
	}

	Gdiplus::GdiplusStartupInput gin;
	ULONG_PTR token;
	Gdiplus::GdiplusStartup(&token, &gin, nullptr);
	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
	InitCommonControlsEx(&icc);

	WNDCLASSW wc{};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.lpszClassName = L"PspcLauncher";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 30));
	RegisterClassW(&wc);
	std::wstring title = L"PSPC - " + g_gamesDir.wstring();
	HWND hwnd = CreateWindowW(wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000,
	                          640, nullptr, nullptr, inst, nullptr);
	ShowWindow(hwnd, show);
	MSG m;
	while (GetMessageW(&m, nullptr, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	Gdiplus::GdiplusShutdown(token);
	return 0;
}
