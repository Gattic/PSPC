// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc-launcher: a game library window for the Games folder.
//   * lists UMD images (ISO/CSO/ZSO/DAX), EBOOT.PBP folders and PBP/PRX/ELF files,
//     with ICON0, the PARAM.SFO title and whether the game's native (AOT) code is built;
//   * the selected game is shown on the right over its PIC1 backdrop;
//   * Enter/double-click plays the game with pspc.exe, offering to build its native code first;
//   * the toolbar and the context menu build native code (tools/pspc_build.py), play
//     interpreted, and open the Settings dialog (pspc.ini);
//   * "Boot XMB" starts the firmware's XMB (pspc --vsh) once tools/pspc_install_fw.py installed it.
//
//   pspc-launcher [games dir] [--list]   (--list prints the library to the console and exits)
//
// Native code status comes from cache\aot\games.json, which pspc_build.py writes: the modules
// each game was built from, by SHA-1; a module is current when <sha1>.dll exists and its
// .semver stamp matches this build's PSPC_SEM_VERSION.
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
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/fileutil.h"
#include "common/settings.h"
#include "frontend/settings_dialog.h"
#include "fs/vfs.h"
#include "pspc_sem_version.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
// Common controls 6 (themed controls; the Settings dialog's list and trackbar).
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr int kIconW = 144, kIconH = 80;
constexpr int kToolbarH = 44;
enum : int {
	ID_LIST = 100,
	ID_PLAY = 200,
	ID_PLAY_INTERP,
	ID_BUILD,
	ID_REFRESH,
	ID_OPEN_FOLDER,
	ID_SETTINGS,
	ID_BOOT_XMB,
	ID_TIMER_JOBS = 300,
};
const COLORREF kBackground = RGB(24, 24, 30), kToolbar = RGB(34, 34, 43), kText = RGB(230, 230, 235);

// ---------------------------------------------------------------------------------------------
// A minimal JSON reader (for games.json).

struct Json {
	enum Type { Null, Bool, Number, String, Array, Object } type = Null;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<Json> items;
	std::vector<std::pair<std::string, Json>> members;
	const Json *Get(const char *key) const {
		for (const auto &m : members)
			if (m.first == key) return &m.second;
		return nullptr;
	}
	bool IsTrue() const { return type == Bool && b; }
};

class JsonReader {
public:
	explicit JsonReader(const std::string &text) : p_(text.c_str()), end_(text.c_str() + text.size()) {}
	bool Read(Json &out) {
		if (!Value(out, 0)) return false;
		Space();
		return p_ == end_;
	}

private:
	void Space() {
		while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) p_++;
	}
	bool Word(const char *w) {
		size_t n = strlen(w);
		if ((size_t)(end_ - p_) < n || memcmp(p_, w, n) != 0) return false;
		p_ += n;
		return true;
	}
	void Utf8(u32 cp, std::string &out) {
		if (cp < 0x80) out += (char)cp;
		else if (cp < 0x800) out += {(char)(0xC0 | cp >> 6), (char)(0x80 | (cp & 0x3F))};
		else if (cp < 0x10000) out += {(char)(0xE0 | cp >> 12), (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F))};
		else
			out += {(char)(0xF0 | cp >> 18), (char)(0x80 | ((cp >> 12) & 0x3F)), (char)(0x80 | ((cp >> 6) & 0x3F)),
			        (char)(0x80 | (cp & 0x3F))};
	}
	bool Hex4(u32 &v) {
		if (end_ - p_ < 4) return false;
		v = 0;
		for (int i = 0; i < 4; i++) {
			char c = *p_++;
			v <<= 4;
			if (c >= '0' && c <= '9') v |= c - '0';
			else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
			else return false;
		}
		return true;
	}
	bool Str(std::string &out) {
		if (p_ >= end_ || *p_ != '"') return false;
		p_++;
		while (p_ < end_ && *p_ != '"') {
			char c = *p_++;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (p_ >= end_) return false;
			c = *p_++;
			switch (c) {
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u': {
				u32 cp;
				if (!Hex4(cp)) return false;
				if (cp >= 0xD800 && cp < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
					p_ += 2;
					u32 lo;
					if (!Hex4(lo)) return false;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				Utf8(cp, out);
				break;
			}
			default: out += c; break;  // \" \\ \/
			}
		}
		if (p_ >= end_) return false;
		p_++;
		return true;
	}
	bool Value(Json &v, int depth) {
		Space();
		if (p_ >= end_ || depth > 32) return false;
		if (*p_ == '{' || *p_ == '[') {
			bool object = *p_++ == '{';
			v.type = object ? Json::Object : Json::Array;
			Space();
			if (p_ < end_ && *p_ == (object ? '}' : ']')) {
				p_++;
				return true;
			}
			for (;;) {
				Json child;
				std::string key;
				if (object) {
					Space();
					if (!Str(key)) return false;
					Space();
					if (p_ >= end_ || *p_++ != ':') return false;
				}
				if (!Value(child, depth + 1)) return false;
				if (object) v.members.emplace_back(std::move(key), std::move(child));
				else v.items.push_back(std::move(child));
				Space();
				if (p_ < end_ && *p_ == ',') {
					p_++;
					continue;
				}
				if (p_ < end_ && *p_ == (object ? '}' : ']')) {
					p_++;
					return true;
				}
				return false;
			}
		}
		if (*p_ == '"') {
			v.type = Json::String;
			return Str(v.s);
		}
		if (Word("true") || Word("false")) {
			v.type = Json::Bool;
			v.b = p_[-1] == 'e' && p_[-2] == 'u';
			return true;
		}
		if (Word("null")) return true;
		char *e = nullptr;
		v.n = strtod(p_, &e);  // the text is NUL-terminated
		if (e == p_ || e > end_) return false;
		v.type = Json::Number;
		p_ = e;
		return true;
	}
	const char *p_, *end_;
};

// ---------------------------------------------------------------------------------------------
// The library.

enum class Native { None, Partial, Stale, Built };

struct NativeInfo {
	Native state = Native::None;
	int built = 0, total = 0;  // distinct modules with current DLLs / that built successfully
	int failed = 0;            // modules whose build failed
	bool operator==(const NativeInfo &o) const = default;
};

struct Game {
	fs::path path;
	std::wstring title;
	std::wstring id;
	u64 size = 0;
	std::vector<u8> icon;  // PNG
	NativeInfo native;
	bool pic1Loaded = false;
	std::unique_ptr<Gdiplus::Bitmap> pic1, iconImage;  // decoded on first display
};

struct BuildJob {
	HANDLE process;
	fs::path game;
	bool playAfter;
};

HWND g_main = nullptr, g_list = nullptr, g_detail = nullptr;
std::vector<HWND> g_tools;
HIMAGELIST g_images = nullptr;
HFONT g_uiFont = nullptr;
std::vector<Game> g_games;
std::vector<BuildJob> g_jobs;
fs::path g_gamesDir, g_exeDir;
settings::Settings g_settings;

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

bool IsDisc(const fs::path &p) {
	std::string ext = ToLower(p.extension().string());
	return ext == ".iso" || ext == ".cso" || ext == ".zso" || ext == ".dax";
}

bool Describe(const fs::path &p, Game &g) {
	g.path = p;
	g.title = p.stem().wstring();
	std::error_code ec;
	g.size = fs::file_size(p, ec);
	std::string ext = ToLower(p.extension().string());
	if (IsDisc(p)) {
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

std::vector<u8> ReadPic1(const fs::path &p) {
	std::vector<u8> png;
	if (IsDisc(p)) {
		std::string err;
		if (auto bd = vfs::OpenBlockDevice(p, &err)) {
			vfs::IsoFileSystem iso(std::shared_ptr<vfs::BlockDevice>(std::move(bd)));
			if (iso.IsValid() && iso.ReadWholeFile("/PSP_GAME/PIC1.PNG", png) != 0) png.clear();
		}
	} else if (ToLower(p.extension().string()) == ".pbp") {
		vfs::PbpFile pbp;
		if (pbp.Open(p)) pbp.GetSection(vfs::PbpSection::Pic1Png, png);
	}
	return png;
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

// ---------------------------------------------------------------------------------------------
// Native code status (cache\aot\games.json, written by tools\pspc_build.py).

fs::path CacheDir() { return g_exeDir / "cache" / "aot"; }

std::string ReadText(const fs::path &p) {
	std::vector<u8> d;
	if (!ReadWholeFile(p, d)) return "";
	return std::string(d.begin(), d.end());
}

std::wstring NormPath(const fs::path &p) {
	std::error_code ec;
	fs::path c = fs::weakly_canonical(p, ec);
	std::wstring w = (ec ? p : c).wstring();
	CharLowerBuffW(w.data(), (DWORD)w.size());
	return w;
}

NativeInfo NativeStatus(const Json *entry) {
	NativeInfo ni;
	const Json *modules = entry ? entry->Get("modules") : nullptr;
	if (!modules || modules->type != Json::Array) return ni;
	char want[16];
	snprintf(want, sizeof(want), "0x%08x", (unsigned)PSPC_SEM_VERSION);
	std::vector<std::string> seen;
	int stale = 0;
	for (const Json &m : modules->items) {
		const Json *ok = m.Get("ok"), *sha = m.Get("sha1");
		if (!ok || !ok->IsTrue() || !sha || sha->type != Json::String || sha->s.size() != 40) {
			ni.failed++;
			continue;
		}
		// BOOT.BIN and EBOOT.BIN are often the same module.
		if (std::find(seen.begin(), seen.end(), sha->s) != seen.end()) continue;
		seen.push_back(sha->s);
		ni.total++;
		std::error_code ec;
		if (!fs::exists(CacheDir() / (sha->s + ".dll"), ec)) continue;
		std::string stamp = ReadText(CacheDir() / (sha->s + ".semver"));
		while (!stamp.empty() && (stamp.back() == '\n' || stamp.back() == '\r' || stamp.back() == ' ')) stamp.pop_back();
		if (stamp == want) ni.built++;
		else stale++;
	}
	if (ni.built > 0 && ni.built == ni.total) ni.state = Native::Built;
	else if (ni.built > 0) ni.state = Native::Partial;
	else if (stale > 0) ni.state = Native::Stale;
	return ni;
}

// Re-reads the index; returns true if any game's status changed.
bool RefreshNative() {
	Json index;
	std::string text = ReadText(CacheDir() / "games.json");
	const Json *games = nullptr;
	if (!text.empty() && JsonReader(text).Read(index)) games = index.Get("games");
	std::vector<std::pair<std::wstring, const Json *>> entries;
	if (games && games->type == Json::Object)
		for (const auto &m : games->members) entries.emplace_back(NormPath(fs::u8path(m.first)), &m.second);
	bool changed = false;
	for (Game &g : g_games) {
		const Json *entry = nullptr;
		std::wstring key = NormPath(g.path);
		for (const auto &e : entries)
			if (e.first == key) entry = e.second;
		// A disc image that was moved or renamed since: the same disc ID and size.
		for (size_t i = 0; !entry && !g.id.empty() && i < entries.size(); i++) {
			const Json *id = entries[i].second->Get("disc_id"), *size = entries[i].second->Get("size");
			if (id && id->type == Json::String && Widen(id->s) == g.id && size && size->type == Json::Number &&
			    (u64)size->n == g.size)
				entry = entries[i].second;
		}
		NativeInfo ni = NativeStatus(entry);
		changed |= !(ni == g.native);
		g.native = ni;
	}
	return changed;
}

bool Building(const fs::path &game) {
	for (const BuildJob &j : g_jobs)
		if (j.game == game) return true;
	return false;
}

// ---------------------------------------------------------------------------------------------
// Images.

// Decodes a PNG into a bitmap of its own (GDI+ keeps reading the stream a Bitmap was made from).
std::unique_ptr<Gdiplus::Bitmap> Decode(const std::vector<u8> &png) {
	std::unique_ptr<Gdiplus::Bitmap> out;
	if (png.empty()) return out;
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, png.size());
	if (!mem) return out;
	memcpy(GlobalLock(mem), png.data(), png.size());
	GlobalUnlock(mem);
	IStream *stream = nullptr;
	if (CreateStreamOnHGlobal(mem, TRUE, &stream) != S_OK) {
		GlobalFree(mem);
		return out;
	}
	{
		Gdiplus::Bitmap img(stream);
		if (img.GetLastStatus() == Gdiplus::Ok && img.GetWidth() > 0 && img.GetHeight() > 0) {
			out = std::make_unique<Gdiplus::Bitmap>((INT)img.GetWidth(), (INT)img.GetHeight(), PixelFormat32bppPARGB);
			Gdiplus::Graphics g(out.get());
			g.DrawImage(&img, 0, 0, (INT)img.GetWidth(), (INT)img.GetHeight());
		}
	}
	stream->Release();
	return out;
}

void LoadImages(Game &g) {
	if (!g.iconImage && !g.icon.empty()) g.iconImage = Decode(g.icon);
	if (!g.pic1Loaded) {
		g.pic1Loaded = true;
		g.pic1 = Decode(ReadPic1(g.path));
	}
}

// The list icon: ICON0 with a tag when native code is built (green) or needs work (amber).
HBITMAP IconBitmap(const Game &g) {
	using namespace Gdiplus;
	HBITMAP out = nullptr;
	Bitmap canvas(kIconW, kIconH, PixelFormat32bppARGB);
	Graphics gfx(&canvas);
	gfx.Clear(Color(255, 32, 32, 40));
	if (std::unique_ptr<Bitmap> icon = Decode(g.icon)) gfx.DrawImage(icon.get(), 0, 0, kIconW, kIconH);
	if (g.native.state != Native::None) {
		const wchar_t *tag = g.native.state == Native::Built ? L"NATIVE" : g.native.state == Native::Stale ? L"REBUILD" : L"PARTIAL";
		Color color = g.native.state == Native::Built ? Color(235, 38, 150, 72) : Color(235, 196, 128, 24);
		gfx.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
		FontFamily family(L"Segoe UI");
		Font font(&family, 9, FontStyleBold, UnitPixel);
		RectF box;
		gfx.MeasureString(tag, -1, &font, PointF(0, 0), &box);
		RectF r(kIconW - box.Width - 8, kIconH - box.Height - 4, box.Width + 6, box.Height + 1);
		SolidBrush fill(color), text(Color(255, 255, 255, 255));
		gfx.FillRectangle(&fill, r);
		gfx.DrawString(tag, -1, &font, PointF(r.X + 3, r.Y + 0.5f), &text);
	}
	canvas.GetHBITMAP(Color(0, 0, 0, 0), &out);
	return out;
}

// ---------------------------------------------------------------------------------------------
// Main window.

int Selected() {
	int i = g_list ? ListView_GetNextItem(g_list, -1, LVNI_SELECTED) : -1;
	if (i < 0) return -1;
	LVITEMW item{};
	item.mask = LVIF_PARAM;
	item.iItem = i;
	ListView_GetItem(g_list, &item);
	return item.lParam >= 0 && (size_t)item.lParam < g_games.size() ? (int)item.lParam : -1;
}

fs::path SelectedPath() {
	int i = Selected();
	return i < 0 ? fs::path() : g_games[i].path;
}

int FindGame(const fs::path &p) {
	for (size_t i = 0; i < g_games.size(); i++)
		if (g_games[i].path == p) return (int)i;
	return -1;
}

void UpdateUi() {
	bool sel = Selected() >= 0;
	for (HWND b : g_tools) {
		int id = GetDlgCtrlID(b);
		if (id == ID_PLAY || id == ID_BUILD) EnableWindow(b, sel);
	}
	if (g_detail) InvalidateRect(g_detail, nullptr, FALSE);
}

// Refills the list, keeping `keep` selected (else the first game).
void Populate(const fs::path &keep) {
	SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(g_list);
	ImageList_RemoveAll(g_images);
	int select = g_games.empty() ? -1 : 0;
	for (size_t i = 0; i < g_games.size(); i++) {
		HBITMAP bmp = IconBitmap(g_games[i]);
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
		if (!keep.empty() && g_games[i].path == keep) select = (int)i;
	}
	if (select >= 0) {
		ListView_SetItemState(g_list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(g_list, select, FALSE);
	}
	SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(g_list, nullptr, TRUE);
	UpdateUi();
}

void Rescan() {
	fs::path keep = SelectedPath();
	Scan();
	RefreshNative();
	Populate(keep);
}

void ReloadSettings() {
	g_settings = settings::Settings();
	settings::Load(settings::DefaultPath(), g_settings);
}

fs::path FindUp(const fs::path &start, const fs::path &rel) {
	for (fs::path p = start; !p.empty(); p = p.parent_path()) {
		if (fs::exists(p / rel)) return p / rel;
		if (p == p.root_path()) break;
	}
	return {};
}

bool Launch(const std::wstring &exe, const std::wstring &args, bool console, HANDLE *process = nullptr) {
	std::wstring cmd = L"\"" + exe + L"\" " + args;
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	DWORD flags = console ? CREATE_NEW_CONSOLE : 0;
	if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, g_exeDir.wstring().c_str(), &si,
	                   &pi)) {
		CloseHandle(pi.hThread);
		if (process) *process = pi.hProcess;
		else CloseHandle(pi.hProcess);
		return true;
	}
	MessageBoxW(g_main, (L"Could not start:\n" + cmd).c_str(), L"PSPC", MB_ICONERROR);
	return false;
}

fs::path BuildScript() { return FindUp(g_exeDir, fs::path("tools") / "pspc_build.py"); }

// Runs tools\pspc_build.py for the game in a console window. With playAfter the game starts
// when it finishes (the window stays open only if the build fails).
void Build(int idx, bool playAfter) {
	if (idx < 0) return;
	const Game &g = g_games[idx];
	if (Building(g.path)) {
		for (BuildJob &j : g_jobs)
			if (j.game == g.path) j.playAfter |= playAfter;
		return;
	}
	fs::path script = BuildScript();
	if (script.empty()) {
		MessageBoxW(g_main, L"tools\\pspc_build.py not found.", L"PSPC", MB_ICONERROR);
		return;
	}
	wchar_t local[MAX_PATH] = {};
	GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
	fs::path py = fs::path(local) / "Programs" / "Python" / "Python312" / "python.exe";
	std::wstring exe = fs::exists(py) ? py.wstring() : L"python.exe";
	std::wstring args = L"-I \"" + script.wstring() + L"\" \"" + g.path.wstring() + L"\" --cache \"" + CacheDir().wstring() + L"\"";
	// In a build tree, build against this tree's tools and libraries (pspc_build.py's default is
	// build\RelWithDebInfo).
	fs::path tree = g_exeDir.parent_path();
	std::error_code ec;
	if (fs::exists(tree / "generated" / "pspc_sem_version.h", ec) && fs::exists(tree / "lib" / "pspc_sem.lib", ec))
		args += L" --build-dir \"" + tree.wstring() + L"\"";
	// Run through cmd so the window stays open with the result.
	std::wstring tail = playAfter ? L" || (pause & exit 1)" : L" & pause";
	HANDLE process = nullptr;
	if (!Launch(L"C:\\Windows\\System32\\cmd.exe", L"/c \"\"" + exe + L"\" " + args + tail + L"\"", true, &process)) return;
	g_jobs.push_back({process, g.path, playAfter});
	SetTimer(g_main, ID_TIMER_JOBS, 500, nullptr);
	UpdateUi();
}

void Play(int idx, bool interp, bool offerBuild = true) {
	if (idx < 0) return;
	const Game &g = g_games[idx];
	if (!interp && offerBuild && g_settings.aot && !BuildScript().empty()) {
		if (Building(g.path)) {
			int r = MessageBoxW(g_main,
			                    (L"The native code of \"" + g.title +
			                     L"\" is still being built.\n\nStart the game when the build is done?\n\n"
			                     L"No: play now (code that isn't built yet runs in the interpreter).")
			                        .c_str(),
			                    L"PSPC", MB_YESNOCANCEL | MB_ICONQUESTION);
			if (r == IDCANCEL) return;
			if (r == IDYES) {
				Build(idx, true);
				return;
			}
		} else if (g.native.state == Native::None || g.native.state == Native::Stale) {
			std::wstring what = g.native.state == Native::Stale
			                        ? L"The native code of \"" + g.title + L"\" is out of date (PSPC changed since it was built)."
			                        : L"\"" + g.title + L"\" has no native code yet.";
			int r = MessageBoxW(g_main,
			                    (what + L"\n\nBuild it now? That takes about a minute per disc; the game starts when "
			                            L"it is done.\n\nNo: play now in the interpreter (slower).")
			                        .c_str(),
			                    L"PSPC", MB_YESNOCANCEL | MB_ICONQUESTION);
			if (r == IDCANCEL) return;
			if (r == IDYES) {
				Build(idx, true);
				return;
			}
		}
	}
	std::wstring args = (interp ? L"--interp \"" : L"\"") + g.path.wstring() + L"\"";
	Launch((g_exeDir / "pspc.exe").wstring(), args, false);
}

// The firmware's XMB (pspc --vsh): needs tools\pspc_install_fw.py to have installed it.
bool XmbInstalled() {
	std::error_code ec;
	return fs::exists(g_exeDir / "flash0" / "vsh" / "module" / "vshmain.prx", ec);
}

void BootXmb() {
	if (!XmbInstalled()) {
		MessageBoxW(g_main,
		            L"The PSP firmware isn't installed.\n\nDecrypt Sony's 4.01 update with pspc-decrypt psar, then run "
		            L"tools\pspc_install_fw.py (see docs\vsh-boot.md).",
		            L"PSPC", MB_ICONINFORMATION);
		return;
	}
	Launch((g_exeDir / "pspc.exe").wstring(), L"--vsh", false);
}

// Polls the running builds; refreshes the status and starts games waiting for theirs.
void PollJobs() {
	for (size_t i = 0; i < g_jobs.size();) {
		if (WaitForSingleObject(g_jobs[i].process, 0) != WAIT_OBJECT_0) {
			i++;
			continue;
		}
		DWORD code = 1;
		GetExitCodeProcess(g_jobs[i].process, &code);
		CloseHandle(g_jobs[i].process);
		BuildJob job = g_jobs[i];
		g_jobs.erase(g_jobs.begin() + i);
		RefreshNative();
		Populate(SelectedPath());
		int idx = FindGame(job.game);
		if (!job.playAfter || idx < 0) continue;
		if (code == 0 ||
		    MessageBoxW(g_main,
		                (L"Building the native code of \"" + g_games[idx].title +
		                 L"\" reported errors (its window shows them).\n\nPlay anyway? Modules without native code "
		                 L"run in the interpreter.")
		                    .c_str(),
		                L"PSPC", MB_YESNO | MB_ICONWARNING) == IDYES)
			Play(idx, false, false);
	}
	if (g_jobs.empty()) KillTimer(g_main, ID_TIMER_JOBS);
	UpdateUi();
}

void OpenSettings() {
	if (ShowSettingsDialog(g_main, settings::DefaultPath())) {
		ReloadSettings();
		UpdateUi();
	}
	SetFocus(g_list);
}

// The panel for the selected game: PIC1 as the backdrop, ICON0, title, disc ID, native code status.
void PaintDetail(HDC hdc, int w, int h) {
	using namespace Gdiplus;
	if (w <= 0 || h <= 0) return;
	Bitmap buffer(w, h, PixelFormat32bppPARGB);
	Graphics g(&buffer);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	g.Clear(Color(255, 20, 20, 26));
	FontFamily family(L"Segoe UI");
	StringFormat wrap;
	wrap.SetTrimming(StringTrimmingEllipsisCharacter);
	const REAL pad = 22, tw = (REAL)w - 2 * pad;
	int sel = Selected();
	if (sel < 0) {
		Font font(&family, 15, FontStyleRegular, UnitPixel);
		SolidBrush gray(Color(255, 150, 150, 160));
		StringFormat center;
		center.SetAlignment(StringAlignmentCenter);
		center.SetLineAlignment(StringAlignmentCenter);
		std::wstring msg = g_games.empty() ? L"No games found.\n\nPut ISO/CSO images or EBOOT.PBP folders in\n" +
		                                         g_gamesDir.wstring() + L"\nand press F5."
		                                   : L"Select a game.";
		g.DrawString(msg.c_str(), -1, &font, RectF(pad, 0, tw, (REAL)h), &center, &gray);
	} else {
		Game &game = g_games[sel];
		LoadImages(game);
		if (game.pic1) {
			// Cover the panel, centred, keeping the aspect ratio.
			REAL iw = (REAL)game.pic1->GetWidth(), ih = (REAL)game.pic1->GetHeight();
			REAL scale = std::max((REAL)w / iw, (REAL)h / ih);
			g.DrawImage(game.pic1.get(), RectF(((REAL)w - iw * scale) / 2, ((REAL)h - ih * scale) / 2, iw * scale, ih * scale));
		}
		// Shade so the text reads on any picture: light at the top, nearly opaque at the bottom.
		LinearGradientBrush shade(Point(0, 0), Point(0, h + 1), Color(40, 20, 20, 26), Color(250, 20, 20, 26));
		Color stops[] = {Color(30, 20, 20, 26), Color(90, 20, 20, 26), Color(230, 20, 20, 26), Color(250, 20, 20, 26)};
		REAL at[] = {0.0f, 0.3f, 0.62f, 1.0f};
		shade.SetInterpolationColors(stops, at, 4);
		g.FillRectangle(&shade, 0, 0, w, h);

		Font titleFont(&family, 24, FontStyleBold, UnitPixel), infoFont(&family, 14, FontStyleRegular, UnitPixel),
		    smallFont(&family, 12, FontStyleRegular, UnitPixel);
		std::wstring sub = game.id.empty() ? game.path.filename().wstring() : game.id + L"  \u00B7  " + game.path.filename().wstring();
		std::wstring status;
		Color statusColor(255, 160, 160, 172);
		const NativeInfo &n = game.native;
		if (Building(game.path)) {
			status = L"Building native code...";
			statusColor = Color(255, 110, 175, 255);
		} else if (!g_settings.aot) {
			status = L"Native code is turned off in Settings: the game runs in the interpreter.";
		} else if (n.state == Native::Built) {
			status = L"Native code built" + (n.total > 1 ? L" (" + std::to_wstring(n.total) + L" modules)" : std::wstring());
			statusColor = Color(255, 92, 204, 120);
		} else if (n.state == Native::Partial) {
			status = L"Native code partly built (" + std::to_wstring(n.built) + L" of " + std::to_wstring(n.total) +
			         L" modules): build it again to finish.";
			statusColor = Color(255, 238, 178, 70);
		} else if (n.state == Native::Stale) {
			status = L"Native code out of date: PSPC changed since it was built. Play offers to rebuild it.";
			statusColor = Color(255, 238, 178, 70);
		} else {
			status = L"No native code yet: the game runs in the interpreter (slower). Play offers to build it.";
		}
		if (n.failed > 0 && g_settings.aot)
			status += L"\n" + std::to_wstring(n.failed) + (n.failed == 1 ? L" module" : L" modules") +
			          L" could not be built and run in the interpreter.";
		std::wstring where = game.path.wstring();
		std::wstring hint = L"Enter or double-click: play     Right-click: more     F5: refresh";

		struct Line {
			const std::wstring *text;
			Font *font;
			Color color;
			REAL gapAfter;
			REAL height = 0;
		};
		Line lines[] = {{&game.title, &titleFont, Color(255, 245, 245, 250), 4},
		                {&sub, &infoFont, Color(255, 185, 185, 196), 14},
		                {&status, &infoFont, statusColor, 12},
		                {&where, &smallFont, Color(255, 130, 130, 142), 18},
		                {&hint, &smallFont, Color(255, 110, 110, 122), 0}};
		REAL total = 0;
		for (Line &l : lines) {
			RectF box;
			g.MeasureString(l.text->c_str(), -1, l.font, RectF(0, 0, tw, 400), &wrap, &box);
			l.height = std::min(box.Height, (REAL)h / 3);
			total += l.height + l.gapAfter;
		}
		REAL iconH = game.iconImage ? std::min((REAL)kIconH, tw * kIconH / kIconW) : 0;
		REAL y = std::max(pad + (iconH ? iconH + 14 : 0), (REAL)h - pad - total);
		if (game.iconImage) {
			REAL iconW = iconH * kIconW / kIconH;
			g.DrawImage(game.iconImage.get(), RectF(pad, y - iconH - 14, iconW, iconH));
		}
		for (Line &l : lines) {
			SolidBrush brush(l.color);
			g.DrawString(l.text->c_str(), -1, l.font, RectF(pad, y, tw, l.height + 1), &wrap, &brush);
			y += l.height + l.gapAfter;
		}
	}
	Graphics screen(hdc);
	screen.DrawImage(&buffer, 0, 0);
}

LRESULT CALLBACK DetailProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		RECT r;
		GetClientRect(hwnd, &r);
		PaintDetail(hdc, r.right, r.bottom);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_LBUTTONDBLCLK: Play(Selected(), false); return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

void DrawToolButton(const DRAWITEMSTRUCT *di) {
	bool pressed = di->itemState & ODS_SELECTED, disabled = di->itemState & ODS_DISABLED;
	bool accent = di->CtlID == ID_PLAY;
	COLORREF bg = accent ? (pressed ? RGB(0, 90, 180) : RGB(0, 110, 215)) : (pressed ? RGB(70, 70, 86) : RGB(50, 50, 62));
	if (disabled) bg = RGB(42, 42, 52);
	HBRUSH bar = CreateSolidBrush(kToolbar), brush = CreateSolidBrush(bg);
	FillRect(di->hDC, &di->rcItem, bar);
	DeleteObject(bar);
	HPEN pen = CreatePen(PS_SOLID, 1, bg);
	HGDIOBJ oldBrush = SelectObject(di->hDC, brush), oldPen = SelectObject(di->hDC, pen);
	RoundRect(di->hDC, di->rcItem.left, di->rcItem.top, di->rcItem.right, di->rcItem.bottom, 7, 7);
	SelectObject(di->hDC, oldBrush);
	SelectObject(di->hDC, oldPen);
	DeleteObject(brush);
	DeleteObject(pen);
	wchar_t text[64];
	GetWindowTextW(di->hwndItem, text, 64);
	SetBkMode(di->hDC, TRANSPARENT);
	SetTextColor(di->hDC, disabled ? RGB(110, 110, 120) : RGB(240, 240, 245));
	HGDIOBJ oldFont = SelectObject(di->hDC, g_uiFont);
	RECT r = di->rcItem;
	DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	SelectObject(di->hDC, oldFont);
	if (di->itemState & ODS_FOCUS) {
		RECT f = di->rcItem;
		InflateRect(&f, -3, -3);
		DrawFocusRect(di->hDC, &f);
	}
}

void Layout(int w, int h) {
	HDC dc = GetDC(g_main);
	HGDIOBJ old = SelectObject(dc, g_uiFont);
	int x = 10;
	for (HWND b : g_tools) {
		wchar_t text[64];
		GetWindowTextW(b, text, 64);
		SIZE sz{};
		GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
		int bw = sz.cx + 30;
		MoveWindow(b, x, 7, bw, kToolbarH - 14, TRUE);
		x += bw + 8;
	}
	SelectObject(dc, old);
	ReleaseDC(g_main, dc);
	int detailW = std::clamp(w * 2 / 5, 300, 560);
	if (w - detailW < 200) detailW = std::max(0, w - 200);
	MoveWindow(g_list, 0, kToolbarH, w - detailW, std::max(0, h - kToolbarH), TRUE);
	MoveWindow(g_detail, w - detailW, kToolbarH, detailW, std::max(0, h - kToolbarH), TRUE);
}

void ContextMenu(HWND hwnd, LPARAM lp) {
	int sel = Selected();
	HMENU m = CreatePopupMenu();
	UINT on = sel >= 0 ? 0 : MF_GRAYED;
	AppendMenuW(m, on, ID_PLAY, L"&Play\tEnter");
	AppendMenuW(m, on, ID_PLAY_INTERP, L"Play (&interpreter only)");
	AppendMenuW(m, on, ID_BUILD, L"&Build native code (AOT)");
	AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(m, 0, ID_REFRESH, L"&Refresh\tF5");
	AppendMenuW(m, 0, ID_OPEN_FOLDER, L"&Open Games folder");
	AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(m, XmbInstalled() ? 0 : MF_GRAYED, ID_BOOT_XMB, L"Boot &XMB (PSP firmware)");
	AppendMenuW(m, 0, ID_SETTINGS, L"&Settings...");
	POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
	if (lp == -1) {  // from the keyboard: at the selected item
		RECT r{};
		if (sel >= 0 && ListView_GetItemRect(g_list, ListView_GetNextItem(g_list, -1, LVNI_SELECTED), &r, LVIR_ICON)) {
			pt = {r.left + 8, r.bottom};
			ClientToScreen(g_list, &pt);
		} else {
			GetCursorPos(&pt);
		}
	}
	TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
	DestroyMenu(m);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_CREATE: {
		g_main = hwnd;
		NONCLIENTMETRICSW ncm{sizeof(ncm)};
		SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
		ncm.lfMessageFont.lfHeight = -15;
		g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
		const std::pair<int, const wchar_t *> tools[] = {
			{ID_PLAY, L"Play"}, {ID_BUILD, L"Build native code"}, {ID_BOOT_XMB, L"Boot XMB"},
			{ID_SETTINGS, L"Settings"}, {ID_REFRESH, L"Refresh"}, {ID_OPEN_FOLDER, L"Games folder"}};
		for (const auto &t : tools)
			g_tools.push_back(CreateWindowExW(0, L"BUTTON", t.second, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0,
			                                  0, hwnd, (HMENU)(INT_PTR)t.first, nullptr, nullptr));
		g_list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_ICON | LVS_SINGLESEL | LVS_AUTOARRANGE | LVS_SHOWSELALWAYS,
		                         0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LIST, nullptr, nullptr);
		ListView_SetExtendedListViewStyle(g_list, LVS_EX_DOUBLEBUFFER);
		ListView_SetBkColor(g_list, kBackground);
		ListView_SetTextBkColor(g_list, kBackground);
		ListView_SetTextColor(g_list, kText);
		g_images = ImageList_Create(kIconW, kIconH, ILC_COLOR32, 16, 16);
		ListView_SetImageList(g_list, g_images, LVSIL_NORMAL);
		ListView_SetIconSpacing(g_list, kIconW + 24, kIconH + 48);
		g_detail = CreateWindowExW(0, L"PspcDetail", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
		ReloadSettings();
		Scan();
		RefreshNative();
		Populate({});
		SetFocus(g_list);
		return 0;
	}
	case WM_SIZE: Layout(LOWORD(lp), HIWORD(lp)); return 0;
	case WM_SETFOCUS: SetFocus(g_list); return 0;
	case WM_GETMINMAXINFO: ((MINMAXINFO *)lp)->ptMinTrackSize = {720, 420}; return 0;
	case WM_ERASEBKGND: {
		RECT r;
		GetClientRect(hwnd, &r);
		r.bottom = kToolbarH;
		HBRUSH b = CreateSolidBrush(kToolbar);
		FillRect((HDC)wp, &r, b);
		DeleteObject(b);
		return 1;
	}
	case WM_DRAWITEM: DrawToolButton((const DRAWITEMSTRUCT *)lp); return TRUE;
	case WM_ACTIVATEAPP:
		// Back from a game, a build or an editor: pick up new native code and settings.
		if (wp) {
			ReloadSettings();
			if (RefreshNative()) Populate(SelectedPath());
			else UpdateUi();
		}
		return 0;
	case WM_TIMER:
		if (wp == ID_TIMER_JOBS) PollJobs();
		return 0;
	case WM_NOTIFY: {
		auto *nm = (NMHDR *)lp;
		if (nm->idFrom != ID_LIST) return 0;
		if (nm->code == NM_DBLCLK && ((NMITEMACTIVATE *)lp)->iItem >= 0) Play(Selected(), false);
		if (nm->code == LVN_ITEMCHANGED && (((NMLISTVIEW *)lp)->uChanged & LVIF_STATE)) UpdateUi();
		if (nm->code == LVN_KEYDOWN) {
			WORD vk = ((NMLVKEYDOWN *)lp)->wVKey;
			if (vk == VK_RETURN) Play(Selected(), false);
			if (vk == VK_F5) Rescan();
		}
		return 0;
	}
	case WM_CONTEXTMENU: ContextMenu(hwnd, lp); return 0;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case ID_PLAY: Play(Selected(), false); break;
		case ID_PLAY_INTERP: Play(Selected(), true); break;
		case ID_BUILD: Build(Selected(), false); break;
		case ID_REFRESH: Rescan(); break;
		case ID_OPEN_FOLDER: ShellExecuteW(nullptr, L"open", g_gamesDir.wstring().c_str(), nullptr, nullptr, SW_SHOW); break;
		case ID_SETTINGS: OpenSettings(); break;
		case ID_BOOT_XMB: BootXmb(); break;
		}
		// Toolbar buttons hand the keyboard back to the list.
		if ((HWND)lp && GetFocus() == (HWND)lp) SetFocus(g_list);
		return 0;
	case WM_DESTROY:
		KillTimer(hwnd, ID_TIMER_JOBS);
		PostQuitMessage(0);
		return 0;
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
	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
	InitCommonControlsEx(&icc);

	WNDCLASSW dc{};
	dc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
	dc.lpfnWndProc = DetailProc;
	dc.hInstance = inst;
	dc.lpszClassName = L"PspcDetail";
	dc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassW(&dc);

	WNDCLASSW wc{};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.lpszClassName = L"PspcLauncher";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = CreateSolidBrush(kBackground);
	RegisterClassW(&wc);
	std::wstring title = L"PSPC - " + g_gamesDir.wstring();
	HWND hwnd = CreateWindowW(wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
	                          CW_USEDEFAULT, 1120, 680, nullptr, nullptr, inst, nullptr);
	ShowWindow(hwnd, show);
	MSG m;
	while (GetMessageW(&m, nullptr, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	g_games.clear();  // GDI+ bitmaps, before GDI+ goes away
	Gdiplus::GdiplusShutdown(token);
	return 0;
}
