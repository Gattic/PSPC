// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceLibFont (also registered as sceLibFttt): the PSP font library.
//
// A font library ("fontlib") is created with sceFontNewLib and holds up to 9 open fonts. Fonts
// are PGF files (font_pgf.h): the 18 firmware fonts in flash0:/font/ opened by index, or fonts
// the game supplies as a file or a memory block. The library gets all its memory from the
// game's alloc/free callbacks and, for user files opened in mode 0, reads through the game's
// open/seek/read/close callbacks. Those calls are made synchronously on the calling thread.
//
// Ported from PPSSPP (Core/HLE/sceFont.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later: the font registry, style matching, resolution handling and error behaviour.
// Adjusted to pspautotests font/: the four separate allocations of sceFontNewLib, reference
// counting of user fonts, the handler read sequence of sceFontOpenUserFile mode 0, the stingy
// allocation sizes and where the real library reschedules.
//
// Firmware fonts that aren't installed open as blank fonts (plausible metrics, empty glyphs),
// so games keep running; see LoadInternalFont().
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/font_pgf.h"
#include "runtime/hle/modules/misc_libs.h"
#include "runtime/io/io.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"

using namespace font;

namespace {

// Not in errors.h; numbered after SCE_FONT_ERROR_HANDLER_OPEN_FAILED like the SDK's other
// handler errors.
constexpr u32 FONT_ERROR_HANDLER_CLOSE_FAILED = 0x80460006;
constexpr u32 FONT_ERROR_HANDLER_READ_FAILED = 0x80460007;
constexpr u32 FONT_ERROR_HANDLER_SEEK_FAILED = 0x80460008;

constexpr u32 kMaxFontsPerLib = 9;
constexpr int kMaxFontRefs = 4;
constexpr float kPointDpi = 72.0f;

// Sizes of the library's own allocations (pspautotests font/fonttest, font/newlib).
constexpr u32 kLibBlockSize = 0x4C;
constexpr u32 kFontHandleBlockSize = 0x4C;
constexpr u32 kFontStateBlockSize = 0x230;
constexpr u32 kStyleBlockSize = sizeof(FontStyle);
// Extra bytes allocated with a fully loaded font file, and the whole allocation of a memory font.
constexpr u32 kFontFileExtra = 12;

// Delays standing in for the real library's I/O and setup time (PPSSPP's values). They also
// make the calls reschedule the way the firmware's do.
constexpr u32 kNewLibDelayUs = 30000;
constexpr u32 kOpenDelayUs = 10000;
constexpr u32 kFontListDelayUs = 100;
constexpr u32 kGlyphReadDelayUs = 100;

#pragma pack(push, 1)
struct FontNewLibParams {
	u32 userData;
	u32 numFonts;
	u32 cacheData;
	u32 allocFunc, freeFunc;
	u32 openFunc, closeFunc, readFunc, seekFunc;
	u32 errorFunc, ioFinishFunc;
};
static_assert(sizeof(FontNewLibParams) == 0x2C, "FontNewLibParams layout");

// What a library handle points to. The callbacks are read back from here on use: games (and
// pspautotests font/open, font/openfile) change them after sceFontNewLib.
struct NativeFontLib {
	FontNewLibParams params;
	u32 fontHandles;   // numFonts blocks of kFontHandleBlockSize; font handles point into it
	u32 fontStates;    // numFonts blocks of kFontStateBlockSize
	u16 unk1, unk2;
	float hRes, vRes;
	u32 internalFontCount;
	u32 internalFontStyles;  // internalFontCount FontStyles
	u16 altCharCode, unk5;
};
static_assert(sizeof(NativeFontLib) == kLibBlockSize, "NativeFontLib layout");
#pragma pack(pop)

enum Family : u16 { FAMILY_SANS_SERIF = 1, FAMILY_SERIF = 2 };
enum Style : u16 { STYLE_REGULAR = 1, STYLE_ITALIC = 2, STYLE_BOLD = 5, STYLE_BOLD_ITALIC = 6, STYLE_DB = 103 };
enum Language : u16 { LANG_JAPANESE = 1, LANG_LATIN = 2, LANG_KOREAN = 3 };

// The firmware fonts, in firmware index order. Sizes are 1/64 point, resolutions 1/64 dpi.
// fileSize and stingySize are what sceFontOpen allocates in mode 1 (plus kFontFileExtra) and
// otherwise (pspautotests font/open); they are used when the file itself isn't installed.
struct RegistryEntry {
	s32 hSize, vSize, hRes, vRes;
	u32 extraAttributes;
	s32 weight;
	u16 family, style, styleSub, language, region, country;
	const char *fileName;
	const char *fontName;
	u32 expireDate;
	u32 fileSize, stingySize;
};

const RegistryEntry kRegistry[] = {
	{0x288, 0x288, 0x2000, 0x2000, 0, 0, FAMILY_SANS_SERIF, STYLE_DB, 0, LANG_JAPANESE, 0, 1, "jpn0.pgf", "FTT-NewRodin Pro DB", 0, 1581700, 145844},
	{0x288, 0x288, 0x2000, 0x2000, 0, 0, FAMILY_SANS_SERIF, STYLE_REGULAR, 0, LANG_LATIN, 0, 1, "ltn0.pgf", "FTT-NewRodin Pro Latin", 0, 69108, 16680},
	{0x288, 0x288, 0x2000, 0x2000, 0, 0, FAMILY_SERIF, STYLE_REGULAR, 0, LANG_LATIN, 0, 1, "ltn1.pgf", "FTT-Matisse Pro Latin", 0, 65124, 16920},
	{0x288, 0x288, 0x2000, 0x2000, 0, 0, FAMILY_SANS_SERIF, STYLE_ITALIC, 0, LANG_LATIN, 0, 1, "ltn2.pgf", "FTT-NewRodin Pro Latin", 0, 72948, 16872},
	{0x288, 0x288, 0x2000, 0x2000, 0, 0, FAMILY_SERIF, STYLE_ITALIC, 0, LANG_LATIN, 0, 1, "ltn3.pgf", "FTT-Matisse Pro Latin", 0, 67700, 17112},
	{0x288, 0x288, 0x2000, 0x2000, 0, 0, FAMILY_SANS_SERIF, STYLE_BOLD, 0, LANG_LATIN, 0, 1, "ltn4.pgf", "FTT-NewRodin Pro Latin", 0, 72828, 16648},
	{0x288, 0x288, 0x2000, 0x2000, 0, 0, FAMILY_SERIF, STYLE_BOLD, 0, LANG_LATIN, 0, 1, "ltn5.pgf", "FTT-Matisse Pro Latin", 0, 68220, 16928},
	{0x288, 0x288, 0x2000, 0x2000, 0, 0, FAMILY_SANS_SERIF, STYLE_BOLD_ITALIC, 0, LANG_LATIN, 0, 1, "ltn6.pgf", "FTT-NewRodin Pro Latin", 0, 77032, 16792},
	{0x288, 0x288, 0x2000, 0x2000, 0, 0, FAMILY_SERIF, STYLE_BOLD_ITALIC, 0, LANG_LATIN, 0, 1, "ltn7.pgf", "FTT-Matisse Pro Latin", 0, 71144, 17160},
	{0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FAMILY_SANS_SERIF, STYLE_REGULAR, 0, LANG_LATIN, 0, 1, "ltn8.pgf", "FTT-NewRodin Pro Latin", 0, 41000, 16192},
	{0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FAMILY_SERIF, STYLE_REGULAR, 0, LANG_LATIN, 0, 1, "ltn9.pgf", "FTT-Matisse Pro Latin", 0, 40164, 16476},
	{0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FAMILY_SANS_SERIF, STYLE_ITALIC, 0, LANG_LATIN, 0, 1, "ltn10.pgf", "FTT-NewRodin Pro Latin", 0, 42692, 16300},
	{0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FAMILY_SERIF, STYLE_ITALIC, 0, LANG_LATIN, 0, 1, "ltn11.pgf", "FTT-Matisse Pro Latin", 0, 41488, 16656},
	{0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FAMILY_SANS_SERIF, STYLE_BOLD, 0, LANG_LATIN, 0, 1, "ltn12.pgf", "FTT-NewRodin Pro Latin", 0, 43136, 16176},
	{0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FAMILY_SERIF, STYLE_BOLD, 0, LANG_LATIN, 0, 1, "ltn13.pgf", "FTT-Matisse Pro Latin", 0, 41772, 16436},
	{0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FAMILY_SANS_SERIF, STYLE_BOLD_ITALIC, 0, LANG_LATIN, 0, 1, "ltn14.pgf", "FTT-NewRodin Pro Latin", 0, 45184, 16272},
	{0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FAMILY_SERIF, STYLE_BOLD_ITALIC, 0, LANG_LATIN, 0, 1, "ltn15.pgf", "FTT-Matisse Pro Latin", 0, 43044, 16704},
	{0x288, 0x288, 0x2000, 0x2000, 0, 0, FAMILY_SANS_SERIF, STYLE_REGULAR, 0, LANG_KOREAN, 0, 3, "kr0.pgf", "AsiaNHH(512Johab)", 0, 394192, 51856},
};
constexpr u32 kNumInternalFonts = (u32)ArraySize(kRegistry);

FontStyle StyleFromRegistry(const RegistryEntry &e) {
	FontStyle s{};
	s.fontH = e.hSize / 64.0f;
	s.fontV = e.vSize / 64.0f;
	s.fontHRes = e.hRes / 64.0f;
	s.fontVRes = e.vRes / 64.0f;
	s.fontWeight = (float)e.weight;
	s.fontFamily = e.family;
	s.fontStyle = e.style;
	s.fontStyleSub = e.styleSub;
	s.fontLanguage = e.language;
	s.fontRegion = e.region;
	s.fontCountry = e.country;
	snprintf(s.fontName, sizeof(s.fontName), "%s", e.fontName);
	snprintf(s.fontFileName, sizeof(s.fontFileName), "%s", e.fileName);
	s.fontAttributes = e.extraAttributes;
	s.fontExpire = e.expireDate;
	return s;
}

// A user font's style only carries its size and resolution.
FontStyle StyleFromFont(const PgfFont &f) {
	FontStyle s{};
	s.fontH = f.HSize() / 64.0f;
	s.fontV = f.VSize() / 64.0f;
	s.fontHRes = f.HResolution() / 64.0f;
	s.fontVRes = f.VResolution() / 64.0f;
	return s;
}

struct InternalFont {
	FontStyle style{};
	std::shared_ptr<const PgfFont> data;  // loaded on first open
	u32 fileSize = 0;                     // of the loaded file
};
std::vector<InternalFont> g_internal;

void InitInternalFonts() {
	if (!g_internal.empty()) return;
	g_internal.resize(kNumInternalFonts);
	for (u32 i = 0; i < kNumInternalFonts; i++) g_internal[i].style = StyleFromRegistry(kRegistry[i]);
}

// Firmware font data: the game's own copy in PSP_GAME/USRDIR (where translation patches put
// replacements, as PPSSPP allows), else flash0:/font/. Without either (the default flash0 has no
// fonts) the font opens blank, which keeps games running where failing the open would make most
// of them stop; text just doesn't show.
const InternalFont &LoadInternalFont(u32 index) {
	InternalFont &f = g_internal[index];
	if (f.data) return f;
	const RegistryEntry &e = kRegistry[index];
	const std::string paths[] = {std::string("disc0:/PSP_GAME/USRDIR/") + e.fileName,
	                             std::string("flash0:/font/") + e.fileName};
	for (const std::string &path : paths) {
		std::vector<u8> buf;
		if (!io::ReadFile(path, buf)) continue;
		u32 size = (u32)buf.size();
		auto pgf = std::make_shared<PgfFont>();
		if (!pgf->Parse(std::move(buf))) {
			LOGW("FONT", "%s is not a valid PGF font, ignored", path.c_str());
			continue;
		}
		LOGI("FONT", "loaded internal font %u from %s (%u bytes)", index, path.c_str(), size);
		f.data = pgf;
		f.fileSize = size;
		return f;
	}
	LOGW("FONT", "firmware font %s not found in flash0:/font/; it will draw nothing", e.fileName);
	auto blank = std::make_shared<PgfFont>();
	blank->InitBlank(e.hSize, e.vSize, e.hRes, e.vRes, e.language != LANG_LATIN);
	f.data = blank;
	f.fileSize = 0;
	return f;
}

enum class OpenMode { InternalStingy, InternalFull, UserHandlers, UserFile, UserMemory };

struct Slot {
	u32 handle = 0;  // guest address inside the library's font handle block
	int refs = 0;
	bool used = false;  // has held a font; queries on a closed handle still work
	std::string key;    // identity for reference counting
	OpenMode mode = OpenMode::InternalStingy;
	std::shared_ptr<const PgfFont> font;
	FontStyle style{};
	std::vector<u32> allocations;  // freed when the last reference closes
	// Glyphs already read: a font opened without its glyph data reads each one on first use.
	std::unordered_set<int> loadedGlyphs;
};

struct FontLib {
	u32 handle = 0;  // guest NativeFontLib; 0 once sceFontDoneLib released it
	u32 addr = 0;    // where the NativeFontLib was, kept after release
	FontNewLibParams params{};
	u32 numFonts = 0;
	float hRes = 128.0f, vRes = 128.0f;
	u32 altChar = 0x5F;
	u32 blocks[4] = {};  // lib struct, font handles, font states, internal styles
	std::vector<Slot> slots;
	// Buffers for the character last asked about, freed by the next query, sceFontFlush or a close.
	std::vector<u32> charBuffers;
};

// Keyed by the address the library was created at. Released libraries stay until their address
// is reused: a few calls still work on them (pspautotests font/altcharcode, font/resolution).
std::map<u32, std::unique_ptr<FontLib>> g_libs;

// ---------------------------------------------------------------- helpers

void SetError(u32 errorPtr, u32 code) {
	if (errorPtr && mem::IsValidRange(errorPtr, 4)) GWrite32(errorPtr, code);
}

bool ValidFunc(u32 addr) { return addr != 0 && mem::IsValidAddress(addr); }

void Delay(u32 us) { sched::WaitUntil(kernel::WAITTYPE_HLEDELAY, 0, sched::NowUs() + us, false); }

// The real library is user code with sizeable stack frames, so it leaves the stack below the
// caller's frame overwritten; some games depend on that (PPSSPP's HLE_CLEAR_STACK_BYTES).
void ClearStack(u32 bytes) {
	Ctx *c = cpu::CurrentCtx();
	kernel::PspThread *t = sched::Current();
	if (!c || !t) return;
	u32 sp = c->r[29];
	if (sp < bytes || sp - bytes < t->stackAddr || !mem::IsValidRange(sp - bytes, bytes)) return;
	mem::Memset(sp - bytes, 0, bytes);
}

u32 CallGame(u32 func, std::initializer_list<u32> args) { return cpu::CallGuest(cpu::CurrentCtx(), func, args); }

// The callbacks as the game has them now.
FontNewLibParams LiveParams(const FontLib &lib) {
	if (lib.handle && mem::IsValidRange(lib.handle, sizeof(FontNewLibParams)))
		return GRead<FontNewLibParams>(lib.handle);
	return lib.params;
}

u32 GameAlloc(const FontNewLibParams &p, u32 size) {
	if (!ValidFunc(p.allocFunc)) return 0;
	return CallGame(p.allocFunc, {p.userData, size});
}
void GameFree(const FontNewLibParams &p, u32 ptr) {
	if (ptr && ValidFunc(p.freeFunc)) CallGame(p.freeFunc, {p.userData, ptr});
}

FontLib *FindLib(u32 handle) {
	auto it = g_libs.find(handle);
	return it == g_libs.end() ? nullptr : it->second.get();
}
FontLib *FindLiveLib(u32 handle) {
	FontLib *lib = FindLib(handle);
	return lib && lib->handle ? lib : nullptr;
}

// The open font with this handle, or (allowClosed) one that was opened before.
Slot *FindFont(u32 handle, bool allowClosed, FontLib **libOut = nullptr) {
	if (!handle) return nullptr;
	for (auto &[addr, lib] : g_libs) {
		if (!lib->handle) continue;
		for (Slot &s : lib->slots) {
			if (s.handle != handle || !s.used || !s.font) continue;
			if (s.refs == 0 && !allowClosed) return nullptr;
			if (libOut) *libOut = lib.get();
			return &s;
		}
	}
	return nullptr;
}

void FlushCharBuffers(FontLib &lib) {
	if (lib.charBuffers.empty()) return;
	FontNewLibParams p = LiveParams(lib);
	for (u32 b : lib.charBuffers) GameFree(p, b);
	lib.charBuffers.clear();
}

// Allocates the character buffers (zero sizes skipped). False if an allocation failed.
bool AllocCharBuffers(FontLib &lib, std::initializer_list<u32> sizes) {
	FlushCharBuffers(lib);
	FontNewLibParams p = LiveParams(lib);
	for (u32 size : sizes) {
		if (!size) continue;
		u32 b = GameAlloc(p, size);
		if (!b) return false;
		lib.charBuffers.push_back(b);
	}
	return true;
}

void ReleaseSlot(FontLib &lib, Slot &s) {
	FontNewLibParams p = LiveParams(lib);
	for (u32 a : s.allocations) GameFree(p, a);
	s.allocations.clear();
	s.loadedGlyphs.clear();
	s.refs = 0;
}

// The slot already holding `key`, or a free one. The firmware shares one handle between opens of
// the same font, up to kMaxFontRefs (pspautotests font/open, openfile, openmem).
Slot *AcquireSlot(FontLib &lib, const std::string &key, bool &existing, u32 &error) {
	existing = false;
	for (Slot &s : lib.slots) {
		if (s.refs > 0 && s.key == key) {
			if (s.refs >= kMaxFontRefs) {
				error = SCE_FONT_ERROR_TOO_MANY_OPEN_FONTS;
				return nullptr;
			}
			existing = true;
			return &s;
		}
	}
	for (Slot &s : lib.slots)
		if (s.refs == 0) return &s;
	error = SCE_FONT_ERROR_TOO_MANY_OPEN_FONTS;
	return nullptr;
}

void FillSlot(Slot &s, const std::string &key, OpenMode mode, std::shared_ptr<const PgfFont> font,
              const FontStyle &style, std::vector<u32> allocations) {
	s.refs = 1;
	s.used = true;
	s.key = key;
	s.mode = mode;
	s.font = std::move(font);
	s.style = style;
	s.allocations = std::move(allocations);
	s.loadedGlyphs.clear();
}

// Fonts opened without their glyph data read each glyph from the file on first use.
void TouchGlyph(Slot &s, int code) {
	if (s.mode != OpenMode::InternalStingy && s.mode != OpenMode::UserHandlers) return;
	if (s.loadedGlyphs.insert(code).second) Delay(kGlyphReadDelayUs);
}

// Length of the valid guest range starting at addr, up to len.
u32 ValidLength(u32 addr, u32 len) {
	if (mem::IsValidRange(addr, len)) return len;
	u32 lo = 0, hi = len;  // lo is valid, hi isn't
	while (hi - lo > 1) {
		u32 mid = lo + (hi - lo) / 2;
		if (mem::IsValidRange(addr, mid))
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

// The tables of a font opened without its glyph data, one allocation each in file order (zero
// sizes included). `freeOrder` gets them in the order the firmware frees them (pspautotests
// font/fonttest, which opens jpn0 this way). Returns false, with nothing left allocated, when an
// allocation fails.
enum TableAlloc { TA_DIM, TA_XADJ, TA_YADJ, TA_ADV, TA_SHADOW, TA_COMP1, TA_COMP2, TA_CHARMAP, TA_CHARPTR, TA_COUNT };

void TableSizes(const PgfLayout &l, u32 (&sizes)[TA_COUNT]) {
	const u32 s[TA_COUNT] = {l.dimLen * 8,     l.xAdjLen * 8, l.yAdjLen * 8,   l.advLen * 8,  l.shadowMapBytes,
	                         l.comp1Bytes,     l.comp2Bytes,  l.charMapBytes,  l.charPtrBytes};
	memcpy(sizes, s, sizeof(s));
}

bool AllocTables(const FontNewLibParams &p, const PgfLayout &l, std::vector<u32> &freeOrder,
                 u32 (*blocksOut)[TA_COUNT] = nullptr) {
	u32 sizes[TA_COUNT], blocks[TA_COUNT] = {};
	TableSizes(l, sizes);
	for (int i = 0; i < TA_COUNT; i++) {
		blocks[i] = GameAlloc(p, sizes[i]);
		if (sizes[i] && (!blocks[i] || !mem::IsValidRange(blocks[i], sizes[i]))) {
			blocks[i] = 0;
			for (int j = i - 1; j >= 0; j--) GameFree(p, blocks[j]);
			return false;
		}
	}
	freeOrder.clear();
	for (int i : {TA_CHARPTR, TA_CHARMAP, TA_COMP1, TA_COMP2, TA_SHADOW, TA_DIM, TA_XADJ, TA_YADJ, TA_ADV})
		freeOrder.push_back(blocks[i]);
	if (blocksOut) memcpy(*blocksOut, blocks, sizeof(blocks));
	return true;
}

// User files are identified by the name the game passes.
std::string FileKey(const char *fileName) { return std::string("file:") + fileName; }

// ---------------------------------------------------------------- library

u32 sceFontNewLib(u32 paramPtr, u32 errorPtr) {
	ClearStack(0x5A0);
	InitInternalFonts();
	if (!mem::IsValidRange(paramPtr, sizeof(FontNewLibParams))) {
		// The PSP crashes here.
		LOGE("FONT", "sceFontNewLib(%08x, %08x): bad params", paramPtr, errorPtr);
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_PARAMETER);
		return 0;
	}
	FontNewLibParams params = GRead<FontNewLibParams>(paramPtr);
	if (!ValidFunc(params.allocFunc) || !ValidFunc(params.freeFunc)) {
		LOGW("FONT", "sceFontNewLib(%08x, %08x): alloc/free callbacks missing", paramPtr, errorPtr);
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_PARAMETER);
		return 0;
	}
	SetError(errorPtr, 0);
	params.numFonts = std::min(params.numFonts, kMaxFontsPerLib);
	const u32 n = params.numFonts;

	// Four separate allocations, in this order (pspautotests font/fonttest).
	const u32 sizes[4] = {kLibBlockSize, n * kFontHandleBlockSize, n * kFontStateBlockSize,
	                      kNumInternalFonts * kStyleBlockSize};
	u32 blocks[4] = {};
	for (int i = 0; i < 4; i++) {
		if (!sizes[i]) continue;
		blocks[i] = GameAlloc(params, sizes[i]);
		if (!blocks[i] || !mem::IsValidRange(blocks[i], sizes[i])) {
			for (int j = i - 1; j >= 0; j--) GameFree(params, blocks[j]);
			LOGW("FONT", "sceFontNewLib: allocation of %u bytes failed", sizes[i]);
			SetError(errorPtr, SCE_FONT_ERROR_OUT_OF_MEMORY);
			return 0;
		}
	}

	auto lib = std::make_unique<FontLib>();
	lib->handle = blocks[0];
	lib->addr = blocks[0];
	lib->params = params;
	lib->numFonts = n;
	memcpy(lib->blocks, blocks, sizeof(blocks));
	lib->slots.resize(n);
	for (u32 i = 0; i < n; i++) lib->slots[i].handle = blocks[1] + i * kFontHandleBlockSize;

	NativeFontLib nfl{};
	nfl.params = params;
	nfl.fontHandles = blocks[1];
	nfl.fontStates = blocks[2];
	nfl.hRes = lib->hRes;
	nfl.vRes = lib->vRes;
	nfl.internalFontCount = kNumInternalFonts;
	nfl.internalFontStyles = blocks[3];
	nfl.altCharCode = (u16)lib->altChar;
	mem::WriteBytes(blocks[0], &nfl, sizeof(nfl));
	if (blocks[1]) mem::Memset(blocks[1], 0, sizes[1]);
	if (blocks[2]) mem::Memset(blocks[2], 0, sizes[2]);
	for (u32 i = 0; i < kNumInternalFonts; i++)
		mem::WriteBytes(blocks[3] + i * kStyleBlockSize, &g_internal[i].style, kStyleBlockSize);

	u32 handle = blocks[0];
	g_libs[handle] = std::move(lib);
	LOGD("FONT", "sceFontNewLib(%08x, %08x) = %08x (%u fonts)", paramPtr, errorPtr, handle, n);
	Delay(kNewLibDelayUs);
	return handle;
}

int sceFontDoneLib(u32 libHandle) {
	ClearStack(0x2C);
	FontLib *lib = FindLiveLib(libHandle);
	if (!lib) {
		LOGW("FONT", "sceFontDoneLib(%08x): not a font library", libHandle);
		return 0;
	}
	for (Slot &s : lib->slots)
		if (s.refs > 0) ReleaseSlot(*lib, s);
	FlushCharBuffers(*lib);
	FontNewLibParams p = LiveParams(*lib);
	lib->handle = 0;
	lib->slots.clear();
	// The order the firmware frees them in (pspautotests font/fonttest).
	for (int i : {3, 1, 2, 0}) GameFree(p, lib->blocks[i]);
	memset(lib->blocks, 0, sizeof(lib->blocks));
	LOGD("FONT", "sceFontDoneLib(%08x)", libHandle);
	return 0;
}

int sceFontSetResolution(u32 libHandle, float hRes, float vRes) {
	FontLib *lib = FindLib(libHandle);
	if (!lib) return (int)SCE_FONT_ERROR_INVALID_LIBID;
	if (hRes <= 0.0f || vRes <= 0.0f) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	lib->hRes = hRes;
	lib->vRes = vRes;
	// The firmware writes the struct even after sceFontDoneLib freed it (pspautotests
	// font/resolution reads the values back from there).
	if (mem::IsValidRange(lib->addr, sizeof(NativeFontLib))) {
		GWrite<float>(lib->addr + offsetof(NativeFontLib, hRes), hRes);
		GWrite<float>(lib->addr + offsetof(NativeFontLib, vRes), vRes);
	}
	return 0;
}

int sceFontSetAltCharacterCode(u32 libHandle, u32 charCode) {
	FontLib *lib = FindLib(libHandle);
	if (!lib) return (int)SCE_FONT_ERROR_INVALID_LIBID;
	lib->altChar = charCode & 0xFFFF;
	if (mem::IsValidRange(lib->addr, sizeof(NativeFontLib)))
		GWrite16(lib->addr + offsetof(NativeFontLib, altCharCode), (u16)lib->altChar);
	return 0;
}

int sceFontGetNumFontList(u32 libHandle, u32 errorPtr) {
	if (!mem::IsValidRange(errorPtr, 4)) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	FontLib *lib = FindLib(libHandle);
	if (!lib) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_LIBID);
		return 0;
	}
	SetError(errorPtr, 0);
	return lib->handle ? (int)kNumInternalFonts : 0;
}

// Writes the firmware font styles regardless of what the library has open.
int sceFontGetFontList(u32 libHandle, u32 stylesPtr, int numFonts) {
	ClearStack(0x31C);
	FontLib *lib = FindLib(libHandle);
	if (!lib) return (int)SCE_FONT_ERROR_INVALID_LIBID;
	if (!stylesPtr || !mem::IsValidAddress(stylesPtr)) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	if (lib->handle) {
		int n = std::min(numFonts, (int)kNumInternalFonts);
		for (int i = 0; i < n; i++) {
			u32 addr = stylesPtr + (u32)i * kStyleBlockSize;
			if (!mem::IsValidRange(addr, kStyleBlockSize)) break;
			mem::WriteBytes(addr, &g_internal[i].style, kStyleBlockSize);
		}
	}
	Delay(kFontListDelayUs);
	return 0;
}

// "FontInfo" is a misnomer: this returns a firmware font's style, like sceFontGetFontList.
int sceFontGetFontInfoByIndexNumber(u32 libHandle, u32 stylePtr, u32 index) {
	ClearStack(0x20);
	FontLib *lib = FindLib(libHandle);
	if (!lib) return (int)SCE_FONT_ERROR_INVALID_LIBID;
	if (!lib->handle || index >= kNumInternalFonts || !mem::IsValidRange(stylePtr, kStyleBlockSize))
		return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	mem::WriteBytes(stylePtr, &g_internal[index].style, kStyleBlockSize);
	return 0;
}

// ---------------------------------------------------------------- style matching

enum class Match { Unknown, None, Good };

// Every field set in the request must match; Good if at least one did.
Match MatchStyle(const FontStyle &font, const FontStyle &req) {
	Match m = Match::Unknown;
	auto field = [&](u16 want, u16 have) {
		if (want == 0 || m == Match::None) return;
		m = want == have ? Match::Good : Match::None;
	};
	auto text = [&](const char *want, const char *have, size_t n) {
		if (want[0] == '\0' || m == Match::None) return;
		m = strncmp(want, have, n) == 0 ? Match::Good : Match::None;
	};
	field(req.fontFamily, font.fontFamily);
	field(req.fontStyle, font.fontStyle);
	field(req.fontLanguage, font.fontLanguage);
	field(req.fontCountry, font.fontCountry);
	text(req.fontName, font.fontName, sizeof(req.fontName));
	text(req.fontFileName, font.fontFileName, sizeof(req.fontFileName));
	return m;
}

// The index of the last font matching every given field, unless sizes were given: then the one
// nearest in pixel size (the first of equals). Index 0 when nothing matches.
int sceFontFindOptimumFont(u32 libHandle, u32 stylePtr, u32 errorPtr) {
	ClearStack(0xF0);
	if (!mem::IsValidRange(errorPtr, 4)) return (int)SCE_KERNEL_ERROR_INVALID_ARGUMENT;
	FontLib *lib = FindLib(libHandle);
	if (!lib) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_LIBID);
		return 0;
	}
	if (!mem::IsValidRange(stylePtr, kStyleBlockSize)) {
		// Really INVALID_LIBID (pspautotests font/optimum).
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_LIBID);
		return 0;
	}
	FontStyle req = GRead<FontStyle>(stylePtr);
	InitInternalFonts();
	const float hRes = req.fontHRes > 0.0f ? req.fontHRes : lib->hRes;
	const float vRes = req.fontVRes > 0.0f ? req.fontVRes : lib->vRes;
	int optimum = -1, nearest = -1;
	float nearestDist = std::numeric_limits<float>::infinity();
	for (u32 i = 0; i < kNumInternalFonts; i++) {
		const FontStyle &fs = g_internal[i].style;
		Match q = MatchStyle(fs, req);
		if (q != Match::None) {
			if (req.fontH > 0.0f) {
				float d = std::fabs(fs.fontHRes * fs.fontH - hRes * req.fontH);
				if (d < nearestDist) {
					nearestDist = d;
					nearest = (int)i;
				}
			}
			if (req.fontV > 0.0f) {
				// Compares against the requested H size, as the firmware does.
				float d = std::fabs(fs.fontVRes * fs.fontV - vRes * req.fontH);
				if (d < nearestDist) {
					nearestDist = d;
					nearest = (int)i;
				}
			}
		}
		if (q == Match::Good) optimum = (int)i;
	}
	if (nearest >= 0) optimum = nearest;
	SetError(errorPtr, 0);
	return optimum >= 0 ? optimum : 0;
}

// The first font matching every given field exactly, including the H size; -1 if none. A V size
// alone never matches.
int sceFontFindFont(u32 libHandle, u32 stylePtr, u32 errorPtr) {
	ClearStack(0x40);
	if (!mem::IsValidRange(errorPtr, 4)) return (int)SCE_KERNEL_ERROR_INVALID_ARGUMENT;
	FontLib *lib = FindLib(libHandle);
	if (!lib) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_LIBID);
		return 0;
	}
	if (!mem::IsValidRange(stylePtr, kStyleBlockSize)) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_PARAMETER);
		return 0;
	}
	FontStyle req = GRead<FontStyle>(stylePtr);
	InitInternalFonts();
	const float hRes = req.fontHRes > 0.0f ? req.fontHRes : lib->hRes;
	SetError(errorPtr, 0);
	for (u32 i = 0; i < kNumInternalFonts; i++) {
		const FontStyle &fs = g_internal[i].style;
		if (MatchStyle(fs, req) == Match::None) continue;
		if (req.fontH > 0.0f) {
			if (std::fabs(fs.fontHRes * fs.fontH - hRes * req.fontH) > 0.001f) continue;
		} else if (req.fontV > 0.0f) {
			continue;
		}
		return (int)i;
	}
	return -1;
}

// ---------------------------------------------------------------- opening fonts

// Open a firmware font. Mode 1 loads it whole; anything else keeps only its tables in memory
// ("stingy") and reads glyphs as they are used.
u32 sceFontOpen(u32 libHandle, u32 index, u32 mode, u32 errorPtr) {
	ClearStack(0x460);
	FontLib *lib = FindLiveLib(libHandle);
	if (!lib) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_LIBID);
		return 0;
	}
	if (index >= kNumInternalFonts) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_PARAMETER);
		return 0;
	}
	const std::string key = "internal:" + std::to_string(index);
	bool existing;
	u32 error = 0;
	Slot *slot = AcquireSlot(*lib, key, existing, error);
	if (!slot) {
		SetError(errorPtr, error);
		return 0;
	}
	if (existing) {
		slot->refs++;
		SetError(errorPtr, 0);
		return slot->handle;
	}

	const InternalFont &f = LoadInternalFont(index);
	const RegistryEntry &e = kRegistry[index];
	const bool full = mode == 1;
	FontNewLibParams p = LiveParams(*lib);
	std::vector<u32> allocations;
	bool allocated;
	if (!full && !f.data->IsBlank()) {
		allocated = AllocTables(p, f.data->Layout(), allocations);
	} else {
		// A blank font has no tables to split the stingy size into.
		u32 block = GameAlloc(p, full ? kFontFileExtra + (f.fileSize ? f.fileSize : e.fileSize) : e.stingySize);
		allocated = block != 0;
		if (block) allocations = {block};
	}
	if (!allocated) {
		SetError(errorPtr, SCE_FONT_ERROR_OUT_OF_MEMORY);
		return 0;
	}
	FillSlot(*slot, key, full ? OpenMode::InternalFull : OpenMode::InternalStingy, f.data, f.style,
	         std::move(allocations));
	SetError(errorPtr, 0);
	LOGD("FONT", "sceFontOpen(%08x, %u, %u) = %08x", libHandle, index, mode, slot->handle);
	Delay(kOpenDelayUs);
	return slot->handle;
}

u32 sceFontOpenUserMemory(u32 libHandle, u32 dataPtr, u32 dataSize, u32 errorPtr) {
	ClearStack(0x440);
	if (!dataPtr || !mem::IsValidAddress(dataPtr)) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_PARAMETER);
		return 0;
	}
	FontLib *lib = FindLiveLib(libHandle);
	if (!lib) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_LIBID);
		return 0;
	}
	if (dataSize == 0) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_PARAMETER);
		return 0;
	}
	char keyBuf[32];
	snprintf(keyBuf, sizeof(keyBuf), "mem:%08x", dataPtr);
	const std::string key = keyBuf;
	bool existing;
	u32 error = 0;
	Slot *slot = AcquireSlot(*lib, key, existing, error);
	if (!slot) {
		SetError(errorPtr, error);
		return 0;
	}
	if (existing) {
		slot->refs++;
		SetError(errorPtr, 0);
		return slot->handle;
	}

	// Games may overstate the size (even pass -1); use what is actually there.
	u32 size = ValidLength(dataPtr, std::min(dataSize, 0x02000000u));
	std::vector<u8> data(size);
	mem::ReadBytes(data.data(), dataPtr, size);
	auto pgf = std::make_shared<PgfFont>();
	if (!pgf->Parse(std::move(data))) {
		LOGW("FONT", "sceFontOpenUserMemory(%08x, %08x, %u): invalid font data", libHandle, dataPtr, dataSize);
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_FONT_DATA);
		return 0;
	}
	u32 block = GameAlloc(LiveParams(*lib), kFontFileExtra);
	if (!block) {
		SetError(errorPtr, SCE_FONT_ERROR_OUT_OF_MEMORY);
		return 0;
	}
	FontStyle style = StyleFromFont(*pgf);
	FillSlot(*slot, key, OpenMode::UserMemory, std::move(pgf), style, {block});
	SetError(errorPtr, 0);
	return slot->handle;
}

// Reads a user font through the game's file callbacks, the way the firmware does: header, then
// each table into its own allocation, then close (pspautotests font/openfile).
// The firmware then reads glyphs on demand; here the glyph data is taken from the same file
// through the PSP file system, or through the callbacks again if that isn't possible.
struct HandlerIo {
	FontNewLibParams p;
	u32 handle = 0;
	u32 errPtr = 0;
	bool Seek(u32 pos) { return CallGame(p.seekFunc, {p.userData, handle, pos}) == 0; }
	// Returns the number of elements read.
	u32 Read(u32 dst, u32 size, u32 count) {
		GWrite32(errPtr, 0);
		u32 got = CallGame(p.readFunc, {p.userData, handle, dst, size, count, errPtr});
		return GRead32(errPtr) == 0 ? got : 0;
	}
	bool ReadAll(u32 dst, u32 size, u32 count) { return count == 0 || Read(dst, size, count) == count; }
	bool Open(u32 namePtr) {
		GWrite32(errPtr, 0);
		handle = CallGame(p.openFunc, {p.userData, namePtr, errPtr});
		return GRead32(errPtr) == 0;
	}
	void Close() { CallGame(p.closeFunc, {p.userData, handle}); }
};

u32 OpenViaHandlers(FontLib &lib, Slot &slot, u32 namePtr, const char *fileName, std::shared_ptr<PgfFont> &out,
                    std::vector<u32> &allocations) {
	HandlerIo io{LiveParams(lib)};
	// The slot's state block (game memory) holds the header and the callbacks' error word.
	const u32 state = lib.blocks[2] + (u32)(&slot - lib.slots.data()) * kFontStateBlockSize;
	const u32 headerBuf = state;
	io.errPtr = state + 0x200;
	if (!io.Open(namePtr)) return SCE_FONT_ERROR_HANDLER_OPEN_FAILED;

	u32 err = 0;
	PgfLayout layout;
	std::vector<u32> tableBlocks;  // in free order
	u32 blocks[TA_COUNT] = {}, sizes[TA_COUNT] = {};
	do {
		if (!io.Seek(0)) {
			err = FONT_ERROR_HANDLER_SEEK_FAILED;
			break;
		}
		if (!io.ReadAll(headerBuf, 12, 1)) {
			err = FONT_ERROR_HANDLER_READ_FAILED;
			break;
		}
		u32 headerSize = GRead16(headerBuf + 2);
		if (headerSize < kPgfHeaderBytes || headerSize > 0x200 - 4) {
			err = SCE_FONT_ERROR_INVALID_FONT_DATA;
			break;
		}
		if (!io.ReadAll(headerBuf + 12, headerSize - 12, 1)) {
			err = FONT_ERROR_HANDLER_READ_FAILED;
			break;
		}
		if ((s32)GRead32(headerBuf + 8) == 3 && headerSize < kPgfHeaderBytes + kPgfRev3ExtraBytes) {
			u32 rest = kPgfHeaderBytes + kPgfRev3ExtraBytes - headerSize;
			if (!io.ReadAll(headerBuf + headerSize, rest, 1)) {
				err = FONT_ERROR_HANDLER_READ_FAILED;
				break;
			}
		}
		if (!ReadPgfLayout(GPtr(headerBuf), 0x200 - 4, layout)) {
			err = SCE_FONT_ERROR_INVALID_FONT_DATA;
			break;
		}
		if (!io.Seek(layout.headerBytes) || !io.Seek(layout.headerBytes)) {
			err = FONT_ERROR_HANDLER_SEEK_FAILED;
			break;
		}
		if (!AllocTables(io.p, layout, tableBlocks, &blocks)) {
			err = SCE_FONT_ERROR_OUT_OF_MEMORY;
			break;
		}
		TableSizes(layout, sizes);
		bool ok = true;
		for (int i = TA_DIM; i <= TA_ADV; i++) ok = ok && io.ReadAll(blocks[i], 8, sizes[i] / 8);
		if (ok && !io.Seek(layout.headerBytes + layout.MetricTableBytes())) {
			err = FONT_ERROR_HANDLER_SEEK_FAILED;
			break;
		}
		for (int i = TA_SHADOW; i < TA_COUNT; i++)
			if (sizes[i]) ok = ok && io.ReadAll(blocks[i], sizes[i], 1);
		if (!ok) err = FONT_ERROR_HANDLER_READ_FAILED;
	} while (false);
	io.Close();
	if (err) {
		for (u32 b : tableBlocks) GameFree(io.p, b);
		return err;
	}

	// The header and tables as read, then the glyph data.
	const u32 glyphOffset = layout.GlyphDataOffset();
	std::vector<u8> image(glyphOffset);
	mem::ReadBytes(image.data(), headerBuf, layout.headerBytes);
	u32 off = layout.headerBytes;
	for (int i = 0; i < TA_COUNT; off += sizes[i], i++)
		if (sizes[i]) mem::ReadBytes(image.data() + off, blocks[i], sizes[i]);
	std::vector<u8> file;
	if (io::ReadFile(fileName, file) && file.size() >= glyphOffset &&
	    memcmp(file.data(), image.data(), glyphOffset) == 0) {
		image.insert(image.end(), file.begin() + glyphOffset, file.end());
	} else {
		// Not a file the PSP file system knows (the game's callbacks may read an archive):
		// read the rest through the callbacks, halving the request whenever one fails.
		LOGI("FONT", "%s: reading the glyph data through the game's file callbacks", fileName);
		const u32 chunkMax = 0x8000;
		u32 chunk = GameAlloc(io.p, chunkMax);
		bool ok = chunk && io.Open(namePtr);
		if (ok) {
			u32 pos = glyphOffset, want = chunkMax;
			while (want > 0 && image.size() < 0x04000000) {
				u32 got = io.Seek(pos) ? io.Read(chunk, 1, want) : 0;
				if (got > want) got = 0;
				if (got) {
					size_t old = image.size();
					image.resize(old + got);
					mem::ReadBytes(image.data() + old, chunk, got);
					pos += got;
				}
				if (got < want) want = got ? 0 : want / 2;
			}
			io.Close();
		}
		GameFree(io.p, chunk);
	}

	auto pgf = std::make_shared<PgfFont>();
	if (!pgf->Parse(std::move(image))) {
		for (u32 b : tableBlocks) GameFree(io.p, b);
		return SCE_FONT_ERROR_INVALID_FONT_DATA;
	}
	out = std::move(pgf);
	allocations = std::move(tableBlocks);
	return 0;
}

// Open a font file. Mode 1 reads it whole through the file system; other modes go through the
// game's file callbacks and keep only the tables in memory.
u32 sceFontOpenUserFile(u32 libHandle, u32 fileNamePtr, u32 mode, u32 errorPtr) {
	const char *fileName = fileNamePtr ? mem::GetCString(fileNamePtr) : nullptr;
	if (!fileName) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_PARAMETER);
		return 0;
	}
	FontLib *lib = FindLiveLib(libHandle);
	if (!lib) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_LIBID);
		return 0;
	}
	const bool handlers = mode != 1;
	if (handlers) {
		FontNewLibParams p = LiveParams(*lib);
		if (!ValidFunc(p.openFunc) || !ValidFunc(p.closeFunc) || !ValidFunc(p.readFunc) || !ValidFunc(p.seekFunc)) {
			SetError(errorPtr, SCE_FONT_ERROR_INVALID_PARAMETER);
			return 0;
		}
	}
	const std::string name = fileName;  // the guest string may move under the callbacks
	const std::string key = FileKey(name.c_str());
	bool existing;
	u32 error = 0;
	Slot *slot = AcquireSlot(*lib, key, existing, error);
	if (!slot) {
		SetError(errorPtr, error);
		return 0;
	}
	if (existing) {
		slot->refs++;
		SetError(errorPtr, 0);
		return slot->handle;
	}

	std::shared_ptr<PgfFont> pgf;
	std::vector<u32> allocations;
	if (handlers) {
		error = OpenViaHandlers(*lib, *slot, fileNamePtr, name.c_str(), pgf, allocations);
	} else {
		std::vector<u8> data;
		Delay(kOpenDelayUs);
		if (!io::ReadFile(name, data)) {
			error = SCE_FONT_ERROR_HANDLER_OPEN_FAILED;
		} else {
			u32 fileSize = (u32)data.size();
			pgf = std::make_shared<PgfFont>();
			if (!pgf->Parse(std::move(data))) {
				error = SCE_FONT_ERROR_INVALID_FONT_DATA;
			} else {
				u32 block = GameAlloc(LiveParams(*lib), kFontFileExtra + fileSize);
				if (block)
					allocations = {block};
				else
					error = SCE_FONT_ERROR_OUT_OF_MEMORY;
			}
		}
	}
	if (error) {
		LOGW("FONT", "sceFontOpenUserFile(%08x, %s, %d) failed: %08x", libHandle, name.c_str(), (int)mode, error);
		SetError(errorPtr, error);
		return 0;
	}
	FontStyle style = StyleFromFont(*pgf);
	FillSlot(*slot, key, handlers ? OpenMode::UserHandlers : OpenMode::UserFile, std::move(pgf), style,
	         std::move(allocations));
	SetError(errorPtr, 0);
	LOGD("FONT", "sceFontOpenUserFile(%08x, %s, %d) = %08x", libHandle, name.c_str(), (int)mode, slot->handle);
	return slot->handle;
}

int sceFontClose(u32 fontHandle) {
	ClearStack(0x54);
	FontLib *lib = nullptr;
	Slot *s = FindFont(fontHandle, false, &lib);
	if (!s) {
		LOGW("FONT", "sceFontClose(%08x): font not open", fontHandle);
		return 0;
	}
	if (--s->refs == 0) ReleaseSlot(*lib, *s);
	FlushCharBuffers(*lib);
	return 0;
}

int sceFontFlush(u32 fontHandle) {
	FontLib *lib = nullptr;
	if (!FindFont(fontHandle, true, &lib)) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	FlushCharBuffers(*lib);
	return 0;
}

// ---------------------------------------------------------------- font queries

int sceFontGetFontInfo(u32 fontHandle, u32 infoPtr) {
	if (!mem::IsValidRange(infoPtr, kFontInfoWrittenBytes)) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	Slot *s = FindFont(fontHandle, true);
	if (!s) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	FontInfo info{};
	s->font->GetFontInfo(info);
	info.fontStyle = s->style;
	mem::WriteBytes(infoPtr, &info, kFontInfoWrittenBytes);
	return 0;
}

int CharInfoCommon(u32 fontHandle, u32 charCode, u32 infoPtr, GlyphKind kind) {
	charCode &= 0xFFFF;
	if (!mem::IsValidRange(infoPtr, sizeof(CharInfo))) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	FontLib *lib = nullptr;
	Slot *s = FindFont(fontHandle, true, &lib);
	if (!s) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	CharInfo ci;
	s->font->GetCharInfo((int)charCode, (int)lib->altChar, kind, ci);
	mem::WriteBytes(infoPtr, &ci, sizeof(ci));
	TouchGlyph(*s, (int)charCode);
	if (kind == GlyphKind::Shadow) return 0;

	// The library allocates a bitmap buffer for the character (JPCSP's findings, as PPSSPP has
	// them).
	if (ci.sfp26AdvanceH == 0 && ci.sfp26AdvanceV == 0) return 0;
	if (!AllocCharBuffers(*lib, {ci.bitmapWidth * ci.bitmapHeight})) return (int)SCE_FONT_ERROR_OUT_OF_MEMORY;
	return 0;
}

int sceFontGetCharInfo(u32 fontHandle, u32 charCode, u32 infoPtr) {
	ClearStack(0x110);
	return CharInfoCommon(fontHandle, charCode, infoPtr, GlyphKind::Char);
}

int sceFontGetShadowInfo(u32 fontHandle, u32 charCode, u32 infoPtr) {
	ClearStack(0x150);
	return CharInfoCommon(fontHandle, charCode, infoPtr, GlyphKind::Shadow);
}

int ImageRectCommon(u32 fontHandle, u32 charCode, u32 rectPtr, GlyphKind kind) {
	charCode &= 0xFFFF;
	FontLib *lib = nullptr;
	Slot *s = FindFont(fontHandle, true, &lib);
	if (!s) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	if (!mem::IsValidRange(rectPtr, 4)) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	CharInfo ci;
	s->font->GetCharInfo((int)charCode, (int)lib->altChar, kind, ci);
	TouchGlyph(*s, (int)charCode);
	GWrite16(rectPtr, (u16)ci.bitmapWidth);
	GWrite16(rectPtr + 2, (u16)ci.bitmapHeight);
	return 0;
}

int sceFontGetCharImageRect(u32 fontHandle, u32 charCode, u32 rectPtr) {
	ClearStack(0x120);
	return ImageRectCommon(fontHandle, charCode, rectPtr, GlyphKind::Char);
}

int sceFontGetShadowImageRect(u32 fontHandle, u32 charCode, u32 rectPtr) {
	ClearStack(0x150);
	return ImageRectCommon(fontHandle, charCode, rectPtr, GlyphKind::Shadow);
}

int GlyphImageCommon(u32 fontHandle, u32 charCode, u32 imagePtr, int clipX, int clipY, int clipW, int clipH,
                     GlyphKind kind) {
	charCode &= 0xFFFF;
	if (!mem::IsValidRange(imagePtr, sizeof(GlyphImage))) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	FontLib *lib = nullptr;
	Slot *s = FindFont(fontHandle, true, &lib);
	if (!s) return (int)SCE_FONT_ERROR_INVALID_PARAMETER;
	GlyphImage img = GRead<GlyphImage>(imagePtr);
	TouchGlyph(*s, (int)charCode);
	// A font without its glyph data reads the glyph record into one buffer and decodes it into
	// another (pspautotests font/fonttest).
	if (s->mode == OpenMode::InternalStingy || s->mode == OpenMode::UserHandlers) {
		u32 recordBytes = 0, bitmapBytes = 0;
		s->font->GlyphBufferSizes((int)charCode, (int)lib->altChar, kind, recordBytes, bitmapBytes);
		if (!AllocCharBuffers(*lib, {recordBytes, bitmapBytes})) return (int)SCE_FONT_ERROR_OUT_OF_MEMORY;
	}
	s->font->Draw(img, clipX, clipY, clipW, clipH, (int)charCode, (int)lib->altChar, kind);
	return 0;
}

int sceFontGetCharGlyphImage(u32 fontHandle, u32 charCode, u32 imagePtr) {
	ClearStack(0x120);
	return GlyphImageCommon(fontHandle, charCode, imagePtr, -1, -1, -1, -1, GlyphKind::Char);
}

int sceFontGetCharGlyphImage_Clip(u32 fontHandle, u32 charCode, u32 imagePtr, int clipX, int clipY, int clipW,
                                  int clipH) {
	ClearStack(0x130);
	return GlyphImageCommon(fontHandle, charCode, imagePtr, clipX, clipY, clipW, clipH, GlyphKind::Char);
}

int sceFontGetShadowGlyphImage(u32 fontHandle, u32 charCode, u32 imagePtr) {
	ClearStack(0x160);
	return GlyphImageCommon(fontHandle, charCode, imagePtr, -1, -1, -1, -1, GlyphKind::Shadow);
}

int sceFontGetShadowGlyphImage_Clip(u32 fontHandle, u32 charCode, u32 imagePtr, int clipX, int clipY, int clipW,
                                    int clipH) {
	ClearStack(0x170);
	return GlyphImageCommon(fontHandle, charCode, imagePtr, clipX, clipY, clipW, clipH, GlyphKind::Shadow);
}

// ---------------------------------------------------------------- unit conversion

// Pixels <-> points at the library's resolution (dpi).
float Convert(u32 libHandle, float value, u32 errorPtr, bool horizontal, bool toPoints) {
	if (!mem::IsValidRange(errorPtr, 4)) return 0.0f;
	FontLib *lib = FindLib(libHandle);
	if (!lib) {
		SetError(errorPtr, SCE_FONT_ERROR_INVALID_LIBID);
		return 0.0f;
	}
	SetError(errorPtr, 0);
	const float res = horizontal ? lib->hRes : lib->vRes;
	return toPoints ? value * kPointDpi / res : value * res / kPointDpi;
}

float sceFontPixelToPointH(u32 libHandle, float pixels, u32 errorPtr) {
	ClearStack(0x10);
	return Convert(libHandle, pixels, errorPtr, true, true);
}
float sceFontPixelToPointV(u32 libHandle, float pixels, u32 errorPtr) {
	ClearStack(0x10);
	return Convert(libHandle, pixels, errorPtr, false, true);
}
float sceFontPointToPixelH(u32 libHandle, float points, u32 errorPtr) {
	return Convert(libHandle, points, errorPtr, true, false);
}
float sceFontPointToPixelV(u32 libHandle, float points, u32 errorPtr) {
	return Convert(libHandle, points, errorPtr, false, false);
}

int sceFontCalcMemorySize() {
	LOGW("FONT", "sceFontCalcMemorySize: not implemented");
	return 0;
}

const hle::HleFunction kSceLibFont[] = {
	HLEFN(0x67F17ED7, sceFontNewLib),
	HLEFN(0x574B6FBC, sceFontDoneLib),
	HLEFN(0x48293280, sceFontSetResolution),
	HLEFN(0x27F6E642, sceFontGetNumFontList),
	HLEFN(0xBC75D85B, sceFontGetFontList),
	HLEFN(0x099EF33C, sceFontFindOptimumFont),
	HLEFN(0x681E61A7, sceFontFindFont),
	HLEFN(0x2F67356A, sceFontCalcMemorySize),
	HLEFN(0x5333322D, sceFontGetFontInfoByIndexNumber),
	HLEFN(0xA834319D, sceFontOpen),
	HLEFN(0x57FCB733, sceFontOpenUserFile),
	HLEFN(0xBB8E7FE6, sceFontOpenUserMemory),
	HLEFN(0x3AEA8CB6, sceFontClose),
	HLEFN(0x0DA7535E, sceFontGetFontInfo),
	HLEFN(0xDCC80C2F, sceFontGetCharInfo),
	HLEFN(0xAA3DE7B5, sceFontGetShadowInfo),
	HLEFN(0x5C3E4A9E, sceFontGetCharImageRect),
	HLEFN(0x48B06520, sceFontGetShadowImageRect),
	HLEFN(0x980F4895, sceFontGetCharGlyphImage),
	HLEFN(0xCA1E6945, sceFontGetCharGlyphImage_Clip),
	HLEFN(0x74B21701, sceFontPixelToPointH),
	HLEFN(0xF8F0752E, sceFontPixelToPointV),
	HLEFN(0x472694CD, sceFontPointToPixelH),
	HLEFN(0x3C4B7E82, sceFontPointToPixelV),
	HLEFN(0xEE232411, sceFontSetAltCharacterCode),
	HLEFN(0x568BE516, sceFontGetShadowGlyphImage),
	HLEFN(0x5DCF6858, sceFontGetShadowGlyphImage_Clip),
	HLEFN(0x02D7F94B, sceFontFlush),
	HLESTUB(0x33FFD07C, "sceFontIsElement"),
};

}  // namespace

void Register_FontLibraries() {
	hle::RegisterModule("sceLibFont", kSceLibFont);
	// The same library under another name, used by some games (PPSSPP registers it too).
	hle::RegisterModule("sceLibFttt", kSceLibFont);
}
