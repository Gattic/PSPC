// SPDX-License-Identifier: GPL-2.0-or-later
// The text layout (BreakLines, the wrapped text scaling) is ported from PPSSPP
// (Core/Util/PPGeDraw.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later. See ppge.h.
#include "runtime/dialogs/ppge.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "common/log.h"
#include "media/jpeg_decoder.h"
#include "runtime/hle/display.h"
#include "runtime/hle/modules/font_pgf.h"
#include "runtime/host/host.h"
#include "runtime/io/io.h"

#ifndef _WIN32
// fontconfig exposes POSIX stat macros; include it after the PSP's stat layout declarations.
#include <fontconfig/fontconfig.h>
#endif

namespace ppge {

namespace {

// A dialog frame stays visible this many VBlanks after the Update that drew it.
constexpr u32 kDialogFrameVblanks = 6;

// PPGe's text scale -> pixels of the system font (18 px em). PPGe's own font is 24 px at scale 1;
// the system font is a little wider, so this keeps PPGe's layouts: at the dialogs' usual 0.55 the
// text is about 13 px.
constexpr float kTextScale = 1.25f;
// Line spacing, from the font's ascender + descender.
constexpr float kLineSpacing = 1.15f;

// ---------------------------------------------------------------- glyphs

struct GlyphImg {
	int w = 0, h = 0;
	float left = 0, top = 0, adv = 0;  // native pixels
	std::vector<u8> alpha;            // w * h, 0-255
};

// Characters from a Windows font: the fallback when the firmware fonts aren't installed, and for
// the few characters they lack.
#ifdef _WIN32
class GdiFont {
public:
	~GdiFont() {
		if (font_) DeleteObject(font_);
		if (dc_) DeleteDC(dc_);
	}
	bool Init(int emPx) {
		dc_ = CreateCompatibleDC(nullptr);
		if (!dc_) return false;
		font_ = CreateFontW(-emPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
		                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
		if (!font_) return false;
		SelectObject(dc_, font_);
		TEXTMETRICW tm{};
		GetTextMetricsW(dc_, &tm);
		ascent_ = (float)tm.tmAscent;
		descent_ = (float)tm.tmDescent;
		return true;
	}
	bool Valid() const { return font_ != nullptr; }
	float Ascent() const { return ascent_; }
	float Descent() const { return descent_; }
	bool Render(u32 code, GlyphImg &out) {
		if (!font_ || code > 0xFFFF) return false;
		WORD index = 0;
		wchar_t ch = (wchar_t)code;
		if (GetGlyphIndicesW(dc_, &ch, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || index == 0xFFFF)
			return false;
		static const MAT2 kIdentity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
		GLYPHMETRICS gm{};
		DWORD size = GetGlyphOutlineW(dc_, code, GGO_GRAY4_BITMAP, &gm, 0, nullptr, &kIdentity);
		if (size == GDI_ERROR) return false;
		out = GlyphImg{};
		out.adv = (float)gm.gmCellIncX;
		if (size == 0) return true;  // a space
		std::vector<u8> buf(size);
		if (GetGlyphOutlineW(dc_, code, GGO_GRAY4_BITMAP, &gm, size, buf.data(), &kIdentity) == GDI_ERROR) return false;
		const int w = (int)gm.gmBlackBoxX, h = (int)gm.gmBlackBoxY, pitch = (w + 3) & ~3;
		if ((size_t)pitch * h > buf.size()) return true;
		out.w = w;
		out.h = h;
		out.left = (float)gm.gmptGlyphOrigin.x;
		out.top = (float)gm.gmptGlyphOrigin.y;
		out.alpha.resize((size_t)w * h);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) out.alpha[(size_t)y * w + x] = (u8)std::min(255, buf[(size_t)y * pitch + x] * 255 / 16);
		return true;
	}

private:
	HDC dc_ = nullptr;
	HFONT font_ = nullptr;
	float ascent_ = 15, descent_ = 4;
};

#else
class GdiFont {
public:
	~GdiFont() {
		if (face_) FT_Done_Face(face_);
		if (library_) FT_Done_FreeType(library_);
	}
	bool Init(int emPx) {
		if (FT_Init_FreeType(&library_)) return false;
		FcPattern *pattern = FcNameParse((const FcChar8 *)"sans:weight=demibold");
		if (!pattern) return false;
		FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
		FcDefaultSubstitute(pattern);
		FcResult result;
		FcPattern *match = FcFontMatch(nullptr, pattern, &result);
		FcPatternDestroy(pattern);
		if (!match) return false;
		FcChar8 *file = nullptr;
		int index = 0;
		FcPatternGetInteger(match, FC_INDEX, 0, &index);
		bool ok = FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch &&
		          FT_New_Face(library_, (const char *)file, index, &face_) == 0;
		FcPatternDestroy(match);
		return ok && FT_Set_Pixel_Sizes(face_, 0, emPx) == 0;
	}
	bool Valid() const { return face_ != nullptr; }
	float Ascent() const { return face_ ? face_->size->metrics.ascender / 64.0f : 15; }
	float Descent() const { return face_ ? -face_->size->metrics.descender / 64.0f : 4; }
	bool Render(u32 code, GlyphImg &out) {
		if (!face_ || !FT_Get_Char_Index(face_, code) || FT_Load_Char(face_, code, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) return false;
		auto *g = face_->glyph;
		out = {};
		out.w = g->bitmap.width; out.h = g->bitmap.rows;
		out.left = g->bitmap_left; out.top = g->bitmap_top; out.adv = g->advance.x / 64.0f;
		out.alpha.resize((size_t)out.w * out.h);
		for (int y = 0; y < out.h; ++y) {
			const u8 *row = g->bitmap.buffer + (g->bitmap.pitch < 0 ? out.h - 1 - y : y) * std::abs(g->bitmap.pitch);
			for (int x = 0; x < out.w; ++x)
				out.alpha[(size_t)y * out.w + x] = g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO ? ((row[x / 8] >> (7 - x % 8)) & 1) * 255 : row[x];
		}
		return true;
	}
private:
	FT_Library library_ = nullptr;
	FT_Face face_ = nullptr;
};
#endif

class FontSet {
public:
	const GlyphImg *Get(u32 code) {
		Load();
		auto it = cache_.find(code);
		if (it != cache_.end()) return it->second.get();
		auto g = std::make_unique<GlyphImg>();
		if (!Render(code, *g)) {
			// What PPGe does for characters it lacks.
			if (code == '?' || !Render('?', *g)) *g = GlyphImg{};
		}
		return (cache_[code] = std::move(g)).get();
	}
	float Ascend() {
		Load();
		return ascend_;
	}
	float Height() {
		Load();
		return height_;
	}

private:
	void Load() {
		if (loaded_) return;
		loaded_ = true;
		for (const char *name : {"jpn0.pgf", "ltn0.pgf", "kr0.pgf"}) {
			std::vector<u8> data;
			if (!io::ReadFile(std::string("flash0:/font/") + name, data)) continue;
			auto f = std::make_unique<font::PgfFont>();
			if (!f->Parse(std::move(data))) continue;
			if (pgf_.empty()) {
				font::FontInfo info{};
				f->GetFontInfo(info);
				ascend_ = info.maxGlyphAscenderF;
				height_ = (info.maxGlyphAscenderF - info.maxGlyphDescenderF) * kLineSpacing;
				LOGI("PPGE", "dialog text: flash0:/font/%s (ascender %.1f, descender %.1f)", name, info.maxGlyphAscenderF,
				     info.maxGlyphDescenderF);
			}
			pgf_.push_back(std::move(f));
		}
		// The PGF fonts are 18 px to the em.
		gdi_.Init(18);
		if (pgf_.empty()) {
			LOGW("PPGE", "no firmware fonts in flash0:/font; the dialogs use a host font");
			ascend_ = gdi_.Ascent();
			height_ = (gdi_.Ascent() + gdi_.Descent()) * kLineSpacing;
		}
	}
	bool Render(u32 code, GlyphImg &out) {
		font::PgfFont::HostGlyph hg;
		for (const auto &f : pgf_) {
			if (!f->RenderHostGlyph((int)code, hg)) continue;
			out.w = hg.w;
			out.h = hg.h;
			out.left = (float)hg.left;
			out.top = (float)hg.top;
			out.adv = hg.advH / 64.0f;
			out.alpha.resize(hg.ink.size());
			for (size_t i = 0; i < hg.ink.size(); i++) out.alpha[i] = (u8)(hg.ink[i] * 17);
			return true;
		}
		return gdi_.Render(code, out);
	}

	bool loaded_ = false;
	std::vector<std::unique_ptr<font::PgfFont>> pgf_;
	GdiFont gdi_;
	float ascend_ = 15, height_ = 20;
	std::unordered_map<u32, std::unique_ptr<GlyphImg>> cache_;
};

FontSet &Fonts() {
	static FontSet fonts;
	return fonts;
}

float PxScale(float scale) { return scale * kTextScale; }

// ---------------------------------------------------------------- UTF-8

struct Utf8 {
	const char *p, *e;
	explicit Utf8(std::string_view s) : p(s.data()), e(s.data() + s.size()) {}
	bool end() const { return p >= e; }
	u32 next() {
		const u8 b = (u8)*p++;
		if (b < 0x80) return b;
		int n = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
		if (n == 0) return '?';
		u32 c = b & (0x3F >> n);
		for (int i = 0; i < n; i++) {
			if (p >= e || ((u8)*p & 0xC0) != 0x80) return '?';
			c = (c << 6) | ((u8)*p++ & 0x3F);
		}
		return c;
	}
};

bool IsCJK(u32 c) {
	return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x2E80 && c <= 0x2FFF) || (c >= 0x3040 && c <= 0x4DB5) ||
	       (c >= 0x4E00 && c <= 0x9FBB) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAD9) ||
	       (c >= 0x20000 && c <= 0x2A6D6) || (c >= 0x2F800 && c <= 0x2FA1D);
}

// ---------------------------------------------------------------- frames

struct Cmd {
	enum Kind : u8 { RECT, GLYPH, ICON, TEXTURE } kind = RECT;
	float x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // GLYPH: pen x, baseline y, pixel scale
	u32 color = 0;
	u32 code = 0;
	Icon icon = Icon::Cross;
	short clip[4] = {0, 0, 480, 272};
	std::shared_ptr<const Texture> tex;

	bool operator==(const Cmd &o) const {
		return kind == o.kind && x1 == o.x1 && y1 == o.y1 && x2 == o.x2 && y2 == o.y2 && color == o.color && code == o.code &&
		       icon == o.icon && memcmp(clip, o.clip, sizeof(clip)) == 0 && tex == o.tex;
	}
};

struct Frame {
	std::vector<Cmd> cmds;
	u32 vcount = 0;
	bool live = false;
};

Frame g_layers[2];
Frame g_building;
Layer g_buildLayer = Layer::Dialog;
bool g_inFrame = false;
short g_scissor[4] = {0, 0, 480, 272};
// Counts the changes of the layers' contents (Revision).
u64 g_contents = 0;

void Add(Cmd c) {
	if (!g_inFrame) return;
	memcpy(c.clip, g_scissor, sizeof(c.clip));
	g_building.cmds.push_back(std::move(c));
}

// ---------------------------------------------------------------- text layout (PPGe)

struct LaidGlyph {
	float x, y;  // pen, baseline
	u32 code;
};
using Lines = std::vector<std::vector<LaidGlyph>>;

struct TextMetrics {
	float x, y, w, lineHeight, scale;
	int numLines;
};

// Breaks a string into lines (PPGe's BreakLines).
TextMetrics BreakLines(std::string_view text, float x, float y, Align align, float scale, float lineHeightScale,
                       int wrapType, float wrapWidth, bool dryRun, Lines *lines) {
	FontSet &f = Fonts();
	const float px = PxScale(scale);
	y += f.Ascend() * px;
	float sx = x, sy = y;
	if (wrapWidth <= 0) wrapWidth = 480.f;

	// Used for replacing with ellipses.
	float wrapCutoff = 8.0f;
	const GlyphImg *dot = f.Get('.');
	if (dot) wrapCutoff = dot->adv * px * 3.0f;
	float threshold = sx + wrapWidth - wrapCutoff;

	int numLines = 1;
	float maxw = 0;
	float lineHeight = f.Height() * PxScale(lineHeightScale);
	std::vector<LaidGlyph> line;
	for (Utf8 utf(text); !utf.end();) {
		float lineWidth = 0;
		bool skipRest = false;
		while (!utf.end()) {
			Utf8 utfWord(utf);
			float nextWidth = 0;
			float spaceWidth = 0;
			int numChars = 0;
			bool finished = false;
			while (!utfWord.end() && !finished) {
				Utf8 utfPrev = utfWord;
				u32 cval = utfWord.next();
				const GlyphImg *ch = f.Get(cval);
				switch (cval) {
				case ',':
				case '.':
				case ':':
				case '!':
				case ')':
				case '?':
				case 0x3001:  // IDEOGRAPHIC COMMA
				case 0x3002:  // IDEOGRAPHIC FULL STOP
				case 0x06D4:  // ARABIC FULL STOP
				case 0xFF01:  // FULLWIDTH EXCLAMATION MARK
				case 0xFF09:  // FULLWIDTH RIGHT PARENTHESIS
				case 0xFF1F:  // FULLWIDTH QUESTION MARK
					// Count this character (punctuation is so clingy), but then we're done.
					++numChars;
					nextWidth += ch->adv * px;
					finished = true;
					break;
				case ' ':
				case 0x3000:  // IDEOGRAPHIC SPACE
					spaceWidth += ch->adv * px;
					finished = true;
					break;
				case '\t':
				case '\r':
				case '\n':
					// Ignore this character and we're done.
					finished = true;
					break;
				default:
					// CJK characters can be wrapped more freely.
					if (IsCJK(cval) && numChars > 0) {
						utfWord = utfPrev;
						finished = true;
						break;
					}
					++numChars;
					nextWidth += ch->adv * px;
					break;
				}
			}

			bool useEllipsis = false;
			if (wrapType > 0) {
				if (lineWidth + nextWidth > wrapWidth || skipRest) {
					if (wrapType & LINE_WRAP_WORD) {
						if (lineWidth > 0) {
							++numLines;
							break;
						}
					}
					if (wrapType & LINE_USE_ELLIPSIS) {
						useEllipsis = true;
						if (skipRest) {
							numChars = 0;
						} else if (nextWidth < wrapCutoff) {
							// The word is too short, so just backspace!
							x = threshold;
						}
						nextWidth = 0;
						spaceWidth = 0;
						lineWidth = wrapWidth;
					}
				}
			}
			for (int i = 0; i < numChars; ++i) {
				u32 cval = utf.next();
				const GlyphImg *c = f.Get(cval);
				if (useEllipsis && x >= threshold && dot) {
					if (!dryRun) {
						// Replace the following part with an ellipsis.
						for (int k = 0; k < 3; k++) line.push_back({x + dot->adv * px * k, y, '.'});
					}
					skipRest = true;
					break;
				}
				if (!dryRun) line.push_back({x, y, cval});
				x += c->adv * px;
			}
			lineWidth += nextWidth;

			u32 cval = utf.end() ? 0 : utf.next();
			if (spaceWidth > 0) {
				if (!dryRun) line.push_back({x, y, cval});
				x += spaceWidth;
				lineWidth += spaceWidth;
				if (wrapType > 0 && lineWidth > wrapWidth) lineWidth = wrapWidth;
			} else if (cval == '\n') {
				++numLines;
				break;
			}
			utf = utfWord;
		}
		y += lineHeight;
		x = sx;
		if (lineWidth > maxw) maxw = lineWidth;
		if (!dryRun && lines) {
			lines->push_back(std::move(line));
			line.clear();
		}
	}

	const float w = maxw;
	const float h = (float)numLines * lineHeight;
	if (align & ALIGN_ANY) {
		if (!dryRun && lines) {
			for (auto &l : *lines) {
				// Each line is centered (or right-aligned) on its own, as the PSP does.
				float lineW = 0;
				for (const LaidGlyph &g : l)
					if (g.code != ' ' && g.code != 0x3000) lineW = std::max(lineW, g.x + f.Get(g.code)->adv * px - sx);
				for (LaidGlyph &g : l) {
					if (align & BOX_HCENTER) g.x -= lineW / 2.0f;
					else if (align & BOX_RIGHT) g.x -= lineW;
					if (align & BOX_VCENTER) g.y -= h / 2.0f;
					else if (align & BOX_BOTTOM) g.y -= h;
				}
			}
		}
		if (align & BOX_HCENTER) sx -= w / 2.0f;
		else if (align & BOX_RIGHT) sx -= w;
		if (align & BOX_VCENTER) sy -= h / 2.0f;
		else if (align & BOX_BOTTOM) sy -= h;
	}
	return TextMetrics{sx, sy, w, lineHeight, scale, numLines};
}

void EmitText(std::string_view text, float x, float y, Align align, float scale, float lineHeightScale, int wrapType,
              float wrapWidth, u32 color) {
	Lines lines;
	BreakLines(text, x, y, align, scale, lineHeightScale, wrapType, wrapWidth, false, &lines);
	const float px = PxScale(scale);
	for (const auto &l : lines) {
		for (const LaidGlyph &g : l) {
			if (g.code == ' ' || g.code == 0x3000 || g.code < 0x20) continue;
			Cmd c;
			c.kind = Cmd::GLYPH;
			c.x1 = g.x;
			c.y1 = g.y;
			c.x2 = px;
			c.code = g.code;
			c.color = color;
			Add(std::move(c));
		}
	}
}

std::string_view StripTrailingWhite(std::string_view s) {
	size_t last = s.find_last_not_of(" \t\r\n");
	return last != s.npos ? s.substr(0, last + 1) : s;
}

std::string CropLinesToCount(std::string_view s, int numLines) {
	size_t pos = 0;
	for (int i = 0; i < numLines; i++) {
		size_t nl = s.find('\n', pos);
		if (nl == s.npos) return std::string(s);
		pos = nl + 1;
	}
	return std::string(s.substr(0, pos));
}

// ---------------------------------------------------------------- compositing

struct Target {
	u32 *px;
	int w, h;
	float s;  // output pixels per PSP pixel
	int cx1, cy1, cx2, cy2;
};

// color over d with coverage a. d's alpha accumulates (it stays 255 over an opaque image): over a
// transparent one, the result is the drawing with premultiplied alpha.
inline void Blend(u32 &d, u32 color, int a) {
	if (a <= 0) return;
	if (a >= 255) {
		d = (color & 0x00FFFFFF) | 0xFF000000;
		return;
	}
	const int ia = 255 - a;
	u32 r = ((color & 0xFF) * a + (d & 0xFF) * ia + 127) / 255;
	u32 g = (((color >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * ia + 127) / 255;
	u32 b = (((color >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia + 127) / 255;
	u32 da = a + ((d >> 24) * ia + 127) / 255;
	d = r | (g << 8) | (b << 16) | (da << 24);
}

void SetClip(Target &t, const short *clip) {
	t.cx1 = std::max(0, (int)std::floor(clip[0] * t.s + 0.5f));
	t.cy1 = std::max(0, (int)std::floor(clip[1] * t.s + 0.5f));
	t.cx2 = std::min(t.w, (int)std::floor(clip[2] * t.s + 0.5f));
	t.cy2 = std::min(t.h, (int)std::floor(clip[3] * t.s + 0.5f));
}

void FillRect(Target &t, float x1, float y1, float x2, float y2, u32 color) {
	const int ca = (int)(color >> 24);
	if (ca == 0) return;
	x1 *= t.s, y1 *= t.s, x2 *= t.s, y2 *= t.s;
	if (x2 < x1) std::swap(x1, x2);
	if (y2 < y1) std::swap(y1, y2);
	const int ix1 = std::max(t.cx1, (int)std::floor(x1)), ix2 = std::min(t.cx2, (int)std::ceil(x2));
	const int iy1 = std::max(t.cy1, (int)std::floor(y1)), iy2 = std::min(t.cy2, (int)std::ceil(y2));
	if (ix1 >= ix2) return;
	auto coverX = [&](int x) { return std::min(x2, (float)x + 1) - std::max(x1, (float)x); };
	for (int y = iy1; y < iy2; y++) {
		const float cy = std::min(y2, (float)y + 1) - std::max(y1, (float)y);
		if (cy <= 0) continue;
		u32 *row = t.px + (size_t)y * t.w;
		// Partly covered columns at the ends; the ones between have the row's alpha.
		Blend(row[ix1], color, (int)(ca * coverX(ix1) * cy + 0.5f));
		if (ix2 - 1 > ix1) Blend(row[ix2 - 1], color, (int)(ca * coverX(ix2 - 1) * cy + 0.5f));
		const int a = (int)(ca * cy + 0.5f);
		if (a <= 0) continue;
		const u32 ia = 255 - a;
		const u32 sr = (color & 0xFF) * a + 127, sg = ((color >> 8) & 0xFF) * a + 127, sb = ((color >> 16) & 0xFF) * a + 127;
		for (int x = ix1 + 1; x < ix2 - 1; x++) {
			const u32 d = row[x];
			row[x] = ((sr + (d & 0xFF) * ia) / 255) | (((sg + ((d >> 8) & 0xFF) * ia) / 255) << 8) |
			         (((sb + ((d >> 16) & 0xFF) * ia) / 255) << 16) | ((a + ((d >> 24) * ia + 127) / 255) << 24);
		}
	}
}

void DrawGlyph(Target &t, const Cmd &c) {
	const GlyphImg *g = Fonts().Get(c.code);
	if (!g || g->w <= 0 || g->h <= 0) return;
	const int ca = (int)(c.color >> 24);
	if (ca == 0) return;
	const float k = c.x2 * t.s;  // glyph pixels -> output pixels
	const float ox = (c.x1 + g->left * c.x2) * t.s, oy = (c.y1 - g->top * c.x2) * t.s;
	const int ix1 = std::max(t.cx1, (int)std::floor(ox) - 1), ix2 = std::min(t.cx2, (int)std::ceil(ox + g->w * k) + 1);
	const int iy1 = std::max(t.cy1, (int)std::floor(oy) - 1), iy2 = std::min(t.cy2, (int)std::ceil(oy + g->h * k) + 1);
	auto at = [&](int x, int y) -> int {
		if (x < 0 || y < 0 || x >= g->w || y >= g->h) return 0;
		return g->alpha[(size_t)y * g->w + x];
	};
	for (int y = iy1; y < iy2; y++) {
		const float v = (y + 0.5f - oy) / k - 0.5f;
		const int v0 = (int)std::floor(v);
		const float fv = v - v0;
		u32 *row = t.px + (size_t)y * t.w;
		for (int x = ix1; x < ix2; x++) {
			const float u = (x + 0.5f - ox) / k - 0.5f;
			const int u0 = (int)std::floor(u);
			const float fu = u - u0;
			const float a = (at(u0, v0) * (1 - fu) + at(u0 + 1, v0) * fu) * (1 - fv) +
			                (at(u0, v0 + 1) * (1 - fu) + at(u0 + 1, v0 + 1) * fu) * fv;
			if (a <= 0.5f) continue;
			Blend(row[x], c.color, (int)(a * ca / 255.0f + 0.5f));
		}
	}
}

float SegDist(float px, float py, float ax, float ay, float bx, float by) {
	const float vx = bx - ax, vy = by - ay;
	float h = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
	h = std::clamp(h, 0.0f, 1.0f);
	const float dx = px - ax - vx * h, dy = py - ay - vy * h;
	return std::sqrt(dx * dx + dy * dy);
}

// Distance from (u, v) (unit square) to the symbol's strokes, minus their half width.
float IconDistance(Icon icon, float u, float v) {
	constexpr float kHalf = 0.09f;
	switch (icon) {
	case Icon::Circle: return std::fabs(std::hypot(u - 0.5f, v - 0.5f) - 0.38f) - kHalf;
	case Icon::Cross:
		return std::min(SegDist(u, v, 0.15f, 0.15f, 0.85f, 0.85f), SegDist(u, v, 0.85f, 0.15f, 0.15f, 0.85f)) - kHalf;
	case Icon::Triangle: {
		const float ax = 0.5f, ay = 0.1f, bx = 0.92f, by = 0.84f, cx = 0.08f, cy = 0.84f;
		return std::min({SegDist(u, v, ax, ay, bx, by), SegDist(u, v, bx, by, cx, cy), SegDist(u, v, cx, cy, ax, ay)}) - kHalf;
	}
	case Icon::Square: {
		const float a = 0.14f, b = 0.86f;
		return std::min({SegDist(u, v, a, a, b, a), SegDist(u, v, b, a, b, b), SegDist(u, v, b, b, a, b), SegDist(u, v, a, b, a, a)}) - kHalf;
	}
	case Icon::Hexagon: {
		float d = 1e9f;
		for (int i = 0; i < 6; i++) {
			const float a0 = (float)(i * 60 + 30) * 3.14159265f / 180.0f, a1 = (float)((i + 1) * 60 + 30) * 3.14159265f / 180.0f;
			d = std::min(d, SegDist(u, v, 0.5f + 0.4f * std::cos(a0), 0.5f + 0.4f * std::sin(a0), 0.5f + 0.4f * std::cos(a1),
			                        0.5f + 0.4f * std::sin(a1)));
		}
		return d - 0.1f;
	}
	}
	return 1.0f;
}

void DrawIconCmd(Target &t, const Cmd &c) {
	const int ca = (int)(c.color >> 24);
	if (ca == 0) return;
	const float x1 = c.x1 * t.s, y1 = c.y1 * t.s, w = (c.x2 - c.x1) * t.s, h = (c.y2 - c.y1) * t.s;
	if (w <= 0 || h <= 0) return;
	const int ix1 = std::max(t.cx1, (int)std::floor(x1)), ix2 = std::min(t.cx2, (int)std::ceil(x1 + w));
	const int iy1 = std::max(t.cy1, (int)std::floor(y1)), iy2 = std::min(t.cy2, (int)std::ceil(y1 + h));
	const float pix = 1.0f / std::min(w, h);  // one output pixel in symbol units
	for (int y = iy1; y < iy2; y++) {
		const float v = (y + 0.5f - y1) / h;
		u32 *row = t.px + (size_t)y * t.w;
		for (int x = ix1; x < ix2; x++) {
			const float u = (x + 0.5f - x1) / w;
			const float cov = std::clamp(0.5f - IconDistance(c.icon, u, v) / pix, 0.0f, 1.0f);
			if (cov > 0) Blend(row[x], c.color, (int)(ca * cov + 0.5f));
		}
	}
}

void DrawTextureCmd(Target &t, const Cmd &c) {
	const Texture *tex = c.tex.get();
	if (!tex || tex->width <= 0 || tex->height <= 0) return;
	const float x1 = c.x1 * t.s, y1 = c.y1 * t.s, w = (c.x2 - c.x1) * t.s, h = (c.y2 - c.y1) * t.s;
	if (w <= 0 || h <= 0) return;
	const int ix1 = std::max(t.cx1, (int)std::floor(x1)), ix2 = std::min(t.cx2, (int)std::ceil(x1 + w));
	const int iy1 = std::max(t.cy1, (int)std::floor(y1)), iy2 = std::min(t.cy2, (int)std::ceil(y1 + h));
	const float cr = (c.color & 0xFF) / 255.0f, cg = ((c.color >> 8) & 0xFF) / 255.0f, cb = ((c.color >> 16) & 0xFF) / 255.0f,
	            ca = (c.color >> 24) / 255.0f;
	auto texel = [&](int x, int y) {
		x = std::clamp(x, 0, tex->width - 1);
		y = std::clamp(y, 0, tex->height - 1);
		return tex->pixels[(size_t)y * tex->width + x];
	};
	for (int y = iy1; y < iy2; y++) {
		const float v = (y + 0.5f - y1) / h * tex->height - 0.5f;
		const int v0 = (int)std::floor(v);
		const float fv = v - v0;
		u32 *row = t.px + (size_t)y * t.w;
		for (int x = ix1; x < ix2; x++) {
			const float u = (x + 0.5f - x1) / w * tex->width - 0.5f;
			const int u0 = (int)std::floor(u);
			const float fu = u - u0;
			const u32 p[4] = {texel(u0, v0), texel(u0 + 1, v0), texel(u0, v0 + 1), texel(u0 + 1, v0 + 1)};
			const float wgt[4] = {(1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv};
			float r = 0, g = 0, b = 0, a = 0;
			for (int i = 0; i < 4; i++) {
				const float pa = (p[i] >> 24) * wgt[i];
				r += (p[i] & 0xFF) * pa;
				g += ((p[i] >> 8) & 0xFF) * pa;
				b += ((p[i] >> 16) & 0xFF) * pa;
				a += pa;
			}
			if (a <= 0.5f) continue;
			const u32 rgb = (u32)std::min(255.0f, r / a * cr) | ((u32)std::min(255.0f, g / a * cg) << 8) |
			                ((u32)std::min(255.0f, b / a * cb) << 16);
			Blend(row[x], rgb, (int)(a * ca + 0.5f));
		}
	}
}

bool LayerVisible(int i) {
	const Frame &f = g_layers[i];
	if (!f.live || f.cmds.empty()) return false;
	if (i == (int)Layer::System) return true;
	return display::Vcount() - f.vcount <= kDialogFrameVblanks;
}

}  // namespace

// ---------------------------------------------------------------- API

bool Enabled() {
	static const bool forced = [] {
		const char *v = getenv("PSPC_HEADLESS_OVERLAYS");
		return v && *v && strcmp(v, "0") != 0;
	}();
	return !host::IsHeadless() || forced;
}

std::shared_ptr<const Texture> DecodeTexture(const u8 *data, size_t size) {
	auto t = std::make_shared<Texture>();
	if (!data || !size || !media::DecodeImageRgba(data, size, &t->width, &t->height, &t->pixels)) return nullptr;
	return t;
}

void Begin(Layer layer) {
	g_building = Frame{};
	g_buildLayer = layer;
	g_inFrame = Enabled();
	ScissorReset();
}

void End() {
	if (!g_inFrame) return;
	g_inFrame = false;
	g_building.vcount = display::Vcount();
	g_building.live = true;
	Frame &shown = g_layers[(int)g_buildLayer];
	if (!shown.live || shown.cmds != g_building.cmds) g_contents++;
	shown = std::move(g_building);
	g_building = Frame{};
}

void Clear(Layer layer) {
	g_layers[(int)layer] = Frame{};
	g_contents++;
}

bool Visible(Layer layer) { return LayerVisible((int)layer); }

u64 Revision(bool gameScreen) {
	// Which layers are drawn changes it too (a dialog frame expires; a system screen hides the dialog).
	return g_contents << 2 | (gameScreen && LayerVisible(0) ? 1 : 0) | (LayerVisible(1) ? 2 : 0);
}

void DrawRect(float x1, float y1, float x2, float y2, u32 color) {
	if (!g_inFrame) return;
	Cmd c;
	c.kind = Cmd::RECT;
	c.x1 = x1, c.y1 = y1, c.x2 = x2, c.y2 = y2;
	c.color = color;
	Add(std::move(c));
}

void DrawString(std::string_view text, float x, float y, const Style &style) {
	if (!g_inFrame || text.empty()) return;
	if (style.hasShadow) EmitText(text, x + 1, y + 2, style.align, style.scale, style.scale, LINE_USE_ELLIPSIS, 0, style.shadowColor);
	EmitText(text, x, y, style.align, style.scale, style.scale, LINE_USE_ELLIPSIS, 0, style.color);
}

void DrawStringWrapped(std::string_view text, float x, float y, float wrapWidth, float wrapHeight, const Style &style) {
	if (!g_inFrame || text.empty()) return;
	std::string s(wrapHeight != 0.0f ? StripTrailingWhite(text) : text);
	const float maxScaleDown = 1.3f;
	const int wrap = LINE_USE_ELLIPSIS | LINE_WRAP_WORD;
	TextMetrics m = BreakLines(s, x, y, style.align, style.scale, style.scale, wrap, wrapWidth, true, nullptr);
	float scale = style.scale, lineHeightScale = style.scale;
	float actualHeight = m.lineHeight * m.numLines;
	if (wrapHeight != 0.0f && actualHeight > wrapHeight) {
		if (actualHeight > wrapHeight * maxScaleDown) {
			float maxLines = std::floor(wrapHeight * maxScaleDown / m.lineHeight);
			actualHeight = (maxLines + 1) * m.lineHeight;
			// Add an ellipsis if it's just too long to be readable. On a PSP, it does this without
			// scaling it down.
			s = std::string(StripTrailingWhite(CropLinesToCount(s, (int)maxLines)));
			s.append("\n...");
		}
		const float reduced = style.scale * wrapHeight / actualHeight;
		// Try to keep the font as large as possible, so reduce the line height some.
		scale = reduced * 1.15f;
		lineHeightScale = reduced;
	}
	if (style.hasShadow) EmitText(s, x + 1, y + 2, style.align, scale, lineHeightScale, wrap, wrapWidth, style.shadowColor);
	EmitText(s, x, y, style.align, scale, lineHeightScale, wrap, wrapWidth, style.color);
}

void DrawIcon(Icon icon, float x, float y, float w, float h, const Style &style) {
	if (!g_inFrame) return;
	Cmd c;
	c.kind = Cmd::ICON;
	c.icon = icon;
	if (style.hasShadow) {
		const u32 a = (u32)((style.shadowColor >> 24) * 0.6f);
		c.x1 = x + 1, c.y1 = y + 1.5f, c.x2 = x + w + 1, c.y2 = y + h + 1.5f;
		c.color = (style.shadowColor & 0x00FFFFFF) | (a << 24);
		Add(c);
	}
	c.x1 = x, c.y1 = y, c.x2 = x + w, c.y2 = y + h;
	c.color = style.color;
	Add(std::move(c));
}

void DrawTexture(const std::shared_ptr<const Texture> &tex, float x, float y, float w, float h, u32 color) {
	if (!g_inFrame || !tex) return;
	Cmd c;
	c.kind = Cmd::TEXTURE;
	c.x1 = x, c.y1 = y, c.x2 = x + w, c.y2 = y + h;
	c.color = color;
	c.tex = tex;
	Add(std::move(c));
}

void Scissor(int x1, int y1, int x2, int y2) {
	g_scissor[0] = (short)std::clamp(x1, 0, 480);
	g_scissor[1] = (short)std::clamp(y1, 0, 272);
	g_scissor[2] = (short)std::clamp(x2, 0, 480);
	g_scissor[3] = (short)std::clamp(y2, 0, 272);
}

void ScissorReset() { Scissor(0, 0, 480, 272); }

void MeasureString(float *w, float *h, std::string_view text, float scale, int wrapType, float wrapWidth) {
	TextMetrics m = BreakLines(text, 0, 0, BOX_TOP, scale, scale, wrapType, wrapWidth, true, nullptr);
	if (w) *w = m.w;
	if (h) *h = m.lineHeight * m.numLines;
}

void Composite(u32 *rgba, int width, int height, bool gameScreen) {
	if (!rgba || width <= 0 || height <= 0) return;
	if (!LayerVisible(0) && !LayerVisible(1)) return;
	Target t{rgba, width, height, width / 480.0f, 0, 0, width, height};
	for (int i = gameScreen ? 0 : 1; i < 2; i++) {
		if (!LayerVisible(i)) continue;
		for (const Cmd &c : g_layers[i].cmds) {
			SetClip(t, c.clip);
			if (t.cx1 >= t.cx2 || t.cy1 >= t.cy2) continue;
			switch (c.kind) {
			case Cmd::RECT: FillRect(t, c.x1, c.y1, c.x2, c.y2, c.color); break;
			case Cmd::GLYPH: DrawGlyph(t, c); break;
			case Cmd::ICON: DrawIconCmd(t, c); break;
			case Cmd::TEXTURE: DrawTextureCmd(t, c); break;
			}
		}
	}
}

std::string Utf16ToUtf8(const std::u16string &in) {
	std::string out;
	for (size_t i = 0; i < in.size(); i++) {
		u32 c = in[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
			c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
			i++;
		}
		if (c < 0x80) {
			out.push_back((char)c);
		} else if (c < 0x800) {
			out.push_back((char)(0xC0 | (c >> 6)));
			out.push_back((char)(0x80 | (c & 0x3F)));
		} else if (c < 0x10000) {
			out.push_back((char)(0xE0 | (c >> 12)));
			out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (c & 0x3F)));
		} else {
			out.push_back((char)(0xF0 | (c >> 18)));
			out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
			out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (c & 0x3F)));
		}
	}
	return out;
}

}  // namespace ppge
