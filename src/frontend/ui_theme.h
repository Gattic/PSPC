// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc-launcher's look, shared by the main window and its dialogs: a dark palette, DPI scaling,
// fonts (Segoe UI, and Segoe Fluent Icons / MDL2 Assets for glyphs), rounded shapes drawn with
// GDI+, owner-drawn buttons with hover, a dark title bar, and ThemeDialog(), which restyles a
// dialog's standard controls to match.
#pragma once

#include <windows.h>
#include <objidl.h>
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>

namespace ui {

// Palette.
constexpr COLORREF kBg = RGB(16, 16, 21);            // content area
constexpr COLORREF kSidebar = RGB(22, 22, 29);       // sidebar, title bar, dialogs
constexpr COLORREF kSurface = RGB(30, 30, 39);       // cards, inputs
constexpr COLORREF kSurfaceHot = RGB(39, 39, 51);
constexpr COLORREF kSurfaceDown = RGB(48, 48, 63);
constexpr COLORREF kBorder = RGB(52, 52, 67);
constexpr COLORREF kText = RGB(240, 240, 246);
constexpr COLORREF kTextDim = RGB(166, 166, 184);
constexpr COLORREF kTextFaint = RGB(112, 112, 130);
constexpr COLORREF kAccent = RGB(56, 132, 255);
constexpr COLORREF kAccentHot = RGB(86, 154, 255);
constexpr COLORREF kAccentDown = RGB(38, 110, 226);
constexpr COLORREF kGreen = RGB(72, 199, 122);
constexpr COLORREF kAmber = RGB(240, 176, 66);
constexpr COLORREF kInfo = RGB(110, 172, 255);

// Glyphs (Segoe Fluent Icons / Segoe MDL2 Assets).
constexpr const wchar_t *kIconGames = L"", *kIconXmb = L"", *kIconPlugins = L"",
                        *kIconNetwork = L"", *kIconSettings = L"", *kIconRefresh = L"",
                        *kIconFolder = L"", *kIconSearch = L"", *kIconPlay = L"",
                        *kIconBuild = L"", *kIconCode = L"", *kIconOpenFile = L"",
                        *kIconPerson = L"";
// More glyphs, by code point.
constexpr const wchar_t *kIconGlobe = L"", *kIconStop = L"", *kIconDocument = L"", *kIconCopy = L"",
                        *kIconPeople = L"", *kIconWifi = L"", *kIconGamepad = L"", *kIconEdit = L"",
                        *kIconDownload = L"", *kIconInfo = L"";

inline Gdiplus::Color C(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }
inline int Px(int v, int dpi) { return MulDiv(v, dpi, 96); }
inline float Pxf(float v, int dpi) { return v * (float)dpi / 96.0f; }

// Call first thing in WinMain: per-monitor DPI awareness (v2 where available).
void EnableDpiAwareness();
int DpiOf(HWND hwnd);

// Text faces: the UI font and the icon font (whichever this Windows has).
const wchar_t *TextFace();
const wchar_t *IconFace();
// A GDI font of px logical pixels at this DPI (cached; don't delete).
HFONT Font(int px, int dpi, int weight = FW_NORMAL, bool icon = false);

// Dark title bar (and, on Windows 11, a caption colored like the window); harmless elsewhere.
void DarkTitleBar(HWND hwnd, COLORREF caption = kSidebar);

// Rounded rectangles.
void AddRoundRect(Gdiplus::GraphicsPath &path, const Gdiplus::RectF &r, float radius);
void FillRound(Gdiplus::Graphics &g, const Gdiplus::RectF &r, float radius, const Gdiplus::Color &c);
void StrokeRound(Gdiplus::Graphics &g, const Gdiplus::RectF &r, float radius, const Gdiplus::Color &c, float width);
// Text in a GDI+ graphics: the UI or icon face at px pixels; align 0 = left, 1 = center, 2 = right.
void Text(Gdiplus::Graphics &g, const wchar_t *s, const Gdiplus::RectF &r, float px, const Gdiplus::Color &c,
          bool bold = false, int align = 0, bool vcenter = true, bool wrap = false, bool icon = false);
Gdiplus::SizeF Measure(Gdiplus::Graphics &g, const wchar_t *s, float px, bool bold = false, float maxWidth = 10000);

// Owner-drawn buttons (BS_OWNERDRAW). Style and glyph are set once per button; hover is tracked.
// Toggle: a switch and its label (SetChecked / Checked hold the state; the owner flips it on click).
enum class Look { Primary, Secondary, Ghost, Nav, NavActive, Icon, Toggle };
void SetLook(HWND button, Look look, const wchar_t *glyph = nullptr, const wchar_t *detail = nullptr);
void SetDetail(HWND button, const wchar_t *detail);  // a second, dimmer line (Nav look)
void SetChecked(HWND button, bool on);
bool Checked(HWND button);
void DrawOwnerButton(const DRAWITEMSTRUCT *di, COLORREF background);
bool IsHot(HWND button);

// Restyles a dialog's standard controls to the launcher's dark look (call at the end of
// WM_INITDIALOG, after the controls are created).
void ThemeDialog(HWND dlg);

}  // namespace ui
