// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontend/ui_theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")

namespace ui {

using namespace Gdiplus;

void EnableDpiAwareness() {
	using SetContext = BOOL(WINAPI *)(HANDLE);
	auto set = (SetContext)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
	if (set && set((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) return;
	SetProcessDPIAware();
}

int DpiOf(HWND hwnd) {
	using GetDpi = UINT(WINAPI *)(HWND);
	static auto get = (GetDpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
	if (get && hwnd) {
		UINT d = get(hwnd);
		if (d) return (int)d;
	}
	HDC dc = GetDC(nullptr);
	int d = GetDeviceCaps(dc, LOGPIXELSX);
	ReleaseDC(nullptr, dc);
	return d > 0 ? d : 96;
}

namespace {

bool FontInstalled(const wchar_t *face) {
	LOGFONTW lf{};
	lf.lfCharSet = DEFAULT_CHARSET;
	wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
	bool found = false;
	HDC dc = GetDC(nullptr);
	EnumFontFamiliesExW(
		dc, &lf, [](const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM p) -> int { *(bool *)p = true; return 0; },
		(LPARAM)&found, 0);
	ReleaseDC(nullptr, dc);
	return found;
}

// GDI+ objects live until the process ends (destroying them after GdiplusShutdown would crash).
FontFamily *Family(bool bold, bool icon) {
	static FontFamily *text = nullptr, *semibold = nullptr, *glyphs = nullptr;
	if (icon) return glyphs ? glyphs : (glyphs = new FontFamily(IconFace()));
	if (bold) {
		if (!semibold) {
			semibold = new FontFamily(L"Segoe UI Semibold");
			if (semibold->GetLastStatus() != Ok) {
				delete semibold;
				semibold = new FontFamily(TextFace());
			}
		}
		return semibold;
	}
	return text ? text : (text = new FontFamily(TextFace()));
}

void SetupFormat(StringFormat &f, int align, bool vcenter, bool wrap) {
	f.SetAlignment(align == 1 ? StringAlignmentCenter : align == 2 ? StringAlignmentFar : StringAlignmentNear);
	f.SetLineAlignment(vcenter ? StringAlignmentCenter : StringAlignmentNear);
	f.SetTrimming(StringTrimmingEllipsisCharacter);
	f.SetFormatFlags(wrap ? StringFormatFlagsLineLimit : StringFormatFlagsNoWrap);
}

struct ButtonInfo {
	Look look = Look::Secondary;
	std::wstring glyph, detail;
	bool hot = false;
	bool on = false;  // Toggle
};
std::map<HWND, ButtonInfo> g_buttons;

LRESULT CALLBACK ButtonSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
	switch (msg) {
	case WM_MOUSEMOVE: {
		ButtonInfo &b = g_buttons[hwnd];
		if (!b.hot) {
			b.hot = true;
			TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
			TrackMouseEvent(&t);
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		break;
	}
	case WM_MOUSELEAVE:
		g_buttons[hwnd].hot = false;
		InvalidateRect(hwnd, nullptr, FALSE);
		break;
	case WM_ERASEBKGND: return 1;  // painted whole in WM_DRAWITEM
	case WM_NCDESTROY:
		RemoveWindowSubclass(hwnd, ButtonSubclass, 0);
		g_buttons.erase(hwnd);
		break;
	}
	return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

const wchar_t *TextFace() { return L"Segoe UI"; }

const wchar_t *IconFace() {
	static const wchar_t *face = FontInstalled(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
	return face;
}

HFONT Font(int px, int dpi, int weight, bool icon) {
	struct Entry {
		int px, dpi, weight;
		bool icon;
		HFONT font;
	};
	static std::vector<Entry> cache;
	for (const Entry &e : cache)
		if (e.px == px && e.dpi == dpi && e.weight == weight && e.icon == icon) return e.font;
	LOGFONTW lf{};
	lf.lfHeight = -Px(px, dpi);
	lf.lfWeight = weight;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfQuality = CLEARTYPE_QUALITY;
	wcsncpy_s(lf.lfFaceName, icon ? IconFace() : TextFace(), _TRUNCATE);
	HFONT f = CreateFontIndirectW(&lf);
	cache.push_back({px, dpi, weight, icon, f});
	return f;
}

void DarkTitleBar(HWND hwnd, COLORREF caption) {
	BOOL dark = TRUE;
	if (FAILED(DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark))))
		DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));  // Windows 10 before 20H1
	COLORREF text = kText;
	DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &caption, sizeof(caption));
	DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
}

void AddRoundRect(GraphicsPath &path, const RectF &r, float radius) {
	float d = std::min(radius * 2, std::min(r.Width, r.Height));
	if (d <= 0.5f) {
		path.AddRectangle(r);
		return;
	}
	path.AddArc(r.X, r.Y, d, d, 180, 90);
	path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
	path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
	path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
	path.CloseFigure();
}

void FillRound(Graphics &g, const RectF &r, float radius, const Color &c) {
	GraphicsPath p;
	AddRoundRect(p, r, radius);
	SolidBrush b(c);
	g.FillPath(&b, &p);
}

void StrokeRound(Graphics &g, const RectF &r, float radius, const Color &c, float width) {
	GraphicsPath p;
	RectF in(r.X + width / 2, r.Y + width / 2, r.Width - width, r.Height - width);
	AddRoundRect(p, in, radius);
	Pen pen(c, width);
	g.DrawPath(&pen, &p);
}

void Text(Graphics &g, const wchar_t *s, const RectF &r, float px, const Color &c, bool bold, int align, bool vcenter,
          bool wrap, bool icon) {
	Gdiplus::Font f(Family(bold, icon), px, FontStyleRegular, UnitPixel);
	StringFormat fmt;
	SetupFormat(fmt, align, vcenter, wrap);
	SolidBrush b(c);
	g.DrawString(s, -1, &f, r, &fmt, &b);
}

SizeF Measure(Graphics &g, const wchar_t *s, float px, bool bold, float maxWidth) {
	Gdiplus::Font f(Family(bold, false), px, FontStyleRegular, UnitPixel);
	StringFormat fmt;
	SetupFormat(fmt, 0, false, maxWidth < 9999);
	RectF box;
	g.MeasureString(s, -1, &f, RectF(0, 0, maxWidth, 10000), &fmt, &box);
	return SizeF(box.Width, box.Height);
}

void SetLook(HWND button, Look look, const wchar_t *glyph, const wchar_t *detail) {
	bool fresh = g_buttons.find(button) == g_buttons.end();
	ButtonInfo &b = g_buttons[button];
	b.look = look;
	b.glyph = glyph ? glyph : L"";
	if (detail) b.detail = detail;
	if (fresh) SetWindowSubclass(button, ButtonSubclass, 0, 0);
	InvalidateRect(button, nullptr, FALSE);
}

void SetDetail(HWND button, const wchar_t *detail) {
	auto it = g_buttons.find(button);
	if (it == g_buttons.end() || it->second.detail == (detail ? detail : L"")) return;
	it->second.detail = detail ? detail : L"";
	InvalidateRect(button, nullptr, FALSE);
}

void SetChecked(HWND button, bool on) {
	auto it = g_buttons.find(button);
	if (it == g_buttons.end() || it->second.on == on) return;
	it->second.on = on;
	InvalidateRect(button, nullptr, FALSE);
}

bool Checked(HWND button) {
	auto it = g_buttons.find(button);
	return it != g_buttons.end() && it->second.on;
}

bool IsHot(HWND button) {
	auto it = g_buttons.find(button);
	return it != g_buttons.end() && it->second.hot;
}

void DrawOwnerButton(const DRAWITEMSTRUCT *di, COLORREF background) {
	auto it = g_buttons.find(di->hwndItem);
	ButtonInfo info = it != g_buttons.end() ? it->second : ButtonInfo{};
	int w = di->rcItem.right - di->rcItem.left, h = di->rcItem.bottom - di->rcItem.top;
	if (w <= 0 || h <= 0) return;
	int dpi = DpiOf(di->hwndItem);
	bool down = di->itemState & ODS_SELECTED, disabled = di->itemState & ODS_DISABLED;
	bool focus = (di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT);
	bool hot = info.hot && !disabled;
	wchar_t label[128];
	GetWindowTextW(di->hwndItem, label, 128);

	Bitmap buffer(w, h, PixelFormat32bppPARGB);
	Graphics g(&buffer);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	g.Clear(C(background));
	RectF box(0, 0, (REAL)w, (REAL)h);
	float radius = Pxf(6, dpi);
	Color text = C(disabled ? kTextFaint : kText);
	switch (info.look) {
	case Look::Primary:
		FillRound(g, box, radius, C(disabled ? kSurface : down ? kAccentDown : hot ? kAccentHot : kAccent));
		if (!disabled) text = C(RGB(255, 255, 255));
		break;
	case Look::Secondary:
		FillRound(g, box, radius, C(disabled ? kSurface : down ? kSurfaceDown : hot ? kSurfaceHot : kSurface));
		StrokeRound(g, box, radius, C(kBorder), 1);
		break;
	case Look::Ghost:
	case Look::Icon:
		if (hot || down) FillRound(g, box, radius, C(down ? kSurfaceDown : kSurfaceHot));
		if (!hot && !down && !disabled) text = C(kTextDim);
		break;
	case Look::Nav:
		if (hot || down) FillRound(g, box, radius, C(down ? kSurfaceDown : kSurfaceHot));
		if (!hot && !down && !disabled) text = C(kTextDim);
		break;
	case Look::NavActive: {
		FillRound(g, box, radius, C(kSurfaceHot));
		SolidBrush bar(C(kAccent));
		float bh = (float)h * 0.5f;
		g.FillRectangle(&bar, RectF(0, ((float)h - bh) / 2, Pxf(3, dpi), bh));
		break;
	}
	case Look::Toggle: {
		// A switch on the left, the label after it.
		float sh = Pxf(18, dpi), sw = Pxf(32, dpi), sy = ((float)h - sh) / 2;
		RectF track(Pxf(2, dpi), sy, sw, sh);
		COLORREF fill = info.on ? (disabled ? kSurfaceDown : hot ? kAccentHot : kAccent) : (hot ? kSurfaceDown : kSurfaceHot);
		FillRound(g, track, sh / 2, C(fill));
		if (!info.on) StrokeRound(g, track, sh / 2, C(kBorder), 1);
		float knob = sh - Pxf(6, dpi);
		SolidBrush k(C(info.on ? RGB(255, 255, 255) : kTextDim));
		g.FillEllipse(&k, RectF(info.on ? track.GetRight() - knob - Pxf(3, dpi) : track.X + Pxf(3, dpi), sy + Pxf(3, dpi), knob, knob));
		float textPx = Pxf(13, dpi), x = track.GetRight() + Pxf(10, dpi);
		Text(g, label, RectF(x, 0, (REAL)w - x, (REAL)h), textPx, C(disabled ? kTextFaint : info.on ? kText : kTextDim));
		if (focus) StrokeRound(g, RectF(1, 1, (REAL)w - 2, (REAL)h - 2), radius, C(kAccentHot), Pxf(1.5f, dpi));
		Graphics screen(di->hDC);
		screen.DrawImage(&buffer, (INT)di->rcItem.left, (INT)di->rcItem.top);
		return;
	}
	}

	// Text the size of the button's font when it has one (dialogs), else 14 px.
	float textPx = Pxf(14, dpi);
	if (HFONT f = (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0)) {
		LOGFONTW lf{};
		if (GetObjectW(f, sizeof(lf), &lf) && lf.lfHeight) textPx = (float)std::abs(lf.lfHeight);
	}
	const float glyphPx = Pxf(16, dpi), gap = Pxf(10, dpi);
	bool bold = info.look == Look::Primary || info.look == Look::NavActive;
	bool left = info.look == Look::Nav || info.look == Look::NavActive;
	Color glyphColor = info.look == Look::NavActive ? C(kAccentHot) : text;
	if (info.look == Look::Icon) {
		Text(g, info.glyph.c_str(), box, glyphPx, text, false, 1, true, false, true);
	} else if (left) {
		float x = Pxf(14, dpi);
		if (!info.glyph.empty()) {
			Text(g, info.glyph.c_str(), RectF(x, 0, glyphPx * 1.4f, (REAL)h), glyphPx, glyphColor, false, 0, true, false, true);
			x += glyphPx + gap;
		}
		if (info.detail.empty()) {
			Text(g, label, RectF(x, 0, (REAL)w - x - Pxf(8, dpi), (REAL)h), textPx, text, bold);
		} else {
			float lh = (float)h / 2;
			Text(g, label, RectF(x, lh - Pxf(18, dpi), (REAL)w - x - Pxf(8, dpi), Pxf(18, dpi)), textPx, text, bold, 0, true);
			Text(g, info.detail.c_str(), RectF(x, lh, (REAL)w - x - Pxf(8, dpi), Pxf(16, dpi)), Pxf(11.5f, dpi),
			     C(kTextFaint), false, 0, true);
		}
	} else {
		SizeF ts = Measure(g, label, textPx, bold);
		float total = ts.Width + (info.glyph.empty() ? 0 : glyphPx + gap * 0.7f);
		float x = std::max(Pxf(8, dpi), ((float)w - total) / 2);
		if (!info.glyph.empty()) {
			Text(g, info.glyph.c_str(), RectF(x, 0, glyphPx * 1.3f, (REAL)h), glyphPx, text, false, 0, true, false, true);
			x += glyphPx + gap * 0.7f;
		}
		Text(g, label, RectF(x, 0, (REAL)w - x, (REAL)h), textPx, text, bold);
	}
	if (focus) StrokeRound(g, RectF(1, 1, (REAL)w - 2, (REAL)h - 2), radius, C(kAccentHot), Pxf(1.5f, dpi));

	Graphics screen(di->hDC);
	screen.DrawImage(&buffer, (INT)di->rcItem.left, (INT)di->rcItem.top);
}

// ---- dialogs: standard controls in the dark look

namespace {

HBRUSH Brush(COLORREF c) {
	static std::map<COLORREF, HBRUSH> brushes;
	HBRUSH &b = brushes[c];
	if (!b) b = CreateSolidBrush(c);
	return b;
}

bool ClassIs(HWND hwnd, const wchar_t *cls) {
	wchar_t name[64];
	GetClassNameW(hwnd, name, 64);
	return _wcsicmp(name, cls) == 0;
}

LONG_PTR ButtonType(HWND hwnd) { return GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK; }

// Check boxes and radio buttons, drawn whole (themed ones ignore text colors).
LRESULT DrawCheck(NMCUSTOMDRAW *cd) {
	if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
	HWND b = cd->hdr.hwndFrom;
	LONG_PTR type = ButtonType(b);
	bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
	bool checked = SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED, enabled = IsWindowEnabled(b) != FALSE;
	bool hot = (cd->uItemState & CDIS_HOT) != 0;
	bool focus = (cd->uItemState & CDIS_FOCUS) && (cd->uItemState & CDIS_SHOWKEYBOARDCUES);
	RECT rc = cd->rc;
	int w = rc.right - rc.left, h = rc.bottom - rc.top, dpi = DpiOf(b);
	Bitmap buffer(std::max(1, w), std::max(1, h), PixelFormat32bppPARGB);
	Graphics g(&buffer);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.Clear(C(kSidebar));
	float box = Pxf(16, dpi), y = ((float)h - box) / 2;
	RectF r(Pxf(1, dpi), y, box, box);
	if (radio) {
		Pen ring(C(checked ? kAccent : hot ? kTextDim : kTextFaint), Pxf(1.5f, dpi));
		SolidBrush fill(C(checked ? kAccent : kSurface));
		g.FillEllipse(&fill, r);
		if (!checked) g.DrawEllipse(&ring, r);
		if (checked) {
			SolidBrush dot(C(RGB(255, 255, 255)));
			float d = box * 0.4f;
			g.FillEllipse(&dot, RectF(r.X + (box - d) / 2, r.Y + (box - d) / 2, d, d));
		}
	} else {
		FillRound(g, r, Pxf(4, dpi), C(checked ? (enabled ? kAccent : kTextFaint) : kSurface));
		if (!checked) StrokeRound(g, r, Pxf(4, dpi), C(hot ? kTextDim : kTextFaint), Pxf(1.2f, dpi));
		if (checked) {
			Pen tick(C(RGB(255, 255, 255)), Pxf(1.8f, dpi));
			tick.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
			tick.SetLineJoin(LineJoinRound);
			PointF pts[] = {{r.X + box * 0.24f, r.Y + box * 0.52f}, {r.X + box * 0.43f, r.Y + box * 0.7f}, {r.X + box * 0.77f, r.Y + box * 0.3f}};
			g.DrawLines(&tick, pts, 3);
		}
	}
	if (focus) StrokeRound(g, RectF(r.X - Pxf(2, dpi), r.Y - Pxf(2, dpi), box + Pxf(4, dpi), box + Pxf(4, dpi)), Pxf(5, dpi), C(kAccentHot), 1);
	Graphics screen(cd->hdc);
	screen.DrawImage(&buffer, (INT)rc.left, (INT)rc.top);
	wchar_t text[256];
	GetWindowTextW(b, text, 256);
	RECT tr = rc;
	tr.left += (int)(box + Pxf(8, dpi));
	SetBkMode(cd->hdc, TRANSPARENT);
	SetTextColor(cd->hdc, enabled ? kText : kTextFaint);
	HGDIOBJ old = SelectObject(cd->hdc, (HFONT)SendMessageW(b, WM_GETFONT, 0, 0));
	DrawTextW(cd->hdc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	SelectObject(cd->hdc, old);
	return CDRF_SKIPDEFAULT;
}

// Trackbars: an accent track up to a round thumb.
LRESULT DrawTrackbar(NMCUSTOMDRAW *cd) {
	if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
	if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
	HWND tb = cd->hdr.hwndFrom;
	int dpi = DpiOf(tb);
	Graphics g(cd->hdc);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	if (cd->dwItemSpec == TBCD_CHANNEL) {
		RECT client, thumb;
		GetClientRect(tb, &client);
		SendMessageW(tb, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
		SolidBrush bg(C(kSidebar));
		g.FillRectangle(&bg, RectF(0, 0, (REAL)client.right, (REAL)client.bottom));
		float cy = (float)(thumb.top + thumb.bottom) / 2, th = Pxf(4, dpi);
		float x0 = (float)cd->rc.left, x1 = (float)cd->rc.right, tx = (float)(thumb.left + thumb.right) / 2;
		FillRound(g, RectF(x0, cy - th / 2, x1 - x0, th), th / 2, C(kSurfaceDown));
		FillRound(g, RectF(x0, cy - th / 2, std::max(0.0f, tx - x0), th), th / 2, C(kAccent));
		return CDRF_SKIPDEFAULT;
	}
	if (cd->dwItemSpec == TBCD_THUMB) {
		float cx = (float)(cd->rc.left + cd->rc.right) / 2, cy = (float)(cd->rc.top + cd->rc.bottom) / 2, d = Pxf(16, dpi);
		SolidBrush ring(C(kText)), core(C(kAccent));
		g.FillEllipse(&ring, RectF(cx - d / 2, cy - d / 2, d, d));
		g.FillEllipse(&core, RectF(cx - d * 0.28f, cy - d * 0.28f, d * 0.56f, d * 0.56f));
		return CDRF_SKIPDEFAULT;
	}
	return CDRF_SKIPDEFAULT;  // tics
}

// Group boxes: a rounded frame with the caption set into it (frame only: siblings sit inside).
LRESULT CALLBACK GroupSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
	if (msg == WM_PAINT) {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT rc;
		GetClientRect(hwnd, &rc);
		int dpi = DpiOf(hwnd);
		HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
		HGDIOBJ old = SelectObject(dc, font);
		wchar_t text[128];
		GetWindowTextW(hwnd, text, 128);
		SIZE ts{};
		GetTextExtentPoint32W(dc, text, (int)wcslen(text), &ts);
		{
			Graphics g(dc);
			g.SetSmoothingMode(SmoothingModeAntiAlias);
			float top = (float)ts.cy / 2;
			StrokeRound(g, RectF(0, top, (REAL)rc.right, (REAL)rc.bottom - top), Pxf(8, dpi), C(kBorder), 1);
		}
		if (text[0]) {
			RECT lr{Px(8, dpi), 0, Px(8, dpi) + ts.cx + Px(8, dpi), ts.cy};
			FillRect(dc, &lr, Brush(kSidebar));
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, kTextDim);
			lr.left += Px(4, dpi);
			DrawTextW(dc, text, -1, &lr, DT_LEFT | DT_SINGLELINE);
		}
		SelectObject(dc, old);
		EndPaint(hwnd, &ps);
		return 0;
	}
	if (msg == WM_ERASEBKGND) return 1;
	if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, GroupSubclass, 0);
	return DefSubclassProc(hwnd, msg, wp, lp);
}

// A disabled list view paints a light background whatever its colors: paint it dark, its items dimmed.
void PaintDisabledList(HWND hwnd) {
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(hwnd, &ps);
	RECT rc;
	GetClientRect(hwnd, &rc);
	FillRect(dc, &rc, Brush(kSurface));
	HWND header = ListView_GetHeader(hwnd);
	int columns = header ? Header_GetItemCount(header) : 1;
	HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, kTextFaint);
	int count = ListView_GetItemCount(hwnd);
	for (int i = std::max(0, ListView_GetTopIndex(hwnd)); i < count; i++) {
		for (int c = 0; c < std::max(1, columns); c++) {
			RECT r{};
			if (!ListView_GetSubItemRect(hwnd, i, c, LVIR_LABEL, &r)) continue;
			if (r.top > rc.bottom) break;
			wchar_t text[256] = {};
			ListView_GetItemText(hwnd, i, c, text, 256);
			r.left += Px(6, DpiOf(hwnd));
			DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		}
	}
	SelectObject(dc, old);
	EndPaint(hwnd, &ps);
}

// List views keep their header's text light (the header talks to the list, not the dialog).
LRESULT CALLBACK ListSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
	if (msg == WM_PAINT && !IsWindowEnabled(hwnd)) {
		PaintDisabledList(hwnd);
		return 0;
	}
	if (msg == WM_ERASEBKGND && !IsWindowEnabled(hwnd)) return 1;
	if (msg == WM_ENABLE) InvalidateRect(hwnd, nullptr, TRUE);
	if (msg == WM_NOTIFY && ((NMHDR *)lp)->code == NM_CUSTOMDRAW &&
	    ((NMHDR *)lp)->hwndFrom == ListView_GetHeader(hwnd)) {
		auto *cd = (NMCUSTOMDRAW *)lp;
		if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
		if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
			SetTextColor(cd->hdc, kTextDim);
			return CDRF_DODEFAULT;
		}
	}
	if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ListSubclass, 0);
	return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DialogSubclass(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
	switch (msg) {
	case WM_CTLCOLORDLG: return (LRESULT)Brush(kSidebar);
	case WM_CTLCOLORSTATIC: {
		HWND c = (HWND)lp;
		HDC dc = (HDC)wp;
		SetBkColor(dc, kSidebar);
		if (ClassIs(c, L"Edit")) {  // read-only or disabled edits
			SetTextColor(dc, kTextFaint);
			SetBkColor(dc, kSurface);
			return (LRESULT)Brush(kSurface);
		}
		SetTextColor(dc, !IsWindowEnabled(c) ? kTextFaint : GetWindowTextLengthW(c) > 60 ? kTextDim : kText);
		return (LRESULT)Brush(kSidebar);
	}
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
		SetTextColor((HDC)wp, kText);
		SetBkColor((HDC)wp, kSurface);
		return (LRESULT)Brush(kSurface);
	case WM_CTLCOLORBTN: return (LRESULT)Brush(kSidebar);
	case WM_DRAWITEM: {
		auto *di = (const DRAWITEMSTRUCT *)lp;
		if (di->CtlType == ODT_BUTTON && g_buttons.count(di->hwndItem)) {
			DrawOwnerButton(di, kSidebar);
			return TRUE;
		}
		break;
	}
	case WM_NOTIFY: {
		auto *nm = (NMHDR *)lp;
		if (nm->code == NM_CUSTOMDRAW) {
			if (ClassIs(nm->hwndFrom, L"Button")) {
				LONG_PTR t = ButtonType(nm->hwndFrom);
				if (t == BS_CHECKBOX || t == BS_AUTOCHECKBOX || t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON ||
				    t == BS_3STATE || t == BS_AUTO3STATE)
					return DrawCheck((NMCUSTOMDRAW *)lp);
			}
			if (ClassIs(nm->hwndFrom, TRACKBAR_CLASSW)) return DrawTrackbar((NMCUSTOMDRAW *)lp);
		}
		break;
	}
	case WM_NCDESTROY: RemoveWindowSubclass(dlg, DialogSubclass, 0); break;
	}
	return DefSubclassProc(dlg, msg, wp, lp);
}

BOOL CALLBACK ThemeChild(HWND c, LPARAM) {
	if (ClassIs(c, L"Button")) {
		LONG_PTR type = ButtonType(c);
		if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
			LONG_PTR style = GetWindowLongPtrW(c, GWL_STYLE);
			SetWindowLongPtrW(c, GWL_STYLE, (style & ~BS_TYPEMASK) | BS_OWNERDRAW);
			SetLook(c, GetDlgCtrlID(c) == IDOK ? Look::Primary : Look::Secondary);
		} else if (type == BS_GROUPBOX) {
			SetWindowSubclass(c, GroupSubclass, 0, 0);
		}
	} else if (ClassIs(c, L"Edit") || ClassIs(c, L"ComboBox")) {
		SetWindowTheme(c, L"DarkMode_CFD", nullptr);
	} else if (ClassIs(c, WC_LISTVIEWW)) {
		SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
		ListView_SetBkColor(c, kSurface);
		ListView_SetTextBkColor(c, kSurface);
		ListView_SetTextColor(c, kText);
		if (HWND header = ListView_GetHeader(c)) SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
		SetWindowSubclass(c, ListSubclass, 0, 0);
	}
	InvalidateRect(c, nullptr, TRUE);
	return TRUE;
}

}  // namespace

void ThemeDialog(HWND dlg) {
	DarkTitleBar(dlg);
	SetWindowSubclass(dlg, DialogSubclass, 0, 0);
	EnumChildWindows(dlg, ThemeChild, 0);
	InvalidateRect(dlg, nullptr, TRUE);
}

}  // namespace ui
