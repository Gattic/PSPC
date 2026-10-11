// SPDX-License-Identifier: GPL-2.0-or-later
//
// PPGe: what the sceUtility dialogs and the HOME menu draw, as the PSP draws them over the game
// image. The drawing API (rectangles, text in the system font, button symbols, images, scissor)
// and the text layout rules are PPSSPP's PPGeDraw (Core/Util/PPGeDraw.cpp/.h, Copyright (c)
// 2012- PPSSPP Project, GPL-2.0-or-later), so the dialog layouts port as they are.
//
// How it draws: a dialog builds a frame between Begin() and End() during its Update, as the PSP's
// dialogs (and PPGe) do; End() makes it the frame of that layer. The layers are drawn over every
// image sceDisplay presents (and --dump-frames writes), in the image's own resolution, not into the
// game's framebuffer in guest memory. So both GE renderers show it the same way (the software
// renderer's VRAM image and the D3D11 renderer's upscaled one), the game's memory and the GE
// never see it (no GPU readbacks, nothing for a game that reads its framebuffer back, nothing
// piling up in games that don't redraw every frame), and the HOME menu, which has no Update, uses
// the same path. A dialog frame stays up for a few VBlanks after its Update, as long as a game
// calling Update at 10 frames/s or more needs.
//
// Text comes from the firmware's PGF fonts in flash0:/font (jpn0, then ltn0 and kr0 for what it
// lacks), through font_pgf; without them, from a Windows font.
//
// Coordinates are PSP screen pixels (480x272), floats; colors are PSP ABGR (0xAABBGGRR).
// Everything here runs on the emulation thread.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/common.h"

namespace ppge {

// Text box alignment (PPGeAlign): the point given is the box's left/right/center and top/bottom/
// middle.
using Align = u32;
constexpr Align BOX_LEFT = 0x00;
constexpr Align BOX_RIGHT = 0x01;
constexpr Align BOX_HCENTER = 0x02;
constexpr Align BOX_TOP = 0x00;
constexpr Align BOX_BOTTOM = 0x10;
constexpr Align BOX_VCENTER = 0x20;
constexpr Align BOX_CENTER = 0x22;
constexpr Align ALIGN_ANY = 0xFF;

// Line breaking (PPGE_LINE_*).
enum : int {
	LINE_NONE = 0,
	LINE_USE_ELLIPSIS = 1,  // "..." in place of what doesn't fit
	LINE_WRAP_WORD = 2,
	LINE_WRAP_CHAR = 4,
};

struct Style {
	Align align = BOX_LEFT;
	float scale = 1.0f;  // PPGe's text scale: the dialogs' usual 0.55 is the PSP's dialog text size
	u32 color = 0xFFFFFFFF;
	bool hasShadow = false;
	u32 shadowColor = 0x80000000;
};

// The PSP's button symbols, and the banner's mark.
enum class Icon { Cross, Circle, Triangle, Square, Hexagon };

// An RGBA8888 image (save data icons), straight alpha.
struct Texture {
	int width = 0, height = 0;
	std::vector<u32> pixels;
};
// Decodes a PNG (or another format Windows reads). Null if it can't.
std::shared_ptr<const Texture> DecodeTexture(const u8 *data, size_t size);

// Layers, bottom to top: the sceUtility dialog, and the system's own (HOME menu).
enum class Layer { Dialog = 0, System = 1 };

// Whether anything is drawn at all: with a window, or headless with PSPC_HEADLESS_OVERLAYS=1 set
// (a debugging aid for frame dumps; the dialogs are then interactive, driven by --press).
bool Enabled();

// A frame: the draw calls between Begin and End replace what the layer shows.
void Begin(Layer layer = Layer::Dialog);
void End();
// The layer shows nothing any more.
void Clear(Layer layer);

void DrawRect(float x1, float y1, float x2, float y2, u32 color);
void DrawString(std::string_view text, float x, float y, const Style &style);
// Wrapped at wrapWidth; with a wrapHeight, scaled down (and cut) to fit it as PPGe does.
void DrawStringWrapped(std::string_view text, float x, float y, float wrapWidth, float wrapHeight, const Style &style);
// The symbol fills (x, y, w, h); style.color and style.hasShadow apply.
void DrawIcon(Icon icon, float x, float y, float w, float h, const Style &style);
void DrawTexture(const std::shared_ptr<const Texture> &tex, float x, float y, float w, float h, u32 color);
// x2/y2 exclusive.
void Scissor(int x1, int y1, int x2, int y2);
void ScissorReset();

// The size of the text's box (PPGeMeasureText).
void MeasureString(float *w, float *h, std::string_view text, float scale, int wrapType = LINE_NONE, float wrapWidth = 0);

// Draws the visible layers over a presented image: RGBA8888 (R in the low byte), width x height
// at any multiple of 480x272. The dialog layer belongs to the game's screen: without `gameScreen`
// (a system or plugin screen is shown instead, display priority 0/1) only the system layer is
// drawn. Over a transparent image (all 0), the result is the layers alone with premultiplied alpha,
// for a GPU renderer to lay over its own picture.
void Composite(u32 *rgba, int width, int height, bool gameScreen = true);
// Whether a layer would be drawn now.
bool Visible(Layer layer);

// UTF-16 (OSK text) to UTF-8.
std::string Utf16ToUtf8(const std::u16string &s);

}  // namespace ppge
