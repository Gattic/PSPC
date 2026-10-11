// SPDX-License-Identifier: GPL-2.0-or-later
//
// The launcher's Online page (see online_page.h). Drawn with GDI+ like the rest of the launcher;
// the buttons are owner-drawn children placed by Layout(), which also says where the text goes.
// The page scrolls (mouse wheel, keys, or moving the focus) when the window is too short for the
// cards, with a slim indicator on the right instead of a light system scroll bar.
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <commctrl.h>
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>
#include <uxtheme.h>

#include "frontend/online_page.h"

#include <string>
#include <vector>

#include "common/json.h"
#include "common/settings.h"
#include "frontend/server_dialogs.h"
#include "frontend/servers.h"
#include "frontend/ui_theme.h"

namespace online {

namespace {

using Gdiplus::RectF;
using servers::Kind;
using servers::State;

// Layout, in pixels at 96 DPI.
constexpr int kPad = 24, kHeaderH = 88, kCardPad = 18, kCardGap = 14, kLabelW = 112, kRowH = 44, kButtonH = 32, kCardH = 214,
              kMaxCardW = 980;

enum : int {
	kYou = 0,      // Account... / Change...
	kYou2,         // Sign in here (PSPC Network)
	kStart,        // Start / Stop
	kManage,       // Accounts... / Manage...
	kLog,
	kAuto,         // start with PSPC
	kLan,          // PSPC Network: open to other PCs
	kCopy,         // copy the address players use
	kButtonsPerCard,
};

struct Card {
	Kind kind;
	const wchar_t *glyph, *title, *summary;
	HWND b[kButtonsPerCard] = {};
	State shown = State::Stopped;
	RectF rect;
};

HWND g_page = nullptr, g_parent = nullptr;
Hooks g_hooks;
settings::Settings g_s;
std::vector<Card> g_cards;
int g_dpi = 96, g_scroll = 0, g_contentH = 0;
bool g_inTimer = false, g_showingError = false;

int D(int v) { return ui::Px(v, g_dpi); }
float Df(float v) { return ui::Pxf(v, g_dpi); }

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

void Reload() {
	g_s = settings::Settings();
	settings::Load(settings::DefaultPath(), g_s);
}

// Changes pspc.ini: re-read (other dialogs may have written it), changed, saved.
template <class F>
void Modify(F change) {
	Reload();
	change(g_s);
	if (!settings::Save(settings::DefaultPath(), g_s))
		MessageBoxW(g_page, (L"Couldn't write " + settings::DefaultPath().wstring()).c_str(), L"PSPC", MB_ICONERROR);
}

Card *CardOf(Kind k) {
	for (Card &c : g_cards)
		if (c.kind == k) return &c;
	return nullptr;
}

bool IsLocalHost(const std::string &h) { return h.empty() || h == "localhost" || h.rfind("127.", 0) == 0; }

// Whether the player's account server is the one this launcher runs.
bool OnOwnNetwork() {
	const std::string &url = g_s.npServer;
	std::string port = ":" + std::to_string(g_s.hostNetworkPort);
	if (url.size() < port.size() || url.compare(url.size() - port.size(), port.size(), port) != 0) return false;
	return url == Narrow(servers::NetworkUrl(g_s, true)) || url == Narrow(servers::NetworkUrl(g_s, false)) ||
	       url == "http://localhost" + port;
}

std::wstring ServerName(const std::string &url) { return OnOwnNetwork() ? L"this PC's server" : Widen(url); }

std::wstring GameUmd() { return g_hooks.findGame ? g_hooks.findGame(servers::kSfcoDiscId) : L""; }

// The setting that starts a server with PSPC.
bool &AutoStart(settings::Settings &s, Kind k) {
	return k == Kind::Network ? s.hostNetwork : k == Kind::Sfco ? s.hostSfco : k == Kind::GameSpy ? s.hostGameSpy : s.hostAdhoc;
}

// Where the GameSpy games go online (they share one redirect: the first game's says).
std::string GameSpyRedirect() { return servers::Redirect(servers::kGameSpyGames[0].discId, servers::kGameSpyDomain); }

bool SetGameSpyRedirect(const std::string &target, std::wstring *error) {
	for (const servers::GameSpyGame &g : servers::kGameSpyGames)
		if (!servers::SetRedirect(g.name, g.discId, servers::kGameSpyDomain, target, error)) return false;
	return true;
}

std::wstring GameSpyAddress() { return g_s.hostGameSpyAddress.empty() ? servers::LanAddress() : Widen(g_s.hostGameSpyAddress); }

// ---- what the cards say

struct Texts {
	std::wstring you, host, note;
	COLORREF hostColor = ui::kTextDim, noteColor = ui::kTextFaint;
};

Texts TextsOf(const Card &c) {
	Texts t;
	State st = c.shown;
	std::wstring missing = servers::Missing(c.kind);
	std::wstring lan = servers::LanAddress();
	switch (c.kind) {
	case Kind::Network: {
		std::wstring id = Widen(g_s.npOnlineId.empty() ? "PSPCPlayer" : g_s.npOnlineId);
		if (g_s.npServer.empty()) t.you = L"Your games sign in locally as " + id + L", without an account server.";
		else if (g_s.npToken.empty()) t.you = L"Not signed in to " + ServerName(g_s.npServer) + L" yet.";
		else t.you = L"Signed in as " + id + L" on " + ServerName(g_s.npServer) + L".";
		if (st == State::Running)
			t.host = g_s.hostNetworkLan ? L"Running. Players sign in at " + servers::NetworkUrl(g_s, false) + L" (Account... in their PSPC)."
			                            : L"Running, for this PC only.";
		break;
	}
	case Kind::Sfco: {
		std::string to = servers::Redirect(servers::kSfcoDiscId, servers::kSfcoDomain);
		t.you = to.empty() ? L"Combat Ops has no server to go online to: choose one." :
		        IsLocalHost(to) ? L"Combat Ops goes online to this PC's server." : L"Combat Ops goes online to " + Widen(to) + L".";
		if (st == State::Running)
			t.host = L"Running. Players' games connect to " + (g_s.hostSfcoAddress.empty() ? lan : Widen(g_s.hostSfcoAddress)) + L".";
		std::wstring umd = GameUmd();
		t.note = umd.empty() ? L"Add the Combat Ops disc (NPUG80114) to your library to serve its official missions."
		                     : L"Official missions from " + fs::path(umd).filename().wstring() + L".";
		break;
	}
	case Kind::GameSpy: {
		std::string to = GameSpyRedirect();
		t.you = to.empty()                   ? L"Elite Squadron has no GameSpy server to go online to: choose one."
		        : IsLocalHost(to)            ? L"Elite Squadron goes online to this PC's server."
		        : to == servers::kOpenSpyTarget ? L"Elite Squadron goes online to OpenSpy (openspy.net), the public GameSpy revival."
		                                     : L"Elite Squadron goes online to " + Widen(to) + L".";
		if (st == State::Running) {
			json::Value status = json::Value::Parse(servers::Status(Kind::GameSpy));
			size_t online = status["online"].Array().size(), games = status["games"].Array().size();
			t.host = L"Running: " + std::to_wstring(online) + (online == 1 ? L" player" : L" players") + L" online, " + std::to_wstring(games) +
			         (games == 1 ? L" game" : L" games") + L" hosted. Other PCs use " + GameSpyAddress() + L".";
		}
		// The game's Join screen lists dedicated servers (there are none any more) until R is pressed.
		t.note = L"In the game, Create New GameSpy ID the first time. To join a friend's game: Join, then R for Player-Hosted Servers.";
		break;
	}
	case Kind::Adhoc: {
		const bool ours = IsLocalHost(g_s.adhocServer);
		t.you = ours ? L"Ad hoc play goes through this PC's server." : L"Ad hoc play goes through " + Widen(g_s.adhocServer) + L".";
		if (st == State::Running) t.host = L"Running. Players on your network use " + lan + L" as their ad hoc server.";
		if (ours && st != State::Running) {
			t.note = L"Start it before playing ad hoc on this PC.";
			t.noteColor = ui::kAmber;
		}
		break;
	}
	}
	if (st == State::Starting) t.host = L"Starting...";
	if (st == State::Failed) {
		t.host = L"It stopped unexpectedly: the log says why.";
		t.hostColor = ui::kAmber;
	}
	if (st == State::Stopped) {
		t.host = c.kind == Kind::Network   ? L"Run it here for you and your friends: accounts, sign-in and friend lists."
		         : c.kind == Kind::Sfco    ? L"Host the game's lobby, matches, shared missions and leaderboards here."
		         : c.kind == Kind::GameSpy ? L"Host GameSpy here: accounts, friends, games to join and leaderboards."
		                                   : L"Needed for ad hoc play between PSPC windows on this PC, or with friends.";
		if (!missing.empty()) {
			t.host = missing;
			t.hostColor = ui::kAmber;
		}
	}
	return t;
}

// ---- layout

// Where card i goes (page coordinates, scrolled).
RectF CardRect(int i, int w) {
	float cw = (float)std::min(w - 2 * D(kPad), D(kMaxCardW));
	float y = (float)(D(kHeaderH) + i * (D(kCardH) + D(kCardGap)) - g_scroll);
	return RectF((float)D(kPad), y, cw, (float)D(kCardH));
}

float RowY(const RectF &card, int row) { return card.Y + Df(kCardPad + 54) + (float)(row * D(kRowH)); }

void Place(HWND b, float x, float y, float w, float h, bool show) {
	if (!b) return;
	if (!show) {
		ShowWindow(b, SW_HIDE);
		return;
	}
	SetWindowPos(b, nullptr, (int)x, (int)y, (int)w, (int)h, SWP_NOZORDER | SWP_NOACTIVATE);
	ShowWindow(b, SW_SHOWNA);
}

void SyncScroll(int h) { g_scroll = std::clamp(g_scroll, 0, std::max(0, g_contentH - h)); }

void Layout() {
	if (!g_page) return;
	RECT rc;
	GetClientRect(g_page, &rc);
	g_contentH = D(kHeaderH) + (int)g_cards.size() * (D(kCardH) + D(kCardGap)) + D(110);
	SyncScroll(rc.bottom);
	for (size_t i = 0; i < g_cards.size(); i++) {
		Card &c = g_cards[i];
		c.rect = CardRect((int)i, rc.right);
		const RectF &r = c.rect;
		const float bh = (float)D(kButtonH), right = r.GetRight() - Df(kCardPad), off = (float)(D(kRowH) - D(kButtonH)) / 2;
		const bool running = c.shown == State::Running || c.shown == State::Starting;
		const bool canRun = servers::Missing(c.kind).empty();
		// Row 1: your games.
		float y = RowY(r, 0) + off;
		Place(c.b[kYou], right - Df(118), y, Df(118), bh, true);
		bool signInHere = c.kind == Kind::Network && running && !OnOwnNetwork();
		Place(c.b[kYou2], right - Df(118 + 8 + 128), y, Df(128), bh, signInHere);
		// Row 2: this PC.
		y = RowY(r, 1) + off;
		float x = right;
		x -= Df(80);
		Place(c.b[kLog], x, y, Df(80), bh, true);
		if (c.b[kManage]) {
			x -= Df(118 + 8);
			Place(c.b[kManage], x, y, Df(118), bh, true);
			EnableWindow(c.b[kManage], canRun);
		}
		x -= Df(104 + 8);
		Place(c.b[kStart], x, y, Df(104), bh, true);
		SetWindowTextW(c.b[kStart], running ? L"Stop" : L"Start");
		ui::SetLook(c.b[kStart], running ? ui::Look::Secondary : ui::Look::Primary, running ? ui::kIconStop : ui::kIconPlay);
		EnableWindow(c.b[kStart], canRun || running);
		// Row 3: options.
		y = RowY(r, 2) + off;
		x = r.X + Df(kCardPad + kLabelW);
		Place(c.b[kAuto], x, y, Df(170), bh, true);
		Place(c.b[kLan], x + Df(178), y, Df(190), bh, c.b[kLan] != nullptr);
		Place(c.b[kCopy], right - Df(140), y, Df(140), bh, c.shown == State::Running && c.kind != Kind::Adhoc);
	}
	InvalidateRect(g_page, nullptr, FALSE);
}

// ---- painting

void Pill(Gdiplus::Graphics &g, const RectF &card, State st) {
	const wchar_t *text = st == State::Running ? L"Running" : st == State::Starting ? L"Starting" : st == State::Failed ? L"Stopped unexpectedly"
	                                                                                                                  : L"Stopped";
	COLORREF color = st == State::Running ? ui::kGreen : st == State::Starting ? ui::kInfo : st == State::Failed ? ui::kAmber : ui::kTextFaint;
	float px = Df(12), h = Df(26);
	float w = ui::Measure(g, text, px, true).Width + Df(34);
	RectF pill(card.GetRight() - Df(kCardPad) - w, card.Y + Df(kCardPad) + Df(4), w, h);
	ui::FillRound(g, pill, h / 2, ui::C(ui::kSurfaceHot));
	Gdiplus::SolidBrush dot(ui::C(color));
	g.FillEllipse(&dot, RectF(pill.X + Df(11), pill.Y + h / 2 - Df(4), Df(8), Df(8)));
	ui::Text(g, text, RectF(pill.X + Df(24), pill.Y, w - Df(26), h), px, ui::C(st == State::Stopped ? ui::kTextDim : ui::kText), true);
}

void PaintCard(Gdiplus::Graphics &g, const Card &c) {
	using namespace Gdiplus;
	const RectF &r = c.rect;
	ui::FillRound(g, r, Df(12), ui::C(ui::kSurface));
	ui::StrokeRound(g, r, Df(12), ui::C(ui::kBorder, 160), 1);
	// Icon, title, what it is.
	RectF icon(r.X + Df(kCardPad), r.Y + Df(kCardPad), Df(40), Df(40));
	LinearGradientBrush grad(PointF(icon.X, icon.Y), PointF(icon.GetRight(), icon.GetBottom()), ui::C(RGB(72, 146, 255)), ui::C(RGB(136, 92, 255)));
	GraphicsPath iconPath;
	ui::AddRoundRect(iconPath, icon, Df(10));
	g.FillPath(&grad, &iconPath);
	ui::Text(g, c.glyph, icon, Df(19), ui::C(RGB(255, 255, 255)), false, 1, true, false, true);
	float tx = icon.GetRight() + Df(14), tw = r.GetRight() - Df(kCardPad) - tx - Df(190);
	ui::Text(g, c.title, RectF(tx, r.Y + Df(kCardPad) - Df(1), tw, Df(22)), Df(16.5f), ui::C(ui::kText), true);
	ui::Text(g, c.summary, RectF(tx, r.Y + Df(kCardPad) + Df(22), r.GetRight() - Df(kCardPad) - tx, Df(18)), Df(12.5f), ui::C(ui::kTextDim));
	Pill(g, r, c.shown);
	Texts t = TextsOf(c);
	// The rows: a label, the text, the buttons (children) on the right.
	const wchar_t *labels[3] = {c.kind == Kind::Network ? L"YOU" : c.kind == Kind::Adhoc ? L"YOUR GAMES" : L"YOUR GAME", L"THIS PC", L""};
	float textX = r.X + Df(kCardPad + kLabelW);
	float buttonsW[3] = {Df(118 + 16) + (c.kind == Kind::Network && IsWindowVisible(c.b[kYou2]) ? Df(136) : 0),
	                     Df(80 + 8 + 104 + 16) + (c.b[kManage] ? Df(126) : 0), 0};
	Pen line(ui::C(ui::kBorder, 120), 1);
	for (int row = 0; row < 2; row++) {
		float y = RowY(r, row);
		g.DrawLine(&line, r.X + Df(kCardPad), y, r.GetRight() - Df(kCardPad), y);
		ui::Text(g, labels[row], RectF(r.X + Df(kCardPad), y, Df(kLabelW - 8), (float)D(kRowH)), Df(10.5f), ui::C(ui::kTextFaint), true);
		const std::wstring &s = row == 0 ? t.you : t.host;
		COLORREF color = row == 0 ? ui::kText : t.hostColor;
		ui::Text(g, s.c_str(), RectF(textX, y, r.GetRight() - Df(kCardPad) - buttonsW[row] - textX, (float)D(kRowH)), Df(13), ui::C(color), false, 0,
		         true, true);
	}
	float y3 = RowY(r, 2);
	g.DrawLine(&line, r.X + Df(kCardPad), y3, r.GetRight() - Df(kCardPad), y3);
	if (!t.note.empty()) {
		float nx = textX + Df(c.b[kLan] ? 378 : 182);
		float nr = r.GetRight() - Df(kCardPad) - (IsWindowVisible(c.b[kCopy]) ? Df(150) : 0);
		ui::Text(g, t.note.c_str(), RectF(nx, y3, std::max(0.0f, nr - nx), (float)D(kRowH)), Df(12), ui::C(t.noteColor), false, 0, true, true);
	}
}

void Paint(HDC hdc, int w, int h) {
	using namespace Gdiplus;
	if (w <= 0 || h <= 0) return;
	Bitmap buffer(w, h, PixelFormat32bppPARGB);
	Graphics g(&buffer);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	g.Clear(ui::C(ui::kBg));
	float y0 = -(float)g_scroll;
	ui::Text(g, L"Online", RectF(Df(kPad), y0 + Df(18), Df(400), Df(30)), Df(22), ui::C(ui::kText), true);
	ui::Text(g, L"The servers PSP games went online to (Sony's, GameSpy's) are gone; PSPC brings its own. Run them here for you and your friends, or use someone else's.",
	         RectF(Df(kPad), y0 + Df(48), (float)w - 2 * Df(kPad), Df(36)), Df(12.5f), ui::C(ui::kTextFaint), false, 0, false, true);
	for (const Card &c : g_cards) PaintCard(g, c);
	// How friends join.
	float fy = (g_cards.empty() ? y0 + Df(kHeaderH) : g_cards.back().rect.GetBottom()) + Df(18);
	float fw = std::min((float)w - 2 * Df(kPad), Df(kMaxCardW));
	ui::Text(g, ui::kIconInfo, RectF(Df(kPad), fy, Df(20), Df(20)), Df(14), ui::C(ui::kTextFaint), false, 0, true, false, true);
	ui::Text(g,
	         L"Playing on a friend's server: in PSPC Network choose Account... and enter their address, and in each game choose "
	         L"Change... and enter it too. Hosting for friends on other PCs: when Windows asks whether PSPC's servers (Python, "
	         L"pspc.exe) may use the network, allow private networks. Over the internet, forward the servers' ports on your router "
	         L"to this PC (PSPC Network: the port in its address; Combat Ops: 10070-10079 and 10443; GameSpy: TCP 80, 28910 and "
	         L"29900-29920, UDP 27900-27901; ad hoc: 27312).",
	         RectF(Df(kPad) + Df(26), fy, fw - Df(26), Df(80)), Df(12), ui::C(ui::kTextFaint), false, 0, false, true);
	// Where the view is, when it doesn't show everything.
	if (g_contentH > h) {
		float trackH = (float)h - Df(16), thumbH = std::max(Df(40), trackH * (float)h / (float)g_contentH);
		float thumbY = Df(8) + (trackH - thumbH) * (float)g_scroll / (float)std::max(1, g_contentH - h);
		ui::FillRound(g, RectF((float)w - Df(8), thumbY, Df(4), thumbH), Df(2), ui::C(ui::kBorder));
	}
	Graphics screen(hdc);
	screen.DrawImage(&buffer, 0, 0);
}

// ---- actions

void CopyText(const std::wstring &text) {
	if (!OpenClipboard(g_page)) return;
	EmptyClipboard();
	size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
		memcpy(GlobalLock(mem), text.c_str(), bytes);
		GlobalUnlock(mem);
		SetClipboardData(CF_UNICODETEXT, mem);
	}
	CloseClipboard();
}

void PollAll();

// A game server started here, and the game had nowhere to go online: this PC's server.
void PointGameHere(Kind k) {
	std::wstring error;
	if (k == Kind::Sfco && servers::Redirect(servers::kSfcoDiscId, servers::kSfcoDomain).empty())
		servers::SetRedirect("Syphon Filter: Combat Ops", servers::kSfcoDiscId, servers::kSfcoDomain, "127.0.0.1", &error);
	if (k == Kind::GameSpy && GameSpyRedirect().empty()) SetGameSpyRedirect("127.0.0.1", &error);
}

bool StartServer(Kind k) {
	Reload();
	std::wstring error;
	if (!servers::Start(k, g_s, k == Kind::Sfco ? GameUmd() : L"", &error)) {
		MessageBoxW(g_page, error.c_str(), L"PSPC", MB_ICONWARNING);
		return false;
	}
	return true;
}

void SignInHere() {
	std::string url = Narrow(servers::NetworkUrl(g_s, true));
	Modify([&](settings::Settings &s) {
		if (s.npServer != url) s.npToken.clear();
		s.npServer = url;
	});
	if (g_hooks.openAccount) g_hooks.openAccount();
	Refresh();
}

void StartStop(Card &c) {
	PollAll();
	if (c.shown == State::Running || c.shown == State::Starting) {
		servers::Stop(c.kind);
	} else if (StartServer(c.kind)) {
		if (c.kind == Kind::Network && g_s.npServer.empty() &&
		    MessageBoxW(g_page,
		                L"Your PSPC Network server is starting.\n\nCreate your own account on it now? Your games then sign in "
		                L"with it, and friends can add you.",
		                L"PSPC", MB_YESNO | MB_ICONQUESTION) == IDYES)
			SignInHere();
		PointGameHere(c.kind);
	}
	PollAll();
	Layout();
}

void ChangeTarget(Card &c) {
	using serverdlg::Target;
	if (c.kind == Kind::Network) {
		if (g_hooks.openAccount) g_hooks.openAccount();
		Refresh();
		return;
	}
	if (c.kind == Kind::Sfco) {
		std::string to = servers::Redirect(servers::kSfcoDiscId, servers::kSfcoDomain);
		Target t = to.empty() ? Target::None : IsLocalHost(to) ? Target::ThisPc : Target::Other;
		std::wstring address = IsLocalHost(to) ? L"" : Widen(to);
		if (!serverdlg::ChooseServer(g_page, L"Combat Ops online",
		                             L"Where should Syphon Filter: Combat Ops go when you choose online play? (Your games also need "
		                             L"a PSPC Network sign-in: see PSPC Network above.)",
		                             true, &t, &address))
			return;
		std::wstring error;
		std::string target = t == Target::ThisPc ? "127.0.0.1" : t == Target::Other ? Narrow(address) : "";
		if (!servers::SetRedirect("Syphon Filter: Combat Ops", servers::kSfcoDiscId, servers::kSfcoDomain, target, &error))
			MessageBoxW(g_page, error.c_str(), L"PSPC", MB_ICONWARNING);
	} else if (c.kind == Kind::GameSpy) {
		std::string to = GameSpyRedirect();
		Target t = to.empty() ? Target::None : IsLocalHost(to) ? Target::ThisPc : to == servers::kOpenSpyTarget ? Target::Public : Target::Other;
		std::wstring address = t == Target::Other ? Widen(to) : L"";
		if (!serverdlg::ChooseServer(g_page, L"GameSpy online",
		                             L"Where should Star Wars Battlefront: Elite Squadron go when you choose online play? Its players "
		                             L"log in with GameSpy accounts on that server.",
		                             true, &t, &address, L"OpenSpy (openspy.net), the public GameSpy revival"))
			return;
		std::wstring error;
		std::string target = t == Target::ThisPc ? "127.0.0.1" : t == Target::Other ? Narrow(address) : t == Target::Public ? servers::kOpenSpyTarget : "";
		if (!SetGameSpyRedirect(target, &error)) MessageBoxW(g_page, error.c_str(), L"PSPC", MB_ICONWARNING);
	} else {
		Target t = IsLocalHost(g_s.adhocServer) ? Target::ThisPc : Target::Other;
		std::wstring address = IsLocalHost(g_s.adhocServer) ? L"" : Widen(g_s.adhocServer);
		if (!serverdlg::ChooseServer(g_page, L"Ad hoc play",
		                             L"Which ad hoc server should games meet other players through? Everyone playing together "
		                             L"uses the same one.",
		                             false, &t, &address))
			return;
		Modify([&](settings::Settings &s) { s.adhocServer = t == Target::ThisPc ? "localhost" : Narrow(address); });
	}
	Layout();
}

void Command(Card &c, int which) {
	switch (which) {
	case kYou: ChangeTarget(c); break;
	case kYou2: SignInHere(); break;
	case kStart: StartStop(c); break;
	case kManage:
		if (c.kind == Kind::Network) {
			serverdlg::NetworkAccounts(g_page);
		} else if (c.kind == Kind::GameSpy) {
			Reload();
			settings::Settings s = g_s;
			if (serverdlg::ManageGameSpy(g_page, s)) Modify([&](settings::Settings &f) { f.hostGameSpyAddress = s.hostGameSpyAddress; });
		} else {
			Reload();
			settings::Settings s = g_s;
			if (serverdlg::ManageSfco(g_page, s))
				Modify([&](settings::Settings &f) {
					f.hostSfcoAddress = s.hostSfcoAddress;
					f.hostSfcoAllowLocal = s.hostSfcoAllowLocal;
				});
		}
		break;
	case kLog: serverdlg::ShowLog(g_page, c.kind, (std::wstring(c.title) + L": log").c_str()); break;
	case kAuto: {
		bool on = !ui::Checked(c.b[kAuto]);
		ui::SetChecked(c.b[kAuto], on);
		Modify([&](settings::Settings &s) { AutoStart(s, c.kind) = on; });
		break;
	}
	case kLan: {
		bool on = !ui::Checked(c.b[kLan]);
		ui::SetChecked(c.b[kLan], on);
		Modify([&](settings::Settings &s) { s.hostNetworkLan = on; });
		// A running server listens anew.
		if (servers::Poll(Kind::Network) == State::Running || servers::Poll(Kind::Network) == State::Starting) {
			servers::Stop(Kind::Network);
			StartServer(Kind::Network);
		}
		break;
	}
	case kCopy:
		CopyText(c.kind == Kind::Network   ? servers::NetworkUrl(g_s, false)
		         : c.kind == Kind::GameSpy ? GameSpyAddress()
		                                   : (g_s.hostSfcoAddress.empty() ? servers::LanAddress() : Widen(g_s.hostSfcoAddress)));
		break;
	}
	PollAll();
	Layout();
}

void PollAll() {
	std::vector<Card *> failed;
	bool changed = false;
	for (Card &c : g_cards) {
		State before = c.shown;
		c.shown = servers::Poll(c.kind);
		if (c.shown == before) continue;
		changed = true;
		if (c.shown == State::Failed) failed.push_back(&c);
	}
	if (!changed) return;
	Layout();
	PostMessageW(g_parent, WM_ONLINE_CHANGED, 0, 0);
	// A server that ended on its own: what its log says (once; not from within another such message).
	for (Card *c : failed) {
		if (g_showingError) break;
		g_showingError = true;
		std::wstring tail = servers::LogTail(c->kind, 8);
		MessageBoxW(g_page, (std::wstring(L"The ") + c->title + L" server stopped:\n\n" + tail).c_str(), L"PSPC", MB_ICONWARNING);
		g_showingError = false;
	}
}

HWND Button(int id, const wchar_t *text, ui::Look look, const wchar_t *glyph = nullptr) {
	HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY, 0, 0, 0, 0, g_page, (HMENU)(INT_PTR)id, nullptr,
	                         nullptr);
	ui::SetLook(b, look, glyph);
	return b;
}

void MakeCards() {
	g_cards = {
		{Kind::Network, ui::kIconPeople, L"PSPC Network", L"Sign-in, online IDs and friends for online games: PSPC's stand-in for the PlayStation Network."},
		{Kind::Sfco, ui::kIconGamepad, L"Syphon Filter: Combat Ops", L"The game's own online service: lobby, matches, shared missions, cells and leaderboards."},
		{Kind::GameSpy, ui::kIconGlobe, L"GameSpy: Star Wars Battlefront: Elite Squadron",
		 L"GameSpy's services for the game: accounts, friends, games to join and leaderboards."},
		{Kind::Adhoc, ui::kIconWifi, L"Ad hoc", L"Local multiplayer (ad hoc mode) over a network, as PPSSPP's ad hoc servers do it."},
	};
	for (size_t i = 0; i < g_cards.size(); i++) {
		Card &c = g_cards[i];
		int base = 600 + (int)i * kButtonsPerCard;
		c.b[kYou] = Button(base + kYou, c.kind == Kind::Network ? L"Account..." : L"Change...", ui::Look::Secondary,
		                   c.kind == Kind::Network ? ui::kIconPerson : ui::kIconEdit);
		if (c.kind == Kind::Network) c.b[kYou2] = Button(base + kYou2, L"Sign in here", ui::Look::Primary, ui::kIconPerson);
		c.b[kStart] = Button(base + kStart, L"Start", ui::Look::Primary, ui::kIconPlay);
		if (c.kind != Kind::Adhoc)
			c.b[kManage] = Button(base + kManage, c.kind == Kind::Network ? L"Accounts..." : L"Manage...", ui::Look::Secondary,
			                      c.kind == Kind::Network ? ui::kIconPeople : ui::kIconSettings);
		c.b[kLog] = Button(base + kLog, L"Log", ui::Look::Ghost, ui::kIconDocument);
		c.b[kAuto] = Button(base + kAuto, L"Start with PSPC", ui::Look::Toggle);
		if (c.kind == Kind::Network) c.b[kLan] = Button(base + kLan, L"Open to other PCs", ui::Look::Toggle);
		if (c.kind != Kind::Adhoc) c.b[kCopy] = Button(base + kCopy, L"Copy address", ui::Look::Ghost, ui::kIconCopy);
	}
}

void SyncToggles() {
	for (Card &c : g_cards) {
		ui::SetChecked(c.b[kAuto], AutoStart(g_s, c.kind));
		if (c.b[kLan]) ui::SetChecked(c.b[kLan], g_s.hostNetworkLan);
	}
}

void Scroll(int to) {
	RECT rc;
	GetClientRect(g_page, &rc);
	int before = g_scroll;
	g_scroll = std::clamp(to, 0, std::max(0, g_contentH - (int)rc.bottom));
	if (g_scroll != before) Layout();
}

LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		RECT r;
		GetClientRect(hwnd, &r);
		Paint(hdc, r.right, r.bottom);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_SIZE: Layout(); return 0;
	case WM_SHOWWINDOW:
		if (wp) {
			Refresh();
			SetTimer(hwnd, 1, 1000, nullptr);
		}
		return 0;
	case WM_TIMER:
		if (!g_inTimer) {
			g_inTimer = true;
			PollAll();
			// The GameSpy card shows who is online: kept current.
			if (servers::Poll(Kind::GameSpy) == State::Running) InvalidateRect(hwnd, nullptr, FALSE);
			g_inTimer = false;
		}
		return 0;
	case WM_DPICHANGED_AFTERPARENT:
		g_dpi = ui::DpiOf(hwnd);
		Layout();
		return 0;
	case WM_MOUSEWHEEL: Scroll(g_scroll - GET_WHEEL_DELTA_WPARAM(wp) * D(kRowH) / WHEEL_DELTA); return 0;
	case WM_LBUTTONDOWN: SetFocus(hwnd); return 0;
	case WM_KEYDOWN: {
		RECT rc;
		GetClientRect(hwnd, &rc);
		switch (wp) {
		case VK_UP: Scroll(g_scroll - D(kRowH)); break;
		case VK_DOWN: Scroll(g_scroll + D(kRowH)); break;
		case VK_PRIOR: Scroll(g_scroll - rc.bottom); break;
		case VK_NEXT: Scroll(g_scroll + rc.bottom); break;
		case VK_HOME: Scroll(0); break;
		case VK_END: Scroll(g_contentH); break;
		}
		return 0;
	}
	case WM_DRAWITEM: ui::DrawOwnerButton((const DRAWITEMSTRUCT *)lp, ui::kSurface); return TRUE;
	case WM_COMMAND: {
		if (HIWORD(wp) == BN_SETFOCUS) {
			// The focus moved to a button out of view (Tab): bring it in.
			RECT br, rc;
			GetWindowRect((HWND)lp, &br);
			MapWindowPoints(nullptr, hwnd, (POINT *)&br, 2);
			GetClientRect(hwnd, &rc);
			if (br.top < D(8)) Scroll(g_scroll + br.top - D(kHeaderH) / 2);
			else if (br.bottom > rc.bottom - D(8)) Scroll(g_scroll + br.bottom - rc.bottom + D(kCardPad));
			return 0;
		}
		if (HIWORD(wp) != BN_CLICKED) return 0;
		int id = LOWORD(wp) - 600;
		if (id >= 0 && id / kButtonsPerCard < (int)g_cards.size()) Command(g_cards[id / kButtonsPerCard], id % kButtonsPerCard);
		return 0;
	}
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND Create(HWND parent, const Hooks &hooks) {
	g_parent = parent;
	g_hooks = hooks;
	WNDCLASSW wc{};
	wc.lpfnWndProc = PageProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"PspcOnline";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassW(&wc);
	g_page = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Online", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, nullptr,
	                         wc.hInstance, nullptr);
	g_dpi = ui::DpiOf(parent);
	Reload();
	MakeCards();
	SyncToggles();
	return g_page;
}

void Refresh() {
	if (!g_page) return;
	g_dpi = ui::DpiOf(g_page);
	Reload();
	SyncToggles();
	for (Card &c : g_cards) c.shown = servers::Poll(c.kind);
	Layout();
}

void StartAutomatic() {
	Reload();
	std::wstring failed;
	// PSPC Network first: the game server checks its sign-ins.
	for (Kind k : {Kind::Network, Kind::Adhoc, Kind::Sfco, Kind::GameSpy}) {
		if (!AutoStart(g_s, k)) continue;
		std::wstring error;
		if (!servers::Start(k, g_s, k == Kind::Sfco ? GameUmd() : L"", &error)) {
			Card *c = CardOf(k);
			failed += std::wstring(c ? c->title : L"A server") + L": " + error + L"\n\n";
		} else {
			PointGameHere(k);
		}
	}
	if (!failed.empty())
		MessageBoxW(g_parent, (L"Some servers set to start with PSPC didn't start:\n\n" + failed).c_str(), L"PSPC", MB_ICONWARNING);
	Refresh();
	PostMessageW(g_parent, WM_ONLINE_CHANGED, 0, 0);
}

bool ConfirmClose(HWND owner) {
	if (!servers::AnyRunning()) return true;
	std::wstring names;
	for (Card &c : g_cards) {
		State st = servers::Poll(c.kind);
		if (st == State::Running || st == State::Starting) names += std::wstring(L"\n   ") + c.title;
	}
	int r = MessageBoxW(owner,
	                    (L"These servers are running:\n" + names +
	                     L"\n\nClosing PSPC stops them, and anyone playing on them is disconnected. Close anyway?")
	                        .c_str(),
	                    L"PSPC", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
	if (r != IDYES) return false;
	servers::StopAll();
	return true;
}

std::wstring Summary() {
	int running = 0;
	for (int i = 0; i < servers::kKinds; i++) {
		State st = servers::Poll((Kind)i);
		running += st == State::Running || st == State::Starting;
	}
	if (running == 0) return L"Servers and sign-in";
	return running == 1 ? L"1 server running" : std::to_wstring(running) + L" servers running";
}

}  // namespace online
