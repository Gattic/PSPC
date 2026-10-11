// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/Dialog/PSPOskDialog.cpp, PSPOskConstants.cpp), Copyright (c) 2012-
// PPSSPP Project, GPL-2.0-or-later.
#include "runtime/dialogs/osk_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/hle/display.h"
#include "runtime/kernel/errors.h"

namespace utility {

namespace {

// --osk-text: what the keyboard returns headless, one answer per keyboard in turn ("a|b|c"; the last
// one answers the rest).
std::vector<std::string> g_headlessOskTexts;
size_t g_headlessOskNext = 0;

constexpr int OSK_INIT_DELAY_US = 300000;
constexpr int OSK_SHUTDOWN_DELAY_US = 40000;
constexpr s32 OSK_RESULT_CHANGED = 2;

// ---- keyboards (PSPOskConstants.cpp)

enum OskKeyboardDisplay {
	OSK_KEYBOARD_LATIN_LOWERCASE,
	OSK_KEYBOARD_LATIN_UPPERCASE,
	OSK_KEYBOARD_HIRAGANA,
	OSK_KEYBOARD_KATAKANA,
	OSK_KEYBOARD_KOREAN,
	OSK_KEYBOARD_RUSSIAN_LOWERCASE,
	OSK_KEYBOARD_RUSSIAN_UPPERCASE,
	OSK_KEYBOARD_LATIN_FW_LOWERCASE,
	OSK_KEYBOARD_LATIN_FW_UPPERCASE,
	OSK_KEYBOARD_COUNT
};

enum OskKeyboardLanguage {
	OSK_LANGUAGE_ENGLISH,  // half-width
	OSK_LANGUAGE_JAPANESE,
	OSK_LANGUAGE_KOREAN,
	OSK_LANGUAGE_RUSSIAN,
	OSK_LANGUAGE_ENGLISH_FW,  // full-width (mostly used in Japanese games)
	OSK_LANGUAGE_COUNT
};

enum { LOWERCASE, UPPERCASE };

const int kKeyboardCases[OSK_LANGUAGE_COUNT][2] = {
	{OSK_KEYBOARD_LATIN_LOWERCASE, OSK_KEYBOARD_LATIN_UPPERCASE},
	{OSK_KEYBOARD_HIRAGANA, OSK_KEYBOARD_KATAKANA},
	{OSK_KEYBOARD_KOREAN, OSK_KEYBOARD_KOREAN},  // Korean only has one case
	{OSK_KEYBOARD_RUSSIAN_LOWERCASE, OSK_KEYBOARD_RUSSIAN_UPPERCASE},
	{OSK_KEYBOARD_LATIN_FW_LOWERCASE, OSK_KEYBOARD_LATIN_FW_UPPERCASE},
};

const char *const kLanguageNames[OSK_LANGUAGE_COUNT] = {"English", "Japanese", "Korean", "Russian", "English Full-width"};

const u8 kNumKeyCols[OSK_KEYBOARD_COUNT] = {12, 12, 13, 13, 12, 12, 12, 12, 12};
const u8 kNumKeyRows[OSK_KEYBOARD_COUNT] = {5, 5, 6, 6, 5, 4, 4, 4, 4};

// PSP_UTILITY_OSK_INPUTTYPE_*
enum : s32 {
	INPUTTYPE_LATIN_DIGIT = 0x00000001,
	INPUTTYPE_LATIN_SYMBOL = 0x00000002,
	INPUTTYPE_LATIN_LOWERCASE = 0x00000004,
	INPUTTYPE_LATIN_UPPERCASE = 0x00000008,
	INPUTTYPE_JAPANESE_DIGIT = 0x00000100,
	INPUTTYPE_JAPANESE_SYMBOL = 0x00000200,
	INPUTTYPE_JAPANESE_LOWERCASE = 0x00000400,
	INPUTTYPE_JAPANESE_UPPERCASE = 0x00000800,
	INPUTTYPE_JAPANESE_HIRAGANA = 0x00001000,
	INPUTTYPE_JAPANESE_HALF_KATAKANA = 0x00002000,
	INPUTTYPE_JAPANESE_KATAKANA = 0x00004000,
	INPUTTYPE_RUSSIAN_LOWERCASE = 0x00010000,
	INPUTTYPE_RUSSIAN_UPPERCASE = 0x00020000,
	INPUTTYPE_KOREAN = 0x00040000,
};

// Which keyboards an input type allows (an approximation, enough to show the right ones).
const s32 kAllowedInputFlags[OSK_KEYBOARD_COUNT] = {
	INPUTTYPE_LATIN_LOWERCASE | INPUTTYPE_LATIN_UPPERCASE | INPUTTYPE_LATIN_SYMBOL | INPUTTYPE_LATIN_DIGIT,
	INPUTTYPE_LATIN_LOWERCASE | INPUTTYPE_LATIN_UPPERCASE | INPUTTYPE_LATIN_SYMBOL,
	INPUTTYPE_JAPANESE_HIRAGANA,
	INPUTTYPE_JAPANESE_KATAKANA | INPUTTYPE_JAPANESE_HALF_KATAKANA,
	INPUTTYPE_KOREAN,
	INPUTTYPE_RUSSIAN_LOWERCASE | INPUTTYPE_RUSSIAN_UPPERCASE,
	INPUTTYPE_RUSSIAN_LOWERCASE | INPUTTYPE_RUSSIAN_UPPERCASE,
	INPUTTYPE_JAPANESE_LOWERCASE | INPUTTYPE_JAPANESE_UPPERCASE | INPUTTYPE_JAPANESE_SYMBOL | INPUTTYPE_JAPANESE_DIGIT,
	INPUTTYPE_JAPANESE_LOWERCASE | INPUTTYPE_JAPANESE_UPPERCASE | INPUTTYPE_JAPANESE_SYMBOL,
};
const s32 kDefaultInputFlags[OSK_KEYBOARD_COUNT] = {
	INPUTTYPE_LATIN_LOWERCASE | INPUTTYPE_LATIN_SYMBOL | INPUTTYPE_LATIN_DIGIT,
	INPUTTYPE_LATIN_UPPERCASE | INPUTTYPE_LATIN_SYMBOL,
	INPUTTYPE_JAPANESE_HIRAGANA,
	INPUTTYPE_JAPANESE_KATAKANA | INPUTTYPE_JAPANESE_HALF_KATAKANA,
	INPUTTYPE_KOREAN,
	INPUTTYPE_RUSSIAN_LOWERCASE,
	INPUTTYPE_RUSSIAN_UPPERCASE,
	INPUTTYPE_JAPANESE_LOWERCASE | INPUTTYPE_JAPANESE_SYMBOL | INPUTTYPE_JAPANESE_DIGIT,
	INPUTTYPE_JAPANESE_UPPERCASE | INPUTTYPE_JAPANESE_SYMBOL,
};

const char16_t *const kOskKeys[OSK_KEYBOARD_COUNT][6] = {
	{
		// Latin lowercase
		u"1234567890-+",
		u"qwertyuiop[]",
		u"asdfghjkl;@~",
		u"zxcvbnm,./?\\",
		u"=<>'€¥&§£*()",
	},
	{
		// Latin uppercase
		u"!@#$%^&*()_+",
		u"QWERTYUIOP{}",
		u"ASDFGHJKL:\"`",
		u"ZXCVBNM<>/?|",
		u"=<>'€¥&§-[]",
	},
	{
		// Hiragana
		u"あかさたなはまやらわぁゃっ",
		u"いきしちにひみ　り　ぃ　　",
		u"うくすつぬふむゆるをぅゅ゛",
		u"えけせてねへめ　れ　ぇ　゜",
		u"おこそとのほもよろんぉょー",
		u"・。、「」『』〜     ",
	},
	{
		// Katakana
		u"アカサタナハマヤラワァャッ",
		u"イキシチニヒミ　リ　ィ　　",
		u"ウクスツヌフムユルヲゥュ゛",
		u"エケセテネヘメ　レ　ェ　゜",
		u"オコソトノホモヨロンォョー",
		u"・。、「」『』〜     ",
	},
	{
		// Korean (Hangul)
		u"1234567890-+",
		u"ㅃㅉㄸㄲㅆ!@#$%^&",
		u"ㅂㅈㄷㄱㅅㅛㅕㅑㅐㅔ[]",
		u"ㅁㄴㅇㄹㅎㅗㅓㅏㅣ;@~",
		u"ㅋㅌㅊㅍㅠㅜㅡ<>/?|",
	},
	{
		// Russian lowercase
		u"1234567890-+",
		u"йцукенгшщзхъ",
		u"фывапролджэё",
		u"ячсмитьбю/?|",
	},
	{
		// Russian uppercase
		u"!@#$%^&*()_+",
		u"ЙЦУКЕНГШЩЗХЪ",
		u"ФЫВАПРОЛДЖЭЁ",
		u"ЯЧСМИТЬБЮ/?|",
	},
	{
		// Latin full-width lowercase
		u"１２３４５６７８９０－＋",
		u"ｑｗｅｒｔｙｕｉｏｐ［］",
		u"ａｓｄｆｇｈｊｋｌ；＠～",
		u"ｚｘｃｖｂｎｍ，．／？￥￥",
	},
	{
		// Latin full-width uppercase
		u"！＠＃＄％＾＆＊（）＿＋",
		u"ＱＷＥＲＴＹＵＩＯＰ｛｝",
		u"ＡＳＤＦＧＨＪＫＬ：￥”‘",
		u"ＺＸＣＶＢＮＭ＜＞／？｜",
	},
};

char16_t OskKeyAt(int keyboard, int row, int col) {
	const char16_t *line = kOskKeys[keyboard][row];
	if (!line) return u' ';
	for (int i = 0; i < col; i++)
		if (!line[i]) return u' ';
	return line[col] ? line[col] : u' ';
}

// Japanese (kana) diacritics: pairs of (character, with the mark).
const char16_t *const kDiacritics[2] = {
	u"かがきぎくぐけげこごさざしじすずせぜそぞただちぢつづてでとどはばぱばひびぴびふぶぷぶへべぺべほぼぽぼウヴカガキギクグケゲコゴサザシジスズセゼソゾタダチヂツヅテデトドハバパバヒビピビフブプブヘベペベホボポボ",
	u"はぱばぱひぴびぴふぷぶぷへぺべぺほぽぼぽハパバパヒピビピフプブプヘペベペホポボポ",
};

// Korean (Hangul): leading consonants, vowels (unused ones are spaces), trailing consonants.
const char16_t *const kKorCons = u"ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ";
const char16_t *const kKorVowel = u"ㅏㅐㅑㅒㅓㅔㅕㅖㅗ   ㅛㅜ   ㅠㅡ ㅣ";
const char16_t *const kKorLCons = u"ㄱㄲㄳㄴㄵㄶㄷㄹㄺㄻㄼㄽㄾㄿㅀㅁㅂㅄㅅㅆㅇㅈㅊㅋㅌㅍㅎ";
// Vowel combinations, trailing consonant combinations and separations, as triples.
const u8 kKorVowelCom[21] = {0, 8, 9, 1, 8, 10, 20, 8, 11, 4, 13, 14, 5, 13, 15, 20, 13, 16, 20, 18, 19};
const u8 kKorLConsCom[33] = {18, 0, 2, 21, 3, 4, 26, 3, 5, 0, 7, 8, 15, 7, 9, 16, 7, 10, 18, 7, 11, 24, 7, 12, 25, 7, 13, 26, 7, 14, 18, 16, 17};
const u8 kKorLConsSpr[33] = {2, 1, 9, 4, 4, 12, 5, 4, 18, 8, 8, 0, 9, 8, 6, 10, 8, 7, 11, 8, 9, 12, 8, 16, 13, 8, 17, 14, 8, 18, 17, 17, 9};

int GetIndex(const char16_t *src, char16_t ch) {
	for (int i = 0; src[i]; i++)
		if (src[i] == ch) return i;
	return -1;
}

size_t Length(const char16_t *s) { return std::char_traits<char16_t>::length(s); }

void FindValidKeyboard(s32 inputType, int direction, int &lang, int &disp) {
	const int origLang = lang, origDisp = disp;
	if (inputType == 0) return;
	// direction 0 is the default, which actually moves "forward".
	const s32 *matchMap = kAllowedInputFlags;
	if (direction == 0) {
		direction = 1;
		matchMap = kDefaultInputFlags;
	}
	int tries = OSK_LANGUAGE_COUNT * 2;
	while (!(inputType & matchMap[disp]) && tries > 0) {
		if ((--tries % 2) == 0) {
			lang = (OSK_LANGUAGE_COUNT + lang + direction) % OSK_LANGUAGE_COUNT;
			disp = kKeyboardCases[lang][LOWERCASE];
		} else {
			disp = kKeyboardCases[lang][UPPERCASE];
		}
	}
	if (tries == 0) {
		// Fall back to allowing all.
		lang = origLang;
		disp = origDisp;
	}
}

bool IsKeyboardShiftValid(s32 inputType, int lang, int disp) {
	disp = disp == kKeyboardCases[lang][UPPERCASE] ? kKeyboardCases[lang][LOWERCASE] : kKeyboardCases[lang][UPPERCASE];
	return inputType == 0 || (inputType & kAllowedInputFlags[disp]) != 0;
}

std::u16string ReadU16String(u32 addr, u32 maxChars = 4096) {
	std::u16string s;
	if (!addr) return s;
	for (u32 i = 0; i < maxChars && mem::IsValidRange(addr + i * 2, 2); i++) {
		u16 c = GRead16(addr + i * 2);
		if (!c) break;
		s.push_back((char16_t)c);
	}
	return s;
}

std::u16string Utf8ToUtf16(const std::string &in) {
	std::u16string out;
	for (size_t i = 0; i < in.size();) {
		u8 b = (u8)in[i];
		u32 c;
		int n;
		if (b < 0x80) {
			c = b;
			n = 1;
		} else if ((b & 0xE0) == 0xC0) {
			c = b & 0x1F;
			n = 2;
		} else if ((b & 0xF0) == 0xE0) {
			c = b & 0x0F;
			n = 3;
		} else {
			c = b & 0x07;
			n = 4;
		}
		for (int k = 1; k < n && i + k < in.size(); k++) c = (c << 6) | ((u8)in[i + k] & 0x3F);
		i += n;
		if (c >= 0x10000) {
			c -= 0x10000;
			out.push_back((char16_t)(0xD800 + (c >> 10)));
			out.push_back((char16_t)(0xDC00 + (c & 0x3FF)));
		} else {
			out.push_back((char16_t)c);
		}
	}
	return out;
}

std::string Char(char16_t c) { return ppge::Utf16ToUtf8(std::u16string(1, c)); }

}  // namespace

DialogCommon *OskDialog::GetCommonParam() {
	if (!paramsAddr_ || !mem::IsValidRange(paramsAddr_, sizeof(DialogCommon))) return nullptr;
	return (DialogCommon *)GPtr(paramsAddr_);
}

int OskDialog::Init(u32 paramAddr) {
	if (GetStatus() != STATUS_NONE) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	const int check = CheckRequest(paramAddr, {0x40, 0x44});
	if (check < 0) return check;
	mem::ReadBytes(&params_, paramAddr, sizeof(params_));
	if (!mem::IsValidRange(params_.fields, sizeof(OskData))) {
		LOGE("UTIL", "sceUtilityOskInitStart: invalid field data (%08x)", params_.fields);
		return -1;
	}
	paramsAddr_ = paramAddr;
	if (params_.fieldCount != 1) LOGW("UTIL", "sceUtilityOskInitStart: unsupported field count %d", params_.fieldCount);

	ChangeStatusInit(OSK_INIT_DELAY_US);
	OskData field;
	mem::ReadBytes(&field, params_.fields, sizeof(field));
	inputChars_ = ReadU16String(field.intext);
	desc_ = ppge::Utf16ToUtf8(ReadU16String(field.desc));
	framesRun_ = 0;
	LOGI("UTIL", "OSK: \"%s\" (initial text \"%s\")", desc_.c_str(), ppge::Utf16ToUtf8(inputChars_).c_str());

	selectedChar_ = 0;
	currentKeyboardLanguage_ = OSK_LANGUAGE_ENGLISH;
	currentKeyboard_ = OSK_KEYBOARD_LATIN_LOWERCASE;
	FindValidKeyboard(field.inputtype, 0, currentKeyboardLanguage_, currentKeyboard_);
	isCombinated_ = false;
	iLevel_ = 0;
	std::fill(std::begin(held_), std::end(held_), 0);

	UpdateButtons();
	InitCommon();
	StartFade(true);
	return 0;
}

u32 OskDialog::FieldMaxLength() {
	const u32 length = GRead32(params_.fields + offsetof(OskData, outtextlength));
	const u32 limit = GRead32(params_.fields + offsetof(OskData, outtextlimit));
	if (length == 0) return 0;
	if (limit > length - 1 || limit == 0) return length - 1;
	return limit;
}

s32 OskDialog::FieldInputType() { return (s32)GRead32(params_.fields + offsetof(OskData, inputtype)); }

int OskDialog::Cols() const { return kNumKeyCols[currentKeyboard_]; }
int OskDialog::Rows() const { return kNumKeyRows[currentKeyboard_]; }

// Only writes the text and its terminator, not the rest of the buffer.
void OskDialog::WriteOutput(s32 fieldResult) {
	OskData field;
	mem::ReadBytes(&field, params_.fields, sizeof(field));
	size_t end = field.outtextlength;
	if (end > inputChars_.size()) end = inputChars_.size() + 1;
	if (end != 0 && mem::IsValidRange(field.outtext, (u32)(end * 2))) {
		u32 maxLen = FieldMaxLength();
		for (size_t i = 0; i < end; ++i) {
			u16 v = (i < maxLen && i < inputChars_.size()) ? (u16)inputChars_[i] : 0;
			GWrite16(field.outtext + (u32)i * 2, v);
		}
	}
	if (DialogCommon *c = GetCommonParam()) c->result = 0;
	GWrite32(params_.fields + offsetof(OskData, result), (u32)fieldResult);
}

std::u16string OskDialog::CombinationKorean(bool isInput) {
	std::u16string string;
	isCombinated_ = true;
	const int selectedRow = selectedChar_ / Cols();
	const int selectedCol = selectedChar_ % Cols();
	const u32 maxLen = FieldMaxLength();

	// The last character of the text is replaced with what typing `sw` makes of it.
	auto startNew = [&](char16_t prev, char16_t sw) {
		string += prev;
		if (inputChars_.size() < maxLen) {
			string += sw;
			if (isInput) {
				iValue_[0] = GetIndex(kKorCons, sw);
				iLevel_ = iValue_[0] != -1 ? 1 : 0;
			}
		} else {
			isCombinated_ = false;
		}
	};

	if (inputChars_.empty()) {
		char16_t sw = OskKeyAt(currentKeyboard_, selectedRow, selectedCol);
		if (inputChars_.size() < maxLen) {
			string += sw;
			iValue_[0] = GetIndex(kKorCons, sw);
			if (iValue_[0] != -1 && isInput) iLevel_ = 1;
		} else {
			isCombinated_ = false;
		}
		return string;
	}

	for (u32 i = 0; i + 1 < inputChars_.size(); i++) string += inputChars_[i];
	const char16_t last = inputChars_.back();
	const char16_t sw = OskKeyAt(currentKeyboard_, selectedRow, selectedCol);
	if (iLevel_ == 0) {
		string += last;
		if (inputChars_.size() < maxLen) {
			string += sw;
			iValue_[0] = GetIndex(kKorCons, sw);
			if (iValue_[0] != -1 && isInput) iLevel_ = 1;
		} else {
			isCombinated_ = false;
		}
	} else if (iLevel_ == 1) {
		iValue_[1] = GetIndex(kKorVowel, sw);
		if (iValue_[1] == -1) {
			startNew(last, sw);
		} else {
			string += (char16_t)(0xAC00 + iValue_[0] * 0x24C + iValue_[1] * 0x1C);
			if (isInput) iLevel_ = 2;
		}
	} else if (iLevel_ == 2) {
		const int tmp = GetIndex(kKorVowel, sw);
		if (tmp != -1) {
			int tmp2 = -1;
			for (size_t j = 0; j + 2 < sizeof(kKorVowelCom); j += 3) {
				if (kKorVowelCom[j] == tmp && kKorVowelCom[j + 1] == iValue_[1]) {
					tmp2 = kKorVowelCom[j + 2];
					break;
				}
			}
			if (tmp2 != -1) {
				if (isInput) iValue_[1] = tmp2;
				string += (char16_t)(0xAC00 + iValue_[0] * 0x24C + tmp2 * 0x1C);
			} else {
				string += last;
				if (inputChars_.size() < maxLen) {
					string += sw;
					if (isInput) iLevel_ = 0;
				} else {
					isCombinated_ = false;
				}
			}
		} else {
			const int tmp2 = GetIndex(kKorLCons, sw);
			if (tmp2 == -1) {
				startNew(last, sw);
			} else {
				string += (char16_t)(0xAC00 + iValue_[0] * 0x24C + iValue_[1] * 0x1C + tmp2 + 1);
				if (isInput) {
					iLevel_ = 3;
					iValue_[2] = tmp2;
				}
			}
		}
	} else if (iLevel_ == 3) {
		const int tmp = GetIndex(kKorLCons, sw);
		if (tmp != -1) {
			int tmp2 = -1;
			for (size_t j = 0; j + 2 < sizeof(kKorLConsCom); j += 3) {
				if (kKorLConsCom[j] == tmp && kKorLConsCom[j + 1] == iValue_[2]) {
					tmp2 = kKorLConsCom[j + 2];
					break;
				}
			}
			if (tmp2 != -1) {
				if (isInput) iValue_[2] = tmp2;
				string += (char16_t)(0xAC00 + iValue_[0] * 0x24C + iValue_[1] * 0x1C + tmp2 + 1);
			} else {
				startNew(last, sw);
			}
		} else {
			const int tmp3 = GetIndex(kKorVowel, sw);
			if (tmp3 == -1) {
				startNew(last, sw);
			} else if (inputChars_.size() < maxLen) {
				int tmp2 = -1;
				for (size_t j = 0; j + 2 < sizeof(kKorLConsSpr); j += 3) {
					if (kKorLConsSpr[j] == iValue_[2]) {
						tmp2 = (int)j;
						break;
					}
				}
				if (tmp2 != -1) {
					string += (char16_t)(0xAC00 + iValue_[0] * 0x24C + iValue_[1] * 0x1C + kKorLConsSpr[tmp2 + 1]);
					string += (char16_t)(0xAC00 + kKorLConsSpr[tmp2 + 2] * 0x24C + tmp3 * 0x1C);
					if (isInput) {
						iValue_[0] = kKorLConsSpr[tmp2 + 2];
						iValue_[1] = tmp3;
						iLevel_ = 2;
					}
				} else {
					const int tmp4 = GetIndex(kKorCons, kKorLCons[iValue_[2]]);
					if (tmp4 != -1) {
						string += (char16_t)(0xAC00 + iValue_[0] * 0x24C + iValue_[1] * 0x1C);
						string += (char16_t)(0xAC00 + tmp4 * 0x24C + tmp3 * 0x1C);
						if (isInput) {
							iValue_[0] = tmp4;
							iValue_[1] = tmp3;
							iLevel_ = 2;
						}
					} else {
						string += last;
						string += sw;
						if (isInput) iLevel_ = 0;
					}
				}
			} else {
				string += last;
				isCombinated_ = false;
			}
		}
	}
	return string;
}

std::u16string OskDialog::CombinationString(bool isInput) {
	std::u16string string;
	isCombinated_ = false;
	const int selectedRow = selectedChar_ / Cols();
	const int selectedCol = selectedChar_ % Cols();
	if (currentKeyboard_ == OSK_KEYBOARD_KOREAN) return CombinationKorean(isInput);
	if (isInput) iLevel_ = 0;

	const char16_t key = OskKeyAt(currentKeyboard_, selectedRow, selectedCol);
	if (key == u'゛' || key == u'゜') {
		// The mark changes the last character, when it takes one.
		const char16_t *table = kDiacritics[key == u'゛' ? 0 : 1];
		for (u32 i = 0; i < inputChars_.size(); i++) {
			if (i + 1 == inputChars_.size()) {
				for (size_t j = 0; j + 1 < Length(table); j += 2) {
					if (inputChars_[i] == table[j]) {
						string += table[j + 1];
						isCombinated_ = true;
						break;
					}
				}
				if (!isCombinated_) string += inputChars_[i];
			} else {
				string += inputChars_[i];
			}
		}
	} else {
		string = inputChars_;
		if (string.size() < FieldMaxLength()) string += key;
		isCombinated_ = true;
	}
	return string;
}

void OskDialog::RemoveKorean() {
	if (iLevel_ == 1) {
		iLevel_ = 0;
	} else if (iLevel_ == 2) {
		int tmp = -1;
		for (size_t i = 2; i < sizeof(kKorVowelCom); i += 3) {
			if (kKorVowelCom[i] == iValue_[1]) {
				tmp = kKorVowelCom[i - 1];
				break;
			}
		}
		if (tmp != -1) {
			iValue_[1] = tmp;
			inputChars_ += (char16_t)(0xAC00 + iValue_[0] * 0x24C + iValue_[1] * 0x1C);
		} else {
			iLevel_ = 1;
			inputChars_ += kKorCons[iValue_[0]];
		}
	} else if (iLevel_ == 3) {
		int tmp = -1;
		for (size_t i = 2; i < sizeof(kKorLConsCom); i += 3) {
			if (kKorLConsCom[i] == iValue_[2]) {
				tmp = kKorLConsCom[i - 1];
				break;
			}
		}
		if (tmp != -1) {
			iValue_[2] = tmp;
			inputChars_ += (char16_t)(0xAC00 + iValue_[0] * 0x24C + iValue_[1] * 0x1C + iValue_[2] + 1);
		} else {
			iLevel_ = 2;
			inputChars_ += (char16_t)(0xAC00 + iValue_[0] * 0x24C + iValue_[1] * 0x1C);
		}
	}
}

void OskDialog::RenderKeyboard() {
	const int selectedRow = selectedChar_ / Cols();
	const int selectedCol = selectedChar_ % Cols();

	constexpr u32 FIELDDRAWMAX = 16;
	const u32 limit = FieldMaxLength();
	const u32 drawLimit = std::min(FIELDDRAWMAX, limit);  // field drawn length limit

	const float keyboardLeftSide = (480.0f - (24.0f * Cols())) / 2.0f;
	const float characterWidth = 12.0f;
	const float previewLeftSide = (480.0f - (12.0f * drawLimit)) / 2.0f;
	const float title = (480.0f - (0.5f * drawLimit)) / 2.0f;

	ppge::DrawString(desc_, title, 20, FadedStyle(ppge::BOX_CENTER, 0.5f));

	const ppge::Style textStyle = FadedStyle(ppge::BOX_HCENTER, 0.5f);
	const ppge::Style keyStyle = FadedStyle(ppge::BOX_HCENTER, 0.6f);
	ppge::Style selectedKeyStyle = FadedStyle(ppge::BOX_HCENTER, 0.6f);
	selectedKeyStyle.color = CalcFadedColor(0xFF3060FF);

	const std::u16string result = CombinationString(false);
	u32 drawIndex = (u32)(result.size() > drawLimit ? result.size() - drawLimit : 0);
	if (result.size() == (size_t)limit + 1 && drawIndex > 0) drawIndex--;  // at the limit the last character doesn't blink
	for (u32 i = 0; i < drawLimit; ++i, ++drawIndex) {
		const float x = previewLeftSide + (i * characterWidth);
		if (drawIndex + 1 < result.size()) {
			ppge::DrawString(Char(result[drawIndex]), x, 40.0f, textStyle);
		} else if (drawIndex + 1 == result.size()) {
			if (isCombinated_) {
				// Fade the next character in and out: it isn't part of the text yet.
				const float animStep = (float)(display::Vcount() % 40) / 20.0f;
				const u32 alpha = (u32)((0.5f - (std::cos(animStep * 3.14159265f) / 2.0f)) * 128 + 127);
				ppge::Style animStyle = textStyle;
				animStyle.color = CalcFadedColor((alpha << 24) | 0x00FFFFFF);
				ppge::DrawString(Char(result[drawIndex]), x, 40.0f, animStyle);
				// Also the underline, for the same reason.
				ppge::DrawString("_", x, 40.0f, textStyle);
			} else {
				ppge::DrawString(Char(result[drawIndex]), x, 40.0f, textStyle);
			}
		} else {
			ppge::DrawString("_", x, 40.0f, textStyle);
		}
	}

	for (int row = 0; row < Rows(); ++row) {
		for (int col = 0; col < Cols(); ++col) {
			const std::string key = Char(OskKeyAt(currentKeyboard_, row, col));
			const float x = keyboardLeftSide + (25.0f * col) + characterWidth / 2.0f, y = 70.0f + (25.0f * row);
			if (selectedRow == row && col == selectedCol) {
				ppge::DrawString(key, x, y, selectedKeyStyle);
				ppge::DrawString("_", x, y, keyStyle);
			} else {
				ppge::DrawString(key, x, y, keyStyle);
			}
		}
	}
}

void OskDialog::UpdateKeyboard() {
	const u32 cancelButton = GetCancelButton();
	const u32 confirmButton = GetConfirmButton();
	constexpr int kHeldThreshold = 10, kHeldRepeat = 5;
	enum { H_CANCEL, H_CONFIRM, H_LEFT, H_UP, H_DOWN, H_RIGHT };
	const s32 inputType = FieldInputType();

	int selectedRow = selectedChar_ / Cols();
	int selectedExtra = selectedChar_ % Cols();

	StartDraw();
	ppge::DrawRect(0, 0, 480, 272, CalcFadedColor(0x63636363));
	RenderKeyboard();

	const ppge::Style actionStyle = FadedStyle(ppge::BOX_LEFT, 0.5f);
	const ppge::Style guideStyle = FadedStyle(ppge::BOX_LEFT, 0.6f);

	ppge::DrawIcon(ppge::Icon::Square, 365, 222, 16, 16, guideStyle);
	ppge::DrawString("Space", 390, 222, actionStyle);
	const bool circleConfirms = confirmButton == CTRL_CIRCLE;
	ppge::DrawIcon(circleConfirms ? ppge::Icon::Circle : ppge::Icon::Cross, 45, 222, 16, 16, guideStyle);
	ppge::DrawIcon(circleConfirms ? ppge::Icon::Cross : ppge::Icon::Circle, 45, 247, 16, 16, guideStyle);
	ppge::DrawString("Select", 75, 222, actionStyle);
	ppge::DrawString("Delete", 75, 247, actionStyle);
	ppge::DrawString("Start", 135, 220, guideStyle);
	ppge::DrawString("Finish", 185, 222, actionStyle);

	auto langName = [&](int direction) -> const char * {
		int lang = (OSK_LANGUAGE_COUNT + currentKeyboardLanguage_ + direction) % OSK_LANGUAGE_COUNT;
		int disp = kKeyboardCases[lang][LOWERCASE];
		FindValidKeyboard(inputType, direction, lang, disp);
		return lang == currentKeyboardLanguage_ ? nullptr : kLanguageNames[lang];
	};
	if (currentKeyboardLanguage_ != OSK_LANGUAGE_KOREAN && IsKeyboardShiftValid(inputType, currentKeyboardLanguage_, currentKeyboard_)) {
		ppge::DrawString("Select", 135, 245, guideStyle);
		ppge::DrawString("Shift", 185, 247, actionStyle);
	}
	if (const char *prev = langName(-1)) {
		ppge::DrawString("L", 235, 220, guideStyle);
		ppge::DrawString(prev, 255, 222, actionStyle);
	}
	if (const char *next = langName(1)) {
		ppge::DrawString("R", 235, 245, guideStyle);
		ppge::DrawString(next, 255, 247, actionStyle);
	}

	auto pressedOrRepeat = [&](u32 button, int &held) {
		return IsButtonPressed(button) || IsButtonHeld(button, held, kHeldThreshold, kHeldRepeat);
	};
	if (pressedOrRepeat(CTRL_UP, held_[H_UP])) {
		selectedChar_ -= Cols();
	} else if (pressedOrRepeat(CTRL_DOWN, held_[H_DOWN])) {
		selectedChar_ += Cols();
	} else if (pressedOrRepeat(CTRL_LEFT, held_[H_LEFT])) {
		selectedChar_--;
		if (((selectedChar_ + Cols()) % Cols()) == Cols() - 1) selectedChar_ += Cols();
	} else if (pressedOrRepeat(CTRL_RIGHT, held_[H_RIGHT])) {
		selectedChar_++;
		if ((selectedChar_ % Cols()) == 0) selectedChar_ -= Cols();
	}
	selectedChar_ = (selectedChar_ + (Cols() * Rows())) % (Cols() * Rows());

	auto clampSelection = [&]() {
		selectedRow = std::min(selectedRow, Rows() - 1);
		selectedExtra = std::min(selectedExtra, Cols() - 1);
		selectedChar_ = selectedRow * Cols() + selectedExtra;
	};
	if (pressedOrRepeat(confirmButton, held_[H_CONFIRM])) {
		inputChars_ = CombinationString(true);
	} else if (IsButtonPressed(CTRL_SELECT)) {
		// Select swaps case.
		if (IsKeyboardShiftValid(inputType, currentKeyboardLanguage_, currentKeyboard_)) {
			const int upper = kKeyboardCases[currentKeyboardLanguage_][UPPERCASE];
			currentKeyboard_ = currentKeyboard_ == upper ? kKeyboardCases[currentKeyboardLanguage_][LOWERCASE] : upper;
		}
		clampSelection();
	} else if (IsButtonPressed(CTRL_RTRIGGER)) {
		// R cycles languages forward.
		currentKeyboardLanguage_ = (currentKeyboardLanguage_ + 1) % OSK_LANGUAGE_COUNT;
		currentKeyboard_ = kKeyboardCases[currentKeyboardLanguage_][LOWERCASE];
		FindValidKeyboard(inputType, 1, currentKeyboardLanguage_, currentKeyboard_);
		clampSelection();
	} else if (IsButtonPressed(CTRL_LTRIGGER)) {
		// L cycles languages backward.
		currentKeyboardLanguage_ = (currentKeyboardLanguage_ + OSK_LANGUAGE_COUNT - 1) % OSK_LANGUAGE_COUNT;
		currentKeyboard_ = kKeyboardCases[currentKeyboardLanguage_][LOWERCASE];
		FindValidKeyboard(inputType, -1, currentKeyboardLanguage_, currentKeyboard_);
		clampSelection();
	} else if (pressedOrRepeat(cancelButton, held_[H_CANCEL])) {
		if (!inputChars_.empty()) {
			inputChars_.pop_back();
			if (iLevel_ != 0) RemoveKorean();
		}
	} else if (IsButtonPressed(CTRL_START)) {
		LOGI("UTIL", "OSK: returning \"%s\"", ppge::Utf16ToUtf8(inputChars_).c_str());
		StartFade(false);
	} else if (IsButtonPressed(CTRL_SQUARE) && inputChars_.size() < FieldMaxLength()) {
		// A full-width space on the Japanese and full-width keyboards.
		inputChars_ += (currentKeyboardLanguage_ != OSK_LANGUAGE_JAPANESE && currentKeyboardLanguage_ != OSK_LANGUAGE_ENGLISH_FW) ? u" " : u"　";
	}
	EndDraw();
}

int OskDialog::Update(int animSpeed) {
	if (GetStatus() != STATUS_RUNNING) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	UpdateButtons();
	UpdateCommon();
	framesRun_++;

	if (Headless()) {
		// Nobody to type: the --osk-text answer if one was given, else the initial text.
		if (framesRun_ >= 2 && !IsFadingOut() && pendingStatus_ != STATUS_FINISHED) {
			if (!g_headlessOskTexts.empty()) {
				const std::string &text = g_headlessOskTexts[std::min(g_headlessOskNext, g_headlessOskTexts.size() - 1)];
				g_headlessOskNext++;
				inputChars_ = Utf8ToUtf16(text);
				u32 maxLen = FieldMaxLength();
				if (inputChars_.size() > maxLen) inputChars_.resize(maxLen);
				LOGI("UTIL", "OSK: returning \"%s\" (headless, --osk-text)", text.c_str());
			} else {
				LOGI("UTIL", "OSK: returning the initial text (headless)");
			}
			ChangeStatus(STATUS_FINISHED, 0);
		}
	} else {
		UpdateFade(animSpeed);
		UpdateKeyboard();
	}
	WriteOutput(OSK_RESULT_CHANGED);
	return 0;
}

int OskDialog::Shutdown(bool force) {
	if (GetStatus() != STATUS_FINISHED && !force) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	Dialog::Shutdown(force);
	if (!force) ChangeStatusShutdown(OSK_SHUTDOWN_DELAY_US);
	return 0;
}

void SetHeadlessOskText(const std::string &text) {
	g_headlessOskTexts.clear();
	g_headlessOskNext = 0;
	if (text.empty()) return;
	size_t pos = 0;
	for (;;) {
		size_t bar = text.find('|', pos);
		g_headlessOskTexts.push_back(text.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos));
		if (bar == std::string::npos) break;
		pos = bar + 1;
	}
}

}  // namespace utility
