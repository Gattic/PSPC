// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc-launcher: a game library window for the Games folder.
//   * lists UMD images (ISO/CSO/ZSO/DAX), EBOOT.PBP folders and PBP/PRX/ELF files,
//     with ICON0, the PARAM.SFO title and whether the game's native (AOT) code is built;
//   * a sidebar (the library, Boot XMB, Plugins, Settings, Online; the PSPC Network account at
//     the bottom), a header with search, the games as cards, and the selected game on the right
//     over its PIC1 backdrop with its actions (ui_theme.* has the look);
//   * the Online page (online_page.*): PSPC Network sign-in, and the servers this PC can run for
//     its player and their friends (servers.*);
//   * Enter/double-click plays the game with pspc.exe, offering to build its native code first;
//     the panel and the context menu also build native code (tools/pspc_build.py) and play in the
//     interpreter;
//   * "Boot XMB" starts the firmware's XMB (pspc --vsh), and offers to install the firmware from
//     Sony's 4.01 update (pspc-decrypt psar, then tools/pspc_install_fw.py) when it isn't yet.
//   * Native code builds need PSPC's developer tools (tools/pspc_build.py, a compiler and the build
//     tree); without them (an installed PSPC) games run in the interpreter and the build actions
//     don't show.
//
//   pspc-launcher [games dir] [--list]   (--list prints the library to the console and exits)
//   pspc-launcher --write-icon <file.ico>  (the app icon, from the sidebar's logo: src/frontend/pspc.ico)
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
#include <shobjidl.h>
#include <uxtheme.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/fileutil.h"
#include "common/settings.h"
#include "frontend/network_dialog.h"
#include "frontend/online_page.h"
#include "frontend/plugins_dialog.h"
#include "frontend/server_dialogs.h"
#include "frontend/servers.h"
#include "frontend/settings_dialog.h"
#include "frontend/ui_theme.h"
#include "fs/vfs.h"
#include "pspc_sem_version.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
// Common controls 6 (themed controls; the Settings dialog's list and trackbar).
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

enum : int {
	ID_LIST = 100,
	ID_SEARCH,
	ID_PLAY = 200,
	ID_PLAY_INTERP,
	ID_BUILD,
	ID_REFRESH,
	ID_OPEN_FOLDER,
	ID_SETTINGS,
	ID_BOOT_XMB,
	ID_PLUGINS,
	ID_NETWORK,
	ID_LIBRARY,
	ID_SHOW_FILE,
	ID_ONLINE,
	ID_CHOOSE_GAMES,
	ID_TIMER_JOBS = 300,
};

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
	bool firmware = false;  // installing the PSP firmware, not building a game
};

HWND g_main = nullptr, g_list = nullptr, g_detail = nullptr;
HIMAGELIST g_images = nullptr;
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

// ---------------------------------------------------------------------------------------------
// Main window: a sidebar (sections and the account), a header (title, search), the game grid
// (a ListView drawn as cards) and the selected game's panel on the right.

// Layout, in pixels at 96 DPI (scaled with D()).
constexpr int kSidebarW = 220, kHeaderH = 76, kCardW = 164, kCardImageH = 91, kCardTextH = 64, kCardGap = 16,
              kGridPad = 24;

int g_dpi = 96;
HWND g_search = nullptr, g_account = nullptr, g_refreshButton = nullptr, g_folderButton = nullptr;
HWND g_playButton = nullptr, g_buildButton = nullptr, g_interpButton = nullptr, g_showButton = nullptr,
     g_emptyButton = nullptr, g_chooseButton = nullptr;
HWND g_online = nullptr;  // the Online page
bool g_onlineShown = false;
std::vector<HWND> g_nav;
int g_hot = -1;  // the card under the mouse
std::wstring g_filter;
RECT g_searchBox{};

int D(int v) { return ui::Px(v, g_dpi); }
float Df(float v) { return ui::Pxf(v, g_dpi); }

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

std::wstring Lower(std::wstring s) {
	CharLowerBuffW(s.data(), (DWORD)s.size());
	return s;
}

bool Matches(const Game &g) {
	if (g_filter.empty()) return true;
	std::wstring f = Lower(g_filter);
	return Lower(g.title).find(f) != std::wstring::npos || Lower(g.id).find(f) != std::wstring::npos ||
	       Lower(g.path.filename().wstring()).find(f) != std::wstring::npos;
}

std::wstring SizeText(u64 bytes) {
	wchar_t buf[32];
	if (bytes >= (1ull << 30)) swprintf_s(buf, L"%.1f GB", bytes / 1073741824.0);
	else swprintf_s(buf, L"%.0f MB", bytes / 1048576.0 + 0.5);
	return buf;
}

std::wstring FormatText(const Game &g) {
	std::wstring ext = g.path.extension().wstring();
	for (wchar_t &c : ext) c = (wchar_t)towupper(c);
	if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
	if (g.path.filename() == "EBOOT.PBP") ext = L"EBOOT";
	return ext;
}

// The account line at the bottom of the sidebar.
void UpdateAccount() {
	if (!g_account) return;
	std::wstring id = Widen(g_settings.npOnlineId);
	std::wstring detail = g_settings.npServer.empty() ? L"Local sign-in" : g_settings.npToken.empty()
	                                                                           ? L"PSPC Network: not signed in"
	                                                                           : L"PSPC Network";
	SetWindowTextW(g_account, id.empty() ? L"Network" : id.c_str());
	ui::SetDetail(g_account, detail.c_str());
}

void UpdateUi();
void Populate(const fs::path &keep);
void Rescan();
void ReloadSettings();

// ---- actions

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

// Whether this copy of PSPC can build native code (a source tree with its tools).
bool CanBuild() { return !BuildScript().empty(); }

void Play(int idx, bool interp, bool offerBuild = true) {
	if (idx < 0) return;
	const Game &g = g_games[idx];
	if (!interp && offerBuild && g_settings.aot && CanBuild()) {
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

// A file chosen with the Windows file dialog ("" if cancelled). filter: pairs of name, pattern.
fs::path PickFile(const wchar_t *title, const std::vector<COMDLG_FILTERSPEC> &filter, bool folder) {
	fs::path out;
	IFileOpenDialog *dlg = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
	DWORD opts = 0;
	dlg->GetOptions(&opts);
	dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
	dlg->SetTitle(title);
	if (!filter.empty()) dlg->SetFileTypes((UINT)filter.size(), filter.data());
	if (SUCCEEDED(dlg->Show(g_main))) {
		IShellItem *item = nullptr;
		if (SUCCEEDED(dlg->GetResult(&item))) {
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
				out = path;
				CoTaskMemFree(path);
			}
			item->Release();
		}
	}
	dlg->Release();
	return out;
}

// Installs the PSP firmware from Sony's 4.01 update (its EBOOT.PBP): decrypted with pspc-decrypt into a
// temporary folder, then tools\pspc_install_fw.py copies what the XMB needs next to pspc.exe (and builds
// its native code when this copy of PSPC can). Runs in a console window, which shows the progress.
void InstallFirmware() {
	fs::path script = FindUp(g_exeDir, fs::path("tools") / "pspc_install_fw.py");
	std::wstring python = servers::PythonCommand();
	std::error_code ec;
	if (script.empty() || python.empty() || !fs::exists(g_exeDir / "pspc-decrypt.exe", ec)) {
		MessageBoxW(g_main,
		            L"This copy of PSPC can't install the firmware: it needs pspc-decrypt.exe, tools\\pspc_install_fw.py and "
		            L"Python (an installed PSPC has them all).",
		            L"PSPC", MB_ICONWARNING);
		return;
	}
	fs::path pbp = PickFile(L"Sony's PSP system software update 4.01 (EBOOT.PBP)", {{L"PSP update (EBOOT.PBP)", L"*.PBP"}, {L"All files", L"*.*"}},
	                        false);
	if (pbp.empty()) return;
	wchar_t temp[MAX_PATH] = {};
	GetTempPathW(MAX_PATH, temp);
	fs::path work = fs::path(temp) / ("pspc_firmware_" + std::to_string(GetCurrentProcessId()));
	std::wstring args = L"--fw \"" + work.wstring() + L"\" --pspc-dir \"" + g_exeDir.wstring() + L"\"";
	fs::path tree = g_exeDir.parent_path();
	if (CanBuild() && fs::exists(tree / "lib" / "pspc_sem.lib", ec)) args += L" --build-dir \"" + tree.wstring() + L"\" --aot-core";
	else args += L" --no-aot";
	std::wstring steps = L"title Installing the PSP firmware for PSPC && echo Decrypting the update... && \"" + (g_exeDir / "pspc-decrypt.exe").wstring() +
	                     L"\" psar \"" + pbp.wstring() + L"\" \"" + work.wstring() + L"\" --model 02g && echo. && echo Installing... && " + python +
	                     L" -I -B -X utf8 \"" + script.wstring() + L"\" " + args;
	std::wstring tail = L" & set \"rc=!errorlevel!\" & rmdir /s /q \"" + work.wstring() + L"\" 2>nul & if !rc! neq 0 (echo. & echo The install failed. & pause) & exit !rc!";
	HANDLE process = nullptr;
	if (!Launch(L"C:\\Windows\\System32\\cmd.exe", L"/v:on /c \"" + steps + tail + L"\"", true, &process)) return;
	g_jobs.push_back({process, fs::path(), false, true});
	SetTimer(g_main, ID_TIMER_JOBS, 500, nullptr);
	UpdateUi();
}

void BootXmb() {
	if (!XmbInstalled()) {
		for (const BuildJob &j : g_jobs)
			if (j.firmware) {
				MessageBoxW(g_main, L"The PSP firmware is being installed (see its window).", L"PSPC", MB_ICONINFORMATION);
				return;
			}
		int r = MessageBoxW(g_main,
		                    L"The PSP's own menu (the XMB) needs Sony's PSP firmware, which PSPC can't come with. Games play "
		                    L"without it, though the ones that use the PSP's system fonts show their text only with it.\n\n"
		                    L"If you have Sony's PSP system software update 4.01 (its EBOOT.PBP), PSPC can install the "
		                    L"firmware from it now.\n\nChoose the update file?",
		                    L"PSPC", MB_YESNO | MB_ICONINFORMATION);
		if (r == IDYES) InstallFirmware();
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
		if (job.firmware) {
			UpdateUi();
			if (code == 0 && XmbInstalled())
				MessageBoxW(g_main, L"The PSP firmware is installed. Boot XMB starts the PSP's menu.", L"PSPC", MB_ICONINFORMATION);
			continue;
		}
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

// The M33 plugins folder pspc lays over ms0: (pspc.ini plugins_dir, else the nearest "plugins"
// folder above the launcher, as pspc.exe finds it, else one next to it).
fs::path PluginsDir() {
	if (!g_settings.pluginsDir.empty()) return fs::u8path(g_settings.pluginsDir);
	fs::path found = FindUp(g_exeDir, fs::path("plugins") / "seplugins");
	if (!found.empty()) return found.parent_path();
	found = FindUp(g_exeDir, "plugins");
	return found.empty() ? g_exeDir / "plugins" : found;
}

void OpenPlugins() {
	if (ShowPluginsDialog(g_main, settings::DefaultPath(), PluginsDir())) ReloadSettings();
	SetFocus(g_list);
}

void OpenNetwork() {
	if (ShowNetworkDialog(g_main, settings::DefaultPath())) ReloadSettings();
	online::Refresh();
	UpdateUi();
	if (!g_onlineShown) SetFocus(g_list);
}

// The library or the Online page.
void ShowPage(bool online) {
	g_onlineShown = online;
	for (HWND h : {g_list, g_detail, g_search, g_refreshButton, g_folderButton}) ShowWindow(h, online ? SW_HIDE : SW_SHOW);
	ShowWindow(g_online, online ? SW_SHOW : SW_HIDE);
	for (HWND b : g_nav) {
		int id = GetDlgCtrlID(b);
		if (id == ID_LIBRARY || id == ID_ONLINE) {
			bool active = (id == ID_ONLINE) == online;
			ui::SetLook(b, active ? ui::Look::NavActive : ui::Look::Nav, id == ID_ONLINE ? ui::kIconGlobe : ui::kIconGames);
		}
	}
	SetFocus(online ? g_online : g_list);
	InvalidateRect(g_main, nullptr, FALSE);
}

// Where the games are: pspc.ini games_dir (pspc.exe lists the same folder in the XMB), chosen here.
void ChooseGamesFolder() {
	fs::path dir = PickFile(L"The folder with your PSP games (ISO, CSO, EBOOT.PBP)", {}, true);
	if (dir.empty()) return;
	settings::Settings s;
	settings::Load(settings::DefaultPath(), s);
	s.gamesDir = PathToUtf8(dir);
	if (!settings::Save(settings::DefaultPath(), s)) {
		MessageBoxW(g_main, (L"Couldn't write " + settings::DefaultPath().wstring()).c_str(), L"PSPC", MB_ICONERROR);
		return;
	}
	ReloadSettings();
	g_gamesDir = dir;
	Rescan();
}

// ---- the selected game's panel

struct DetailRects {
	Gdiplus::RectF hero, icon, title, chips, status, play, build, interp, path, show;
	std::wstring statusText, statusNote;
	COLORREF statusColor = ui::kTextDim;
	std::wstring buildLabel;
	bool buildEnabled = true;
};

void Status(const Game &game, DetailRects &d) {
	const NativeInfo &n = game.native;
	d.buildLabel = L"Build native code";
	if (Building(game.path)) {
		d.statusText = L"Building native code…";
		d.statusNote = L"The game starts on its own if you pressed Play.";
		d.statusColor = ui::kInfo;
		d.buildLabel = L"Building…";
		d.buildEnabled = false;
	} else if (!g_settings.aot) {
		d.statusText = L"Interpreter only";
		d.statusNote = L"Native code is turned off in Settings.";
	} else if (!CanBuild() && n.state != Native::Built) {
		d.statusText = L"Ready to play";
		d.statusNote = L"Runs in PSPC's interpreter.";
		d.statusColor = ui::kGreen;
	} else if (n.state == Native::Built) {
		d.statusText = L"Native code built";
		d.statusNote = n.total > 1 ? std::to_wstring(n.total) + L" modules run as Windows code." : L"Runs as Windows code.";
		d.statusColor = ui::kGreen;
		d.buildLabel = L"Rebuild";
	} else if (n.state == Native::Partial) {
		d.statusText = L"Native code partly built";
		d.statusNote = std::to_wstring(n.built) + L" of " + std::to_wstring(n.total) + L" modules. Build again to finish.";
		d.statusColor = ui::kAmber;
		d.buildLabel = L"Finish build";
	} else if (n.state == Native::Stale) {
		d.statusText = L"Native code out of date";
		d.statusNote = L"PSPC changed since it was built. Play offers to rebuild it.";
		d.statusColor = ui::kAmber;
		d.buildLabel = L"Rebuild";
	} else {
		d.statusText = L"Not built yet";
		d.statusNote = L"Runs in the interpreter (slower). Play offers to build it, about a minute.";
	}
	if (n.failed > 0 && g_settings.aot)
		d.statusNote += L" " + std::to_wstring(n.failed) + (n.failed == 1 ? L" module" : L" modules") +
		                L" couldn't be built and run in the interpreter.";
}

// Where everything in the panel goes for the selected game (also places the panel's buttons).
DetailRects ComputeDetail(Gdiplus::Graphics &g, int w, int h, const Game &game) {
	using Gdiplus::RectF;
	DetailRects d;
	Status(game, d);
	const float pad = Df(24), tw = (float)w - 2 * pad;
	float heroH = std::min((float)w * 272.0f / 480.0f, (float)h * 0.42f);
	d.hero = RectF(0, 0, (float)w, heroH);
	float iconW = std::min(Df(144), tw * 0.5f), iconH = iconW * 80.0f / 144.0f;
	d.icon = RectF(pad, heroH - iconH * 0.55f, iconW, iconH);
	float y = d.icon.GetBottom() + Df(16);
	float titleH = std::min(ui::Measure(g, game.title.c_str(), Df(22), true, tw).Height, Df(60));
	d.title = RectF(pad, y, tw, titleH);
	y += titleH + Df(10);
	d.chips = RectF(pad, y, tw, Df(24));
	y += Df(24) + Df(18);
	float statusH = Df(20) + ui::Measure(g, d.statusNote.c_str(), Df(12.5f), false, tw - Df(18)).Height;
	d.status = RectF(pad, y, tw, statusH);
	y += statusH + Df(20);
	d.play = RectF(pad, y, tw, Df(44));
	y += Df(44) + Df(10);
	float half = (tw - Df(10)) / 2;
	d.build = RectF(pad, y, half, Df(36));
	d.interp = RectF(pad + half + Df(10), y, half, Df(36));
	float pathY = std::max(y + Df(36) + Df(20), (float)h - pad - Df(30));
	d.show = RectF((float)w - pad - Df(32), pathY, Df(32), Df(30));
	d.path = RectF(pad, pathY, tw - Df(40), Df(30));
	return d;
}

void PlaceButton(HWND b, const Gdiplus::RectF &r, bool show = true) {
	if (!show) {
		ShowWindow(b, SW_HIDE);
		return;
	}
	SetWindowPos(b, nullptr, (int)r.X, (int)r.Y, (int)r.Width, (int)r.Height, SWP_NOZORDER | SWP_NOACTIVATE);
	ShowWindow(b, SW_SHOWNA);
}

void PlaceDetailButtons() {
	if (!g_detail) return;
	RECT rc;
	GetClientRect(g_detail, &rc);
	int sel = Selected();
	bool has = sel >= 0 && rc.right > 0;
	if (has) {
		Gdiplus::Bitmap probe(1, 1);
		Gdiplus::Graphics g(&probe);
		DetailRects d = ComputeDetail(g, rc.right, rc.bottom, g_games[sel]);
		SetWindowTextW(g_buildButton, d.buildLabel.c_str());
		EnableWindow(g_buildButton, d.buildEnabled);
		PlaceButton(g_playButton, d.play);
		if (CanBuild()) {
			PlaceButton(g_buildButton, d.build);
			PlaceButton(g_interpButton, d.interp);
		} else {
			// The interpreter is all this copy has: Play does that; the second button shows the file.
			ShowWindow(g_buildButton, SW_HIDE);
			ShowWindow(g_interpButton, SW_HIDE);
		}
		PlaceButton(g_showButton, d.show);
	} else {
		for (HWND b : {g_playButton, g_buildButton, g_interpButton, g_showButton}) ShowWindow(b, SW_HIDE);
	}
	bool empty = g_games.empty() && rc.right > 0;
	if (empty) {
		float bw = Df(220);
		PlaceButton(g_emptyButton, Gdiplus::RectF(((float)rc.right - bw) / 2, (float)rc.bottom / 2 + Df(90), bw, Df(40)));
		PlaceButton(g_chooseButton, Gdiplus::RectF(((float)rc.right - bw) / 2, (float)rc.bottom / 2 + Df(138), bw, Df(36)));
	} else {
		ShowWindow(g_emptyButton, SW_HIDE);
		ShowWindow(g_chooseButton, SW_HIDE);
	}
}

void Chip(Gdiplus::Graphics &g, float &x, float y, float h, const std::wstring &text) {
	if (text.empty()) return;
	float px = Df(12);
	float w = ui::Measure(g, text.c_str(), px).Width + Df(18);
	ui::FillRound(g, Gdiplus::RectF(x, y, w, h), h / 2, ui::C(ui::kSurfaceHot));
	ui::Text(g, text.c_str(), Gdiplus::RectF(x, y, w, h), px, ui::C(ui::kTextDim), false, 1);
	x += w + Df(8);
}

void PaintDetail(HDC hdc, int w, int h) {
	using namespace Gdiplus;
	if (w <= 0 || h <= 0) return;
	Bitmap buffer(w, h, PixelFormat32bppPARGB);
	Graphics g(&buffer);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	g.Clear(ui::C(ui::kSidebar));
	int sel = Selected();
	if (sel < 0) {
		float cy = (float)h / 2;
		if (g_games.empty()) {
			ui::Text(g, ui::kIconGames, RectF(0, cy - Df(110), (float)w, Df(60)), Df(44), ui::C(ui::kTextFaint), false, 1,
			         true, false, true);
			ui::Text(g, L"Your library is empty", RectF(0, cy - Df(40), (float)w, Df(28)), Df(18), ui::C(ui::kText), true, 1);
			std::wstring msg = L"Put your ISO, CSO or EBOOT.PBP games in\n" + g_gamesDir.wstring() +
			                   L"\nand they show up here, or choose the folder where they already are.";
			ui::Text(g, msg.c_str(), RectF(Df(24), cy - Df(6), (float)w - Df(48), Df(84)), Df(12.5f), ui::C(ui::kTextDim),
			         false, 1, false, true);
		} else {
			ui::Text(g, L"Select a game", RectF(0, 0, (float)w, (float)h), Df(15), ui::C(ui::kTextFaint), false, 1);
		}
	} else {
		Game &game = g_games[sel];
		LoadImages(game);
		DetailRects d = ComputeDetail(g, w, h, game);
		if (game.pic1) {
			REAL iw = (REAL)game.pic1->GetWidth(), ih = (REAL)game.pic1->GetHeight();
			REAL scale = std::max(d.hero.Width / iw, d.hero.Height / ih);
			g.SetClip(d.hero);
			g.DrawImage(game.pic1.get(), RectF((d.hero.Width - iw * scale) / 2, (d.hero.Height - ih * scale) / 2, iw * scale, ih * scale));
			g.ResetClip();
		} else if (game.iconImage) {
			// No backdrop: the icon, blown up and dimmed.
			g.SetClip(d.hero);
			REAL ih = d.hero.Width * 80.0f / 144.0f;
			g.DrawImage(game.iconImage.get(), RectF(0, (d.hero.Height - ih) / 2, d.hero.Width, ih));
			g.ResetClip();
			SolidBrush dim(ui::C(ui::kSidebar, 170));
			g.FillRectangle(&dim, d.hero);
		}
		// Fade the backdrop into the panel.
		LinearGradientBrush fade(PointF(0, 0), PointF(0, d.hero.Height + 1), ui::C(ui::kSidebar, 0), ui::C(ui::kSidebar, 255));
		Color stops[] = {ui::C(ui::kSidebar, 20), ui::C(ui::kSidebar, 60), ui::C(ui::kSidebar, 200), ui::C(ui::kSidebar, 255)};
		REAL at[] = {0.0f, 0.45f, 0.82f, 1.0f};
		fade.SetInterpolationColors(stops, at, 4);
		g.FillRectangle(&fade, d.hero);

		if (game.iconImage) {
			float r = Df(8);
			ui::FillRound(g, RectF(d.icon.X + Df(1), d.icon.Y + Df(4), d.icon.Width, d.icon.Height), r, Color(110, 0, 0, 0));
			GraphicsPath clip;
			ui::AddRoundRect(clip, d.icon, r);
			g.SetClip(&clip);
			g.DrawImage(game.iconImage.get(), d.icon);
			g.ResetClip();
			ui::StrokeRound(g, d.icon, r, Color(70, 255, 255, 255), 1);
		}
		ui::Text(g, game.title.c_str(), d.title, Df(22), ui::C(ui::kText), true, 0, false, true);
		float x = d.chips.X;
		Chip(g, x, d.chips.Y, d.chips.Height, game.id);
		Chip(g, x, d.chips.Y, d.chips.Height, FormatText(game) + (game.size ? L"  ·  " + SizeText(game.size) : L""));
		float dot = Df(8);
		SolidBrush dotBrush(ui::C(d.statusColor));
		g.FillEllipse(&dotBrush, RectF(d.status.X, d.status.Y + Df(6), dot, dot));
		ui::Text(g, d.statusText.c_str(), RectF(d.status.X + Df(16), d.status.Y, d.status.Width - Df(16), Df(20)), Df(14),
		         ui::C(d.statusColor == ui::kTextDim ? ui::kText : d.statusColor), true, 0, true);
		ui::Text(g, d.statusNote.c_str(), RectF(d.status.X + Df(16), d.status.Y + Df(22), d.status.Width - Df(18), d.status.Height),
		         Df(12.5f), ui::C(ui::kTextDim), false, 0, false, true);
		Pen line(ui::C(ui::kBorder), 1);
		g.DrawLine(&line, d.path.X, d.path.Y - Df(10), (float)w - d.path.X, d.path.Y - Df(10));
		ui::Text(g, game.path.wstring().c_str(), d.path, Df(11.5f), ui::C(ui::kTextFaint), false, 0, true);
	}
	Pen divider(ui::C(RGB(10, 10, 14)), 1);
	g.DrawLine(&divider, 0.0f, 0.0f, 0.0f, (float)h);
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
	case WM_SIZE: PlaceDetailButtons(); return 0;
	case WM_LBUTTONDBLCLK: Play(Selected(), false); return 0;
	case WM_DRAWITEM: ui::DrawOwnerButton((const DRAWITEMSTRUCT *)lp, ui::kSidebar); return TRUE;
	case WM_COMMAND: return SendMessageW(g_main, msg, wp, lp);  // the panel's buttons
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- the game grid

// A card: ICON0 on top, the title and disc ID below, a tag when native code is built or needs work.
void DrawCard(HDC hdc, const RECT &rc, Game &game, bool selected, bool hot, bool focused) {
	using namespace Gdiplus;
	Graphics g(hdc);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	RectF card((REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));
	float r = Df(10), imageH = (float)D(kCardImageH);
	ui::FillRound(g, card, r, ui::C(selected || hot ? ui::kSurfaceHot : ui::kSurface));
	GraphicsPath clip;
	ui::AddRoundRect(clip, card, r);
	g.SetClip(&clip);
	if (!game.iconImage && !game.icon.empty()) game.iconImage = Decode(game.icon);
	RectF image(card.X, card.Y, card.Width, imageH);
	if (game.iconImage) {
		g.DrawImage(game.iconImage.get(), image);
	} else {
		SolidBrush fill(ui::C(ui::kSurfaceDown));
		g.FillRectangle(&fill, image);
		ui::Text(g, ui::kIconGames, image, Df(30), ui::C(ui::kTextFaint), false, 1, true, false, true);
	}
	if (hot && !selected) {
		SolidBrush lift(Color(18, 255, 255, 255));
		g.FillRectangle(&lift, image);
	}
	g.ResetClip();

	// Native code tag.
	const wchar_t *tag = nullptr;
	COLORREF tagColor = ui::kGreen;
	if (Building(game.path)) tag = L"BUILDING", tagColor = ui::kInfo;
	else if (g_settings.aot && game.native.state == Native::Built) tag = L"NATIVE";
	else if (g_settings.aot && game.native.state == Native::Stale) tag = L"REBUILD", tagColor = ui::kAmber;
	else if (g_settings.aot && game.native.state == Native::Partial) tag = L"PARTIAL", tagColor = ui::kAmber;
	if (tag) {
		float px = Df(9.5f), th = Df(18);
		float tw = ui::Measure(g, tag, px, true).Width + Df(20);
		RectF pill(image.GetRight() - tw - Df(8), image.GetBottom() - th - Df(8), tw, th);
		ui::FillRound(g, pill, th / 2, Color(215, 12, 12, 16));
		SolidBrush dot(ui::C(tagColor));
		g.FillEllipse(&dot, RectF(pill.X + Df(7), pill.Y + th / 2 - Df(3), Df(6), Df(6)));
		ui::Text(g, tag, RectF(pill.X + Df(15), pill.Y, tw - Df(17), th), px, ui::C(ui::kText), true, 0);
	}

	float pad = Df(10);
	RectF title(card.X + pad, image.GetBottom() + Df(8), card.Width - 2 * pad, Df(34));
	ui::Text(g, game.title.c_str(), title, Df(12.5f), ui::C(ui::kText), true, 0, false, true);
	if (!game.id.empty())
		ui::Text(g, game.id.c_str(), RectF(card.X + pad, card.GetBottom() - Df(20), card.Width - 2 * pad, Df(15)), Df(11),
		         ui::C(ui::kTextFaint), false, 0, true);
	if (selected)
		ui::StrokeRound(g, card, r, focused ? ui::C(ui::kAccent) : ui::C(ui::kAccent, 140), Df(2));
}

LRESULT CustomDrawList(NMLVCUSTOMDRAW *cd) {
	if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
	if (cd->nmcd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
	int item = (int)cd->nmcd.dwItemSpec;
	LVITEMW lvi{};
	lvi.mask = LVIF_PARAM | LVIF_STATE;
	lvi.iItem = item;
	lvi.stateMask = LVIS_SELECTED;
	ListView_GetItem(g_list, &lvi);
	if (lvi.lParam < 0 || (size_t)lvi.lParam >= g_games.size()) return CDRF_SKIPDEFAULT;
	RECT cell;
	ListView_GetItemRect(g_list, item, &cell, LVIR_ICON);
	// The icon rect spans the grid cell; the card sits in its middle, the rest is the gutter.
	int cw = D(kCardW), ch = D(kCardImageH + kCardTextH);
	RECT rc{cell.left + (cell.right - cell.left - cw) / 2, cell.top, 0, 0};
	rc.right = rc.left + cw;
	rc.bottom = rc.top + ch;
	DrawCard(cd->nmcd.hdc, rc, g_games[lvi.lParam], (lvi.state & LVIS_SELECTED) != 0, item == g_hot, GetFocus() == g_list);
	return CDRF_SKIPDEFAULT;
}

void InvalidateItem(int item) {
	RECT r;
	if (item >= 0 && ListView_GetItemRect(g_list, item, &r, LVIR_BOUNDS)) InvalidateRect(g_list, &r, FALSE);
}

LRESULT CALLBACK ListSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
	switch (msg) {
	case WM_MOUSEMOVE: {
		LVHITTESTINFO hit{};
		hit.pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
		int item = ListView_HitTest(hwnd, &hit);
		if (item != g_hot) {
			InvalidateItem(g_hot);
			g_hot = item;
			InvalidateItem(g_hot);
			TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
			TrackMouseEvent(&t);
		}
		break;
	}
	case WM_MOUSELEAVE:
		InvalidateItem(g_hot);
		g_hot = -1;
		break;
	case WM_SETFOCUS:
	case WM_KILLFOCUS: InvalidateItem(ListView_GetNextItem(hwnd, -1, LVNI_SELECTED)); break;
	}
	return DefSubclassProc(hwnd, msg, wp, lp);
}

// The search box: Down moves to the games (Enter and Escape arrive as IDOK / IDCANCEL); its frame
// lights up while it has the focus.
LRESULT CALLBACK SearchSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
	if (msg == WM_KEYDOWN && wp == VK_DOWN) {
		SetFocus(g_list);
		return 0;
	}
	if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(g_main, &g_searchBox, FALSE);
	return DefSubclassProc(hwnd, msg, wp, lp);
}

void ListMetrics() {
	HIMAGELIST old = g_images;
	g_images = ImageList_Create(D(kCardW), D(kCardImageH + kCardTextH), ILC_COLOR32, 1, 1);
	ListView_SetImageList(g_list, g_images, LVSIL_NORMAL);
	if (old) ImageList_Destroy(old);
	ListView_SetIconSpacing(g_list, D(kCardW + kCardGap), D(kCardImageH + kCardTextH + kCardGap));
}

void UpdateUi() {
	bool sel = Selected() >= 0;
	EnableWindow(g_playButton, sel);
	if (g_detail) {
		PlaceDetailButtons();
		InvalidateRect(g_detail, nullptr, FALSE);
	}
	for (HWND b : g_nav) {
		if (GetDlgCtrlID(b) == ID_BOOT_XMB) ui::SetDetail(b, XmbInstalled() ? L"PSP firmware menu" : L"Firmware not installed");
		if (GetDlgCtrlID(b) == ID_ONLINE) ui::SetDetail(b, online::Summary().c_str());
	}
	UpdateAccount();
	InvalidateRect(g_main, nullptr, FALSE);
}

// Refills the list with the games matching the search, keeping `keep` selected (else the first).
void Populate(const fs::path &keep) {
	SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(g_list);
	g_hot = -1;
	int select = -1, row = 0;
	for (size_t i = 0; i < g_games.size(); i++) {
		if (!Matches(g_games[i])) continue;
		// The text is for keyboard search and screen readers; the cards draw their own.
		std::wstring label = g_games[i].title;
		LVITEMW item{};
		item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
		item.iItem = row;
		item.pszText = label.data();
		item.iImage = 0;
		item.lParam = (LPARAM)i;
		ListView_InsertItem(g_list, &item);
		if (select < 0 || (!keep.empty() && g_games[i].path == keep)) select = row;
		row++;
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

// ---- painting the window itself: sidebar, header

void PaintMain(HDC hdc, int w, int h) {
	using namespace Gdiplus;
	int sw = D(kSidebarW);
	Bitmap buffer(w, h, PixelFormat32bppPARGB);
	Graphics g(&buffer);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	g.Clear(ui::C(ui::kBg));
	SolidBrush side(ui::C(ui::kSidebar));
	g.FillRectangle(&side, 0, 0, sw, h);
	Pen divider(ui::C(RGB(10, 10, 14)), 1);
	g.DrawLine(&divider, (REAL)sw - 0.5f, 0.0f, (REAL)sw - 0.5f, (REAL)h);

	// Brand.
	RectF logo(Df(20), Df(22), Df(34), Df(34));
	LinearGradientBrush grad(PointF(logo.X, logo.Y), PointF(logo.GetRight(), logo.GetBottom()), ui::C(RGB(72, 146, 255)),
	                         ui::C(RGB(136, 92, 255)));
	GraphicsPath logoPath;
	ui::AddRoundRect(logoPath, logo, Df(9));
	g.FillPath(&grad, &logoPath);
	ui::Text(g, ui::kIconGames, logo, Df(18), ui::C(RGB(255, 255, 255)), false, 1, true, false, true);
	ui::Text(g, L"PSPC", RectF(logo.GetRight() + Df(12), logo.Y - Df(2), Df(140), Df(22)), Df(19), ui::C(ui::kText), true, 0);
	ui::Text(g, L"PSP games on Windows", RectF(logo.GetRight() + Df(12), logo.Y + Df(19), Df(150), Df(16)), Df(11.5f),
	         ui::C(ui::kTextFaint), false, 0);

	// Section labels over the nav buttons.
	for (HWND b : g_nav) {
		int id = GetDlgCtrlID(b);
		const wchar_t *label = id == ID_LIBRARY ? L"LIBRARY" : id == ID_BOOT_XMB ? L"SYSTEM" : id == ID_ONLINE ? L"ONLINE" : nullptr;
		if (!label) continue;
		RECT br;
		GetWindowRect(b, &br);
		MapWindowPoints(nullptr, g_main, (POINT *)&br, 2);
		ui::Text(g, label, RectF(Df(24), (REAL)br.top - Df(24), Df(160), Df(18)), Df(10.5f), ui::C(ui::kTextFaint), true, 0);
	}

	// The account, set apart at the bottom.
	if (g_account) {
		RECT ar;
		GetWindowRect(g_account, &ar);
		MapWindowPoints(nullptr, g_main, (POINT *)&ar, 2);
		Pen sep(ui::C(ui::kBorder), 1);
		g.DrawLine(&sep, Df(20), (REAL)ar.top - Df(10), (REAL)sw - Df(20), (REAL)ar.top - Df(10));
	}

	// Header: title, count, search box (the Online page covers it).
	if (g_onlineShown) {
		Graphics screen(hdc);
		screen.DrawImage(&buffer, 0, 0);
		return;
	}
	float hx = (float)sw + Df(kGridPad);
	size_t shown = (size_t)std::max(0, ListView_GetItemCount(g_list));
	std::wstring sub = g_filter.empty() ? std::to_wstring(g_games.size()) + (g_games.size() == 1 ? L" game" : L" games")
	                                    : std::to_wstring(shown) + L" of " + std::to_wstring(g_games.size()) + L" games";
	int native = 0;
	for (const Game &game : g_games) native += game.native.state == Native::Built;
	if (g_settings.aot && !g_games.empty() && (CanBuild() || native > 0))
		sub += L"  ·  " + std::to_wstring(native) + L" built as native code";
	ui::Text(g, L"Games", RectF(hx, Df(18), Df(300), Df(30)), Df(22), ui::C(ui::kText), true, 0);
	ui::Text(g, sub.c_str(), RectF(hx, Df(48), (float)(g_searchBox.left) - hx - Df(12), Df(18)), Df(12.5f),
	         ui::C(ui::kTextFaint), false, 0);
	if (g_search) {
		RectF box((REAL)g_searchBox.left, (REAL)g_searchBox.top, (REAL)(g_searchBox.right - g_searchBox.left),
		          (REAL)(g_searchBox.bottom - g_searchBox.top));
		bool focus = GetFocus() == g_search;
		ui::FillRound(g, box, Df(8), ui::C(ui::kSurface));
		ui::StrokeRound(g, box, Df(8), ui::C(focus ? ui::kAccent : ui::kBorder), focus ? Df(1.5f) : 1);
		ui::Text(g, ui::kIconSearch, RectF(box.X + Df(10), box.Y, Df(20), box.Height), Df(14), ui::C(ui::kTextFaint), false,
		         0, true, false, true);
	}
	Graphics screen(hdc);
	screen.DrawImage(&buffer, 0, 0);
}

void Layout(int w, int h) {
	int sw = D(kSidebarW);
	// Sidebar: Library, then System; the account at the bottom.
	int y = D(104);
	for (HWND b : g_nav) {
		int id = GetDlgCtrlID(b);
		if (id == ID_BOOT_XMB || id == ID_ONLINE) y += D(34);
		MoveWindow(b, D(12), y, sw - D(24), D(44), TRUE);
		y += D(48);
	}
	MoveWindow(g_online, sw, 0, std::max(0, w - sw), h, TRUE);
	MoveWindow(g_account, D(12), h - D(68), sw - D(24), D(52), TRUE);

	int detailW = std::clamp(w * 34 / 100, D(330), D(440));
	if (w - sw - detailW < D(kCardW + 2 * kGridPad)) detailW = std::max(0, w - sw - D(kCardW + 2 * kGridPad));
	int contentR = w - detailW;
	int bh = D(36), by = D(20);
	MoveWindow(g_folderButton, contentR - D(kGridPad) - bh, by, bh, bh, TRUE);
	MoveWindow(g_refreshButton, contentR - D(kGridPad) - 2 * bh - D(6), by, bh, bh, TRUE);
	int searchW = std::clamp((contentR - sw) * 40 / 100, D(160), D(300));
	g_searchBox = {contentR - D(kGridPad) - 2 * bh - D(18) - searchW, by, contentR - D(kGridPad) - 2 * bh - D(18), by + bh};
	int editH = D(20);
	MoveWindow(g_search, g_searchBox.left + D(34), by + (bh - editH) / 2, searchW - D(44), editH, TRUE);
	MoveWindow(g_list, sw + D(kGridPad) - D(8), D(kHeaderH), contentR - sw - D(kGridPad) + D(8), std::max(0, h - D(kHeaderH)), TRUE);
	MoveWindow(g_detail, contentR, 0, detailW, h, TRUE);
	InvalidateRect(g_main, nullptr, FALSE);
}

void ContextMenu(HWND hwnd, LPARAM lp) {
	int sel = Selected();
	HMENU m = CreatePopupMenu();
	UINT on = sel >= 0 ? 0 : MF_GRAYED;
	AppendMenuW(m, on, ID_PLAY, L"&Play\tEnter");
	AppendMenuW(m, on, ID_PLAY_INTERP, L"Play in the &interpreter");
	if (CanBuild()) AppendMenuW(m, on, ID_BUILD, L"&Build native code");
	AppendMenuW(m, on, ID_SHOW_FILE, L"&Show in folder");
	AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(m, 0, ID_REFRESH, L"&Refresh\tF5");
	AppendMenuW(m, 0, ID_OPEN_FOLDER, L"&Open Games folder");
	AppendMenuW(m, 0, ID_CHOOSE_GAMES, L"&Choose the games folder...");
	POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
	if (lp == -1) {  // from the keyboard: at the selected item
		RECT r{};
		if (sel >= 0 && ListView_GetItemRect(g_list, ListView_GetNextItem(g_list, -1, LVNI_SELECTED), &r, LVIR_ICON)) {
			pt = {r.left + D(12), r.top + D(kCardImageH)};
			ClientToScreen(g_list, &pt);
		} else {
			GetCursorPos(&pt);
		}
	}
	TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
	DestroyMenu(m);
}

HWND MakeButton(HWND parent, int id, const wchar_t *text, ui::Look look, const wchar_t *glyph = nullptr,
                const wchar_t *detail = nullptr) {
	HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, parent,
	                         (HMENU)(INT_PTR)id, nullptr, nullptr);
	ui::SetLook(b, look, glyph, detail);
	return b;
}

void ApplyFonts() {
	SendMessageW(g_search, WM_SETFONT, (WPARAM)ui::Font(14, g_dpi), TRUE);
	ListMetrics();
}

void ShowInFolder(int idx) {
	if (idx < 0) return;
	std::wstring args = L"/select,\"" + g_games[idx].path.wstring() + L"\"";
	ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOW);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_CREATE: {
		g_main = hwnd;
		g_dpi = ui::DpiOf(hwnd);
		ui::DarkTitleBar(hwnd);
		ReloadSettings();
		g_nav.push_back(MakeButton(hwnd, ID_LIBRARY, L"Games", ui::Look::NavActive, ui::kIconGames));
		g_nav.push_back(MakeButton(hwnd, ID_BOOT_XMB, L"Boot XMB", ui::Look::Nav, ui::kIconXmb, L""));
		g_nav.push_back(MakeButton(hwnd, ID_PLUGINS, L"Plugins", ui::Look::Nav, ui::kIconPlugins, L"M33 plugins"));
		g_nav.push_back(MakeButton(hwnd, ID_SETTINGS, L"Settings", ui::Look::Nav, ui::kIconSettings, L"Video, audio, controls"));
		g_nav.push_back(MakeButton(hwnd, ID_ONLINE, L"Online", ui::Look::Nav, ui::kIconGlobe, L"Servers and sign-in"));
		g_account = MakeButton(hwnd, ID_NETWORK, L"Network", ui::Look::Nav, ui::kIconPerson, L"");
		g_refreshButton = MakeButton(hwnd, ID_REFRESH, L"Refresh", ui::Look::Icon, ui::kIconRefresh);
		g_folderButton = MakeButton(hwnd, ID_OPEN_FOLDER, L"Open the Games folder", ui::Look::Icon, ui::kIconFolder);
		g_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
		                           (HMENU)(INT_PTR)ID_SEARCH, nullptr, nullptr);
		SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search games");
		SetWindowSubclass(g_search, SearchSubclass, 0, 0);
		g_list = CreateWindowExW(0, WC_LISTVIEWW, L"Games",
		                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_ICON | LVS_SINGLESEL | LVS_AUTOARRANGE | LVS_SHOWSELALWAYS,
		                         0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LIST, nullptr, nullptr);
		ListView_SetExtendedListViewStyle(g_list, LVS_EX_DOUBLEBUFFER | LVS_EX_HIDELABELS);
		SetWindowTheme(g_list, L"DarkMode_Explorer", nullptr);  // dark scrollbar where Windows has one
		ListView_SetBkColor(g_list, ui::kBg);
		ListView_SetTextBkColor(g_list, ui::kBg);
		ListView_SetTextColor(g_list, ui::kText);
		SetWindowSubclass(g_list, ListSubclass, 0, 0);
		g_detail = CreateWindowExW(WS_EX_CONTROLPARENT, L"PspcDetail", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd, nullptr,
		                           nullptr, nullptr);
		g_playButton = MakeButton(g_detail, ID_PLAY, L"Play", ui::Look::Primary, ui::kIconPlay);
		g_buildButton = MakeButton(g_detail, ID_BUILD, L"Build native code", ui::Look::Secondary, ui::kIconBuild);
		g_interpButton = MakeButton(g_detail, ID_PLAY_INTERP, L"Interpreter", ui::Look::Secondary, ui::kIconCode);
		g_showButton = MakeButton(g_detail, ID_SHOW_FILE, L"Show in folder", ui::Look::Icon, ui::kIconOpenFile);
		g_emptyButton = MakeButton(g_detail, ID_OPEN_FOLDER, L"Open Games folder", ui::Look::Primary, ui::kIconFolder);
		g_chooseButton = MakeButton(g_detail, ID_CHOOSE_GAMES, L"Choose a folder...", ui::Look::Secondary, ui::kIconSearch);
		servers::Init(g_exeDir);
		online::Hooks hooks;
		hooks.findGame = [](const char *discId) {
			std::wstring id = Widen(discId);
			for (const Game &g : g_games)
				if (g.id == id) return g.path.wstring();
			return std::wstring();
		};
		hooks.openAccount = [] { OpenNetwork(); };
		g_online = online::Create(hwnd, hooks);
		ApplyFonts();
		Scan();
		RefreshNative();
		Populate({});
		SetFocus(g_list);
		// After the library: the Combat Ops server serves the missions of the disc found in it.
		online::StartAutomatic();
		return 0;
	}
	case WM_SIZE: Layout(LOWORD(lp), HIWORD(lp)); return 0;
	case WM_DPICHANGED: {
		g_dpi = HIWORD(wp);
		const RECT *r = (const RECT *)lp;
		ApplyFonts();
		SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		Populate(SelectedPath());
		return 0;
	}
	case WM_SETFOCUS: SetFocus(g_onlineShown ? g_online : g_list); return 0;
	case WM_CLOSE:
		if (online::ConfirmClose(hwnd)) DestroyWindow(hwnd);
		return 0;
	case online::WM_ONLINE_CHANGED: UpdateUi(); return 0;
	case WM_GETMINMAXINFO: ((MINMAXINFO *)lp)->ptMinTrackSize = {D(960), D(600)}; return 0;
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		RECT r;
		GetClientRect(hwnd, &r);
		PaintMain(hdc, r.right, r.bottom);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_CTLCOLOREDIT: {
		static HBRUSH surface = CreateSolidBrush(ui::kSurface);
		SetTextColor((HDC)wp, ui::kText);
		SetBkColor((HDC)wp, ui::kSurface);
		return (LRESULT)surface;
	}
	case WM_DRAWITEM: ui::DrawOwnerButton((const DRAWITEMSTRUCT *)lp, ui::kSidebar); return TRUE;
	case WM_ACTIVATEAPP:
		// Back from a game, a build or an editor: pick up new native code and settings.
		if (wp) {
			ReloadSettings();
			if (g_onlineShown) online::Refresh();
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
		if (nm->code == NM_CUSTOMDRAW) return CustomDrawList((NMLVCUSTOMDRAW *)lp);
		if (nm->code == NM_DBLCLK && ((NMITEMACTIVATE *)lp)->iItem >= 0) Play(Selected(), false);
		if (nm->code == LVN_ITEMCHANGED && (((NMLISTVIEW *)lp)->uChanged & LVIF_STATE)) UpdateUi();
		if (nm->code == LVN_GETEMPTYMARKUP) {
			auto *em = (NMLVEMPTYMARKUP *)lp;
			em->dwFlags = EMF_CENTERED;
			std::wstring text = g_games.empty() ? L"No games yet." : L"No games match “" + g_filter + L"”.";
			wcsncpy_s(em->szMarkup, text.c_str(), _TRUNCATE);
			return TRUE;
		}
		if (nm->code == LVN_KEYDOWN) {
			WORD vk = ((NMLVKEYDOWN *)lp)->wVKey;
			if (vk == VK_RETURN) Play(Selected(), false);
			if (vk == VK_F5) Rescan();
		}
		return 0;
	}
	case WM_CONTEXTMENU:
		if ((HWND)wp == g_list || (HWND)wp == g_detail) ContextMenu(hwnd, lp);
		return 0;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK: {  // Enter, from IsDialogMessage
			HWND focus = GetFocus();
			if (focus == g_search) SetFocus(g_list);
			else if (focus == g_list) Play(Selected(), false);
			else if (focus && GetParent(focus) && (GetParent(focus) == hwnd || GetParent(focus) == g_detail))
				SendMessageW(focus, BM_CLICK, 0, 0);
			return 0;
		}
		case IDCANCEL:  // Escape
			if (GetFocus() == g_search) {
				SetWindowTextW(g_search, L"");
				SetFocus(g_list);
			}
			return 0;
		case ID_PLAY: Play(Selected(), false); break;
		case ID_PLAY_INTERP: Play(Selected(), true); break;
		case ID_BUILD: Build(Selected(), false); break;
		case ID_SHOW_FILE: ShowInFolder(Selected()); break;
		case ID_REFRESH: Rescan(); break;
		case ID_OPEN_FOLDER: ShellExecuteW(nullptr, L"open", g_gamesDir.wstring().c_str(), nullptr, nullptr, SW_SHOW); break;
		case ID_SETTINGS: OpenSettings(); break;
		case ID_PLUGINS: OpenPlugins(); break;
		case ID_NETWORK: OpenNetwork(); break;
		case ID_BOOT_XMB: BootXmb(); break;
		case ID_LIBRARY: ShowPage(false); return 0;
		case ID_ONLINE: ShowPage(true); return 0;
		case ID_CHOOSE_GAMES: ChooseGamesFolder(); break;
		case ID_SEARCH:
			if (HIWORD(wp) == EN_CHANGE) {
				wchar_t text[128];
				GetWindowTextW(g_search, text, 128);
				g_filter = text;
				Populate(SelectedPath());
			}
			return 0;
		}
		// Buttons hand the keyboard back to the list.
		if ((HWND)lp && GetFocus() == (HWND)lp && !g_onlineShown) SetFocus(g_list);
		return 0;
	case WM_DESTROY:
		KillTimer(hwnd, ID_TIMER_JOBS);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// PSPC_LAUNCHER_SNAPSHOT=<file.png>: render the window (off screen) to a PNG and exit; with
// PSPC_LAUNCHER_SNAPSHOT_DIALOG=settings|plugins|network|accounts|manage|gamespy|log|choose|choosegs, the dialog instead, with
// PSPC_LAUNCHER_SNAPSHOT_PAGE=online the Online page, with PSPC_LAUNCHER_SNAPSHOT_DELAY=<ms> later than
// 1.5 s, and with PSPC_LAUNCHER_SNAPSHOT_SEARCH=<text>, the library filtered (for checking the look of changes
// without a desktop session).
bool SavePng(HWND hwnd, const fs::path &file) {
	RECT r;
	GetWindowRect(hwnd, &r);
	int w = r.right - r.left, h = r.bottom - r.top;
	HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
	HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
	HGDIOBJ old = SelectObject(dc, bmp);
	PrintWindow(hwnd, dc, 2 /* PW_RENDERFULLCONTENT */);
	SelectObject(dc, old);
	bool ok = false;
	{
		Gdiplus::Bitmap image(bmp, nullptr);
		CLSID png{};
		UINT n = 0, size = 0;
		Gdiplus::GetImageEncodersSize(&n, &size);
		std::vector<u8> buf(size);
		auto *enc = (Gdiplus::ImageCodecInfo *)buf.data();
		Gdiplus::GetImageEncoders(n, size, enc);
		for (UINT i = 0; i < n; i++)
			if (wcscmp(enc[i].MimeType, L"image/png") == 0) png = enc[i].Clsid;
		ok = image.Save(file.wstring().c_str(), &png, nullptr) == Gdiplus::Ok;
	}
	DeleteObject(bmp);
	DeleteDC(dc);
	ReleaseDC(nullptr, screen);
	return ok;
}

// The app icon: the sidebar's logo (the gamepad on the blue-violet tile) at the sizes Windows uses,
// as PNG images in an .ico.
bool WriteIcon(const fs::path &file) {
	CLSID png{};
	UINT n = 0, size = 0;
	Gdiplus::GetImageEncodersSize(&n, &size);
	std::vector<u8> encoders(size);
	auto *enc = (Gdiplus::ImageCodecInfo *)encoders.data();
	Gdiplus::GetImageEncoders(n, size, enc);
	for (UINT i = 0; i < n; i++)
		if (wcscmp(enc[i].MimeType, L"image/png") == 0) png = enc[i].Clsid;
	const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
	std::vector<std::vector<u8>> images;
	for (int s : sizes) {
		using namespace Gdiplus;
		Bitmap bmp(s, s, PixelFormat32bppPARGB);
		{
			Graphics g(&bmp);
			g.SetSmoothingMode(SmoothingModeAntiAlias);
			g.SetTextRenderingHint(TextRenderingHintAntiAlias);
			g.Clear(Color(0, 0, 0, 0));
			float inset = s <= 24 ? 0.0f : (float)s * 0.04f;
			RectF tile(inset, inset, (float)s - 2 * inset, (float)s - 2 * inset);
			LinearGradientBrush grad(PointF(tile.X, tile.Y), PointF(tile.GetRight(), tile.GetBottom()), ui::C(RGB(72, 146, 255)),
			                         ui::C(RGB(136, 92, 255)));
			GraphicsPath path;
			ui::AddRoundRect(path, tile, tile.Width * 0.24f);
			g.FillPath(&grad, &path);
			ui::Text(g, ui::kIconGames, tile, tile.Width * 0.56f, ui::C(RGB(255, 255, 255)), false, 1, true, false, true);
		}
		IStream *stream = nullptr;
		if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK) return false;
		bool ok = bmp.Save(stream, &png, nullptr) == Gdiplus::Ok;
		STATSTG st{};
		stream->Stat(&st, STATFLAG_NONAME);
		std::vector<u8> data((size_t)st.cbSize.QuadPart);
		LARGE_INTEGER zero{};
		stream->Seek(zero, STREAM_SEEK_SET, nullptr);
		ULONG got = 0;
		stream->Read(data.data(), (ULONG)data.size(), &got);
		stream->Release();
		if (!ok || got != data.size()) return false;
		images.push_back(std::move(data));
	}
	std::vector<u8> ico;
	auto put16 = [&](unsigned v) { ico.push_back((u8)v); ico.push_back((u8)(v >> 8)); };
	auto put32 = [&](u32 v) { put16(v & 0xFFFF); put16(v >> 16); };
	put16(0);
	put16(1);
	put16((unsigned)images.size());
	u32 offset = 6 + 16 * (u32)images.size();
	for (size_t i = 0; i < images.size(); i++) {
		ico.push_back((u8)(sizes[i] >= 256 ? 0 : sizes[i]));
		ico.push_back((u8)(sizes[i] >= 256 ? 0 : sizes[i]));
		ico.push_back(0);
		ico.push_back(0);
		put16(1);
		put16(32);
		put32((u32)images[i].size());
		put32(offset);
		offset += (u32)images[i].size();
	}
	for (const auto &img : images) ico.insert(ico.end(), img.begin(), img.end());
	return WriteWholeFile(file, ico.data(), ico.size());
}

fs::path g_snapshot;
std::wstring g_snapshotDialog;

void CALLBACK SnapshotTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
	KillTimer(hwnd, id);
	HWND target = hwnd;
	if (!g_snapshotDialog.empty()) {
		HWND popup = GetLastActivePopup(hwnd);
		if (popup == hwnd) return;  // the dialog isn't up yet
		target = popup;
	}
	SavePng(target, g_snapshot);
	if (target != hwnd) PostMessageW(target, WM_COMMAND, IDCANCEL, 0);
	servers::StopAll();  // (no question on closing)
	PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
	ui::EnableDpiAwareness();
	g_exeDir = ExecutableDir();
	int argc = 0;
	wchar_t **argv = cmdLine && *cmdLine ? CommandLineToArgvW(cmdLine, &argc) : nullptr;
	bool list = false;
	fs::path writeIcon;
	for (int i = 0; i < argc; i++) {
		if (std::wstring(argv[i]) == L"--list") list = true;
		else if (std::wstring(argv[i]) == L"--write-icon" && i + 1 < argc) writeIcon = argv[++i];
		else g_gamesDir = argv[i];
	}
	if (!writeIcon.empty()) {
		Gdiplus::GdiplusStartupInput gin;
		ULONG_PTR token;
		Gdiplus::GdiplusStartup(&token, &gin, nullptr);
		bool ok = WriteIcon(writeIcon);
		Gdiplus::GdiplusShutdown(token);
		return ok ? 0 : 1;
	}
	if (argv) LocalFree(argv);
	if (g_gamesDir.empty()) {
		settings::Settings s;
		settings::Load(settings::DefaultPath(), s);
		if (!s.gamesDir.empty()) g_gamesDir = fs::u8path(s.gamesDir);
	}
	if (g_gamesDir.empty()) {
		fs::path found = FindUp(g_exeDir, "Games");
		g_gamesDir = found.empty() ? g_exeDir / "Games" : found;
	}
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // the file dialogs
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
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
	wc.lpszClassName = L"PspcLauncher";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassW(&wc);
	std::wstring title = L"PSPC";
	wchar_t snap[MAX_PATH] = {}, snapDialog[32] = {};
	if (GetEnvironmentVariableW(L"PSPC_LAUNCHER_SNAPSHOT", snap, MAX_PATH)) g_snapshot = snap;
	if (GetEnvironmentVariableW(L"PSPC_LAUNCHER_SNAPSHOT_DIALOG", snapDialog, 32)) g_snapshotDialog = snapDialog;
	wchar_t snapSearch[64] = {};
	GetEnvironmentVariableW(L"PSPC_LAUNCHER_SNAPSHOT_SEARCH", snapSearch, 64);
	int x = g_snapshot.empty() ? CW_USEDEFAULT : -6000;
	int dpi = ui::DpiOf(nullptr);
	HWND hwnd = CreateWindowW(wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, CW_USEDEFAULT,
	                          ui::Px(1280, dpi), ui::Px(780, dpi), nullptr, nullptr, inst, nullptr);
	ShowWindow(hwnd, g_snapshot.empty() ? show : SW_SHOWNOACTIVATE);
	if (!g_snapshot.empty()) {
		wchar_t delay[16] = {};
		int ms = GetEnvironmentVariableW(L"PSPC_LAUNCHER_SNAPSHOT_DELAY", delay, 16) ? _wtoi(delay) : 0;
		SetTimer(hwnd, 1, ms > 0 ? ms : 1500, SnapshotTimer);
		if (snapSearch[0]) SetWindowTextW(g_search, snapSearch);
		if (g_snapshotDialog == L"settings") OpenSettings();
		else if (g_snapshotDialog == L"plugins") OpenPlugins();
		else if (g_snapshotDialog == L"network") OpenNetwork();
		else if (g_snapshotDialog == L"accounts") serverdlg::NetworkAccounts(hwnd);
		else if (g_snapshotDialog == L"log") serverdlg::ShowLog(hwnd, servers::Kind::Sfco, L"Syphon Filter: Combat Ops: log");
		else if (g_snapshotDialog == L"manage") {
			settings::Settings s;
			settings::Load(settings::DefaultPath(), s);
			serverdlg::ManageSfco(hwnd, s);
		} else if (g_snapshotDialog == L"gamespy") {
			settings::Settings s;
			settings::Load(settings::DefaultPath(), s);
			serverdlg::ManageGameSpy(hwnd, s);
		} else if (g_snapshotDialog == L"choosegs") {
			serverdlg::Target t = serverdlg::Target::Public;
			std::wstring address;
			serverdlg::ChooseServer(hwnd, L"GameSpy online", L"Where should Star Wars Battlefront: Elite Squadron go when you choose online play?",
			                        true, &t, &address, L"OpenSpy (openspy.net), the public GameSpy revival");
		} else if (g_snapshotDialog == L"choose") {
			serverdlg::Target t = serverdlg::Target::Other;
			std::wstring address = L"192.168.1.20";
			serverdlg::ChooseServer(hwnd, L"Combat Ops online", L"Where should Syphon Filter: Combat Ops go when you choose online play?",
			                        true, &t, &address);
		}
		wchar_t snapPage[32] = {};
		if (GetEnvironmentVariableW(L"PSPC_LAUNCHER_SNAPSHOT_PAGE", snapPage, 32) && std::wstring(snapPage) == L"online") ShowPage(true);
	}
	MSG m;
	while (GetMessageW(&m, nullptr, 0, 0) > 0) {
		// Ctrl+F: search; F5: refresh; Tab moves between the controls (as in a dialog).
		if (m.message == WM_KEYDOWN && m.wParam == 'F' && GetKeyState(VK_CONTROL) < 0 && GetActiveWindow() == hwnd) {
			SetFocus(g_search);
			SendMessageW(g_search, EM_SETSEL, 0, -1);
			continue;
		}
		if (m.message == WM_KEYDOWN && m.wParam == VK_F5 && GetActiveWindow() == hwnd) {
			Rescan();
			continue;
		}
		if (IsDialogMessageW(hwnd, &m)) continue;
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	g_games.clear();  // GDI+ bitmaps, before GDI+ goes away
	Gdiplus::GdiplusShutdown(token);
	return 0;
}
