// SPDX-License-Identifier: GPL-2.0-or-later
//
// Small system libraries: sceReg (the system registry), sceOpenPSID, sceUsb / sceUsbstor /
// sceUsbstorBoot / sceUsbAcc (no cable connected), sceChkreg.
//
// The registry is in memory: the default registry of firmware 4.01 (read from its vshmain.prx)
// that guest writes (sceRegCreateKey / sceRegSetKeyValue) are layered on, with
// /CONFIG/SYSTEM/XMB language and button_assign following the system settings PSPC reports.
//
// Ported from PPSSPP (Core/HLE/sceReg.cpp, sceOpenPSID.cpp, sceUsb.cpp, sceUsbAcc.cpp,
// sceChkreg.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/misc_libs.h"
#include "runtime/hle/modules/net_internal.h"
#include "runtime/host/host.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"
#include "runtime/io/io.h"
#include "runtime/vsh.h"

namespace systemparam {
int Language();
int ButtonMode();
}  // namespace systemparam

namespace {

constexpr const char *LOG = "REG";

// ================================================================ registry contents

enum class ValueType {
	FAIL = 0,
	DIR = 1,
	INT = 2,
	STR = 3,
	BIN = 4,
};

// One entry of the static dump. STR/BIN: strValue holds the data and intValue its size (STR sizes
// count the NUL; an empty BIN literal means all zero). DIR: intValue is the entry count.
struct KeyValue {
	const char *name;
	ValueType type;
	const char *strValue;
	int intValue;
	const KeyValue *dirContents;
};

#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))

// The registry a 4.01 PSP starts with: vshmain.prx's own table of default keys (what it writes
// when it creates the registry, flash1:/registry/init.dat), with the values it picks for the
// region PSPC reports (sceChkregGetPsCode: product code 5, Europe), plus the nickname. Categories
// and keys are in the table's order; the VSH upgrades a registry whose category_version is older
// than 0x40 by adding keys, which this one never needs. Key names and values are facts read from
// the 4.01 firmware (vshmain.prx module offsets 0x36E00 = the keys, 0x3FF38 = the categories,
// 0xDE4 = the region-dependent defaults).

// /CONFIG/SYSTEM/XMB/THEME
static const KeyValue tree_CONFIG_SYSTEM_XMB_THEME[] = {
	{ "color_mode", ValueType::INT, "", (int)0x0 },
	{ "wallpaper_mode", ValueType::INT, "", (int)0x0 },
	{ "system_color", ValueType::INT, "", (int)0x0 },
	{ "custom_theme_mode", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO0
static const KeyValue tree_DATA_FONT_PROPERTY_INFO0[] = {
	{ "h_size", ValueType::INT, "", (int)0x288 },
	{ "v_size", ValueType::INT, "", (int)0x288 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x1 },
	{ "style", ValueType::INT, "", (int)0x67 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x1 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-NewRodin Pro DB", 20 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "jpn0.pgf", 9 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO1
static const KeyValue tree_DATA_FONT_PROPERTY_INFO1[] = {
	{ "h_size", ValueType::INT, "", (int)0x288 },
	{ "v_size", ValueType::INT, "", (int)0x288 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x1 },
	{ "style", ValueType::INT, "", (int)0x1 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-NewRodin Pro Latin", 23 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn0.pgf", 9 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO2
static const KeyValue tree_DATA_FONT_PROPERTY_INFO2[] = {
	{ "h_size", ValueType::INT, "", (int)0x288 },
	{ "v_size", ValueType::INT, "", (int)0x288 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x2 },
	{ "style", ValueType::INT, "", (int)0x1 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-Matisse Pro Latin", 22 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn1.pgf", 9 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO3
static const KeyValue tree_DATA_FONT_PROPERTY_INFO3[] = {
	{ "h_size", ValueType::INT, "", (int)0x288 },
	{ "v_size", ValueType::INT, "", (int)0x288 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x1 },
	{ "style", ValueType::INT, "", (int)0x2 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-NewRodin Pro Latin", 23 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn2.pgf", 9 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO4
static const KeyValue tree_DATA_FONT_PROPERTY_INFO4[] = {
	{ "h_size", ValueType::INT, "", (int)0x288 },
	{ "v_size", ValueType::INT, "", (int)0x288 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x2 },
	{ "style", ValueType::INT, "", (int)0x2 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-Matisse Pro Latin", 22 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn3.pgf", 9 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO5
static const KeyValue tree_DATA_FONT_PROPERTY_INFO5[] = {
	{ "h_size", ValueType::INT, "", (int)0x288 },
	{ "v_size", ValueType::INT, "", (int)0x288 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x1 },
	{ "style", ValueType::INT, "", (int)0x5 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-NewRodin Pro Latin", 23 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn4.pgf", 9 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO6
static const KeyValue tree_DATA_FONT_PROPERTY_INFO6[] = {
	{ "h_size", ValueType::INT, "", (int)0x288 },
	{ "v_size", ValueType::INT, "", (int)0x288 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x2 },
	{ "style", ValueType::INT, "", (int)0x5 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-Matisse Pro Latin", 22 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn5.pgf", 9 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO7
static const KeyValue tree_DATA_FONT_PROPERTY_INFO7[] = {
	{ "h_size", ValueType::INT, "", (int)0x288 },
	{ "v_size", ValueType::INT, "", (int)0x288 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x1 },
	{ "style", ValueType::INT, "", (int)0x6 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-NewRodin Pro Latin", 23 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn6.pgf", 9 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO8
static const KeyValue tree_DATA_FONT_PROPERTY_INFO8[] = {
	{ "h_size", ValueType::INT, "", (int)0x288 },
	{ "v_size", ValueType::INT, "", (int)0x288 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x2 },
	{ "style", ValueType::INT, "", (int)0x6 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-Matisse Pro Latin", 22 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn7.pgf", 9 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO9
static const KeyValue tree_DATA_FONT_PROPERTY_INFO9[] = {
	{ "h_size", ValueType::INT, "", (int)0x1c0 },
	{ "v_size", ValueType::INT, "", (int)0x1c0 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x1 },
	{ "style", ValueType::INT, "", (int)0x1 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-NewRodin Pro Latin", 23 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn8.pgf", 9 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO10
static const KeyValue tree_DATA_FONT_PROPERTY_INFO10[] = {
	{ "h_size", ValueType::INT, "", (int)0x1c0 },
	{ "v_size", ValueType::INT, "", (int)0x1c0 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x2 },
	{ "style", ValueType::INT, "", (int)0x1 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-Matisse Pro Latin", 22 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn9.pgf", 9 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO11
static const KeyValue tree_DATA_FONT_PROPERTY_INFO11[] = {
	{ "h_size", ValueType::INT, "", (int)0x1c0 },
	{ "v_size", ValueType::INT, "", (int)0x1c0 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x1 },
	{ "style", ValueType::INT, "", (int)0x2 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-NewRodin Pro Latin", 23 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn10.pgf", 10 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO12
static const KeyValue tree_DATA_FONT_PROPERTY_INFO12[] = {
	{ "h_size", ValueType::INT, "", (int)0x1c0 },
	{ "v_size", ValueType::INT, "", (int)0x1c0 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x2 },
	{ "style", ValueType::INT, "", (int)0x2 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-Matisse Pro Latin", 22 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn11.pgf", 10 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO13
static const KeyValue tree_DATA_FONT_PROPERTY_INFO13[] = {
	{ "h_size", ValueType::INT, "", (int)0x1c0 },
	{ "v_size", ValueType::INT, "", (int)0x1c0 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x1 },
	{ "style", ValueType::INT, "", (int)0x5 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-NewRodin Pro Latin", 23 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn12.pgf", 10 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO14
static const KeyValue tree_DATA_FONT_PROPERTY_INFO14[] = {
	{ "h_size", ValueType::INT, "", (int)0x1c0 },
	{ "v_size", ValueType::INT, "", (int)0x1c0 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x2 },
	{ "style", ValueType::INT, "", (int)0x5 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-Matisse Pro Latin", 22 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn13.pgf", 10 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO15
static const KeyValue tree_DATA_FONT_PROPERTY_INFO15[] = {
	{ "h_size", ValueType::INT, "", (int)0x1c0 },
	{ "v_size", ValueType::INT, "", (int)0x1c0 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x1 },
	{ "style", ValueType::INT, "", (int)0x6 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-NewRodin Pro Latin", 23 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn14.pgf", 10 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO16
static const KeyValue tree_DATA_FONT_PROPERTY_INFO16[] = {
	{ "h_size", ValueType::INT, "", (int)0x1c0 },
	{ "v_size", ValueType::INT, "", (int)0x1c0 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x2 },
	{ "style", ValueType::INT, "", (int)0x6 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x2 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x1 },
	{ "font_name", ValueType::STR, "FTT-Matisse Pro Latin", 22 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "ltn15.pgf", 10 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /DATA/FONT/PROPERTY/INFO17
static const KeyValue tree_DATA_FONT_PROPERTY_INFO17[] = {
	{ "h_size", ValueType::INT, "", (int)0x288 },
	{ "v_size", ValueType::INT, "", (int)0x288 },
	{ "h_resolution", ValueType::INT, "", (int)0x2000 },
	{ "v_resolution", ValueType::INT, "", (int)0x2000 },
	{ "extra_attributes", ValueType::INT, "", (int)0x0 },
	{ "weight", ValueType::INT, "", (int)0x0 },
	{ "family_code", ValueType::INT, "", (int)0x1 },
	{ "style", ValueType::INT, "", (int)0x1 },
	{ "sub_style", ValueType::INT, "", (int)0x0 },
	{ "language_code", ValueType::INT, "", (int)0x3 },
	{ "region_code", ValueType::INT, "", (int)0x0 },
	{ "country_code", ValueType::INT, "", (int)0x3 },
	{ "font_name", ValueType::STR, "AsiaNHH(512Johab)", 18 },  // created with room for 64 bytes
	{ "file_name", ValueType::STR, "kr0.pgf", 8 },  // created with room for 64 bytes
	{ "expire_date", ValueType::INT, "", (int)0x0 },
	{ "shadow_option", ValueType::INT, "", (int)0x0 },
};

// /CONFIG/SYSTEM/XMB
static const KeyValue tree_CONFIG_SYSTEM_XMB[] = {
	{ "theme_type", ValueType::INT, "", (int)0x0 },
	{ "language", ValueType::INT, "", (int)0x1 },
	{ "button_assign", ValueType::INT, "", (int)0x1 },
	{ "THEME", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_SYSTEM_XMB_THEME), tree_CONFIG_SYSTEM_XMB_THEME },
};

// /CONFIG/SYSTEM/SOUND
static const KeyValue tree_CONFIG_SYSTEM_SOUND[] = {
	{ "main_volume", ValueType::INT, "", (int)0x14 },
	{ "mute", ValueType::INT, "", (int)0x0 },
	{ "avls", ValueType::INT, "", (int)0x0 },
	{ "equalizer_mode", ValueType::INT, "", (int)0x0 },
	{ "operation_sound_mode", ValueType::INT, "", (int)0x1 },
	{ "dynamic_normalizer", ValueType::INT, "", (int)0x0 },
};

// /CONFIG/SYSTEM/POWER_SAVING
static const KeyValue tree_CONFIG_SYSTEM_POWER_SAVING[] = {
	{ "suspend_interval", ValueType::INT, "", (int)0x258 },
	{ "backlight_off_interval", ValueType::INT, "", (int)0x12c },
	{ "wlan_mode", ValueType::INT, "", (int)0x0 },
};

// /CONFIG/SYSTEM/LOCK
static const KeyValue tree_CONFIG_SYSTEM_LOCK[] = {
	{ "password", ValueType::BIN, "0000", 4 },
	{ "parental_level", ValueType::INT, "", (int)0x0 },
	{ "browser_start", ValueType::INT, "", (int)0x0 },
};

// /CONFIG/SYSTEM/CHARACTER_SET
static const KeyValue tree_CONFIG_SYSTEM_CHARACTER_SET[] = {
	{ "oem", ValueType::INT, "", (int)0x5 },
	{ "ansi", ValueType::INT, "", (int)0x13 },
};

// /CONFIG/NETWORK/ADHOC
static const KeyValue tree_CONFIG_NETWORK_ADHOC[] = {
	{ "channel", ValueType::INT, "", (int)0x0 },
};

// /CONFIG/NETWORK/INFRASTRUCTURE
static const KeyValue tree_CONFIG_NETWORK_INFRASTRUCTURE[] = {
	{ "latest_id", ValueType::INT, "", (int)0x0 },
	{ "eap_md5", ValueType::INT, "", (int)0x0 },
	{ "auto_setting", ValueType::INT, "", (int)0x0 },
	{ "wifisvc_setting", ValueType::INT, "", (int)0x0 },
};

// /CONFIG/NETWORK/GO_MESSENGER
static const KeyValue tree_CONFIG_NETWORK_GO_MESSENGER[] = {
	{ "auth_name", ValueType::STR, "", 1 },  // created with room for 64 bytes
	{ "auth_key", ValueType::STR, "", 1 },  // created with room for 64 bytes
};

// /DATA/FONT/PROPERTY
static const KeyValue tree_DATA_FONT_PROPERTY[] = {
	{ "INFO0", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO0), tree_DATA_FONT_PROPERTY_INFO0 },
	{ "INFO1", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO1), tree_DATA_FONT_PROPERTY_INFO1 },
	{ "INFO2", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO2), tree_DATA_FONT_PROPERTY_INFO2 },
	{ "INFO3", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO3), tree_DATA_FONT_PROPERTY_INFO3 },
	{ "INFO4", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO4), tree_DATA_FONT_PROPERTY_INFO4 },
	{ "INFO5", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO5), tree_DATA_FONT_PROPERTY_INFO5 },
	{ "INFO6", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO6), tree_DATA_FONT_PROPERTY_INFO6 },
	{ "INFO7", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO7), tree_DATA_FONT_PROPERTY_INFO7 },
	{ "INFO8", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO8), tree_DATA_FONT_PROPERTY_INFO8 },
	{ "INFO9", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO9), tree_DATA_FONT_PROPERTY_INFO9 },
	{ "INFO10", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO10), tree_DATA_FONT_PROPERTY_INFO10 },
	{ "INFO11", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO11), tree_DATA_FONT_PROPERTY_INFO11 },
	{ "INFO12", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO12), tree_DATA_FONT_PROPERTY_INFO12 },
	{ "INFO13", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO13), tree_DATA_FONT_PROPERTY_INFO13 },
	{ "INFO14", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO14), tree_DATA_FONT_PROPERTY_INFO14 },
	{ "INFO15", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO15), tree_DATA_FONT_PROPERTY_INFO15 },
	{ "INFO16", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO16), tree_DATA_FONT_PROPERTY_INFO16 },
	{ "INFO17", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY_INFO17), tree_DATA_FONT_PROPERTY_INFO17 },
};

// /CONFIG/VIDEO
static const KeyValue tree_CONFIG_VIDEO[] = {
	{ "menu_language", ValueType::BIN, "en", 2 },
	{ "sound_language", ValueType::BIN, "00", 2 },
	{ "subtitle_language", ValueType::BIN, "en", 2 },
	{ "appended_volume", ValueType::INT, "", (int)0x0 },
	{ "lr_button_enable", ValueType::INT, "", (int)0x1 },
	{ "list_play_mode", ValueType::INT, "", (int)0x1 },
};

// /CONFIG/PHOTO
static const KeyValue tree_CONFIG_PHOTO[] = {
	{ "slideshow_speed", ValueType::INT, "", (int)0x1 },
};

// /CONFIG/MUSIC
static const KeyValue tree_CONFIG_MUSIC[] = {
	{ "wma_play", ValueType::INT, "", (int)0x0 },
	{ "visualizer_mode", ValueType::INT, "", (int)0x0 },
	{ "track_info_mode", ValueType::INT, "", (int)0x1 },
};

// /CONFIG/BROWSER
static const KeyValue tree_CONFIG_BROWSER[] = {
	{ "home_uri", ValueType::STR, "", 1 },  // created with room for 1024 bytes
	{ "cookie_mode", ValueType::INT, "", (int)0x1 },
	{ "proxy_mode", ValueType::INT, "", (int)0x2 },
	{ "proxy_address", ValueType::STR, "", 1 },  // created with room for 128 bytes
	{ "proxy_port", ValueType::INT, "", (int)0x0 },
	{ "picture", ValueType::INT, "", (int)0x1 },
	{ "animation", ValueType::INT, "", (int)0x1 },
	{ "javascript", ValueType::INT, "", (int)0x1 },
	{ "cache_size", ValueType::INT, "", (int)0x200 },
	{ "char_size", ValueType::INT, "", (int)0x1 },
	{ "disp_mode", ValueType::INT, "", (int)0x0 },
	{ "connect_mode", ValueType::INT, "", (int)0x0 },
	{ "flash_activated", ValueType::INT, "", (int)0x0 },
	{ "flash_play", ValueType::INT, "", (int)0x0 },
	{ "proxy_protect", ValueType::INT, "", (int)0x0 },
	{ "proxy_autoauth", ValueType::INT, "", (int)0x0 },
	{ "proxy_user", ValueType::STR, "", 1 },  // created with room for 128 bytes
	{ "proxy_password", ValueType::STR, "", 1 },  // created with room for 128 bytes
	{ "webpage_quality", ValueType::INT, "", (int)0x0 },
};

// /CONFIG/LFTV
static const KeyValue tree_CONFIG_LFTV[] = {
	{ "easy_reg_done", ValueType::INT, "", (int)0x0 },
	{ "netav_domain_name", ValueType::STR, "", 1 },  // created with room for 256 bytes
	{ "netav_ip_address", ValueType::STR, "172.29.71.1", 12 },  // created with room for 16 bytes
	{ "netav_port_no_home", ValueType::INT, "", (int)0x139d },
	{ "netav_port_no_away", ValueType::INT, "", (int)0x139d },
	{ "netav_nonce", ValueType::STR, "", 1 },  // created with room for 33 bytes
	{ "base_station_version", ValueType::STR, "0.000", 6 },  // created with room for 7 bytes
	{ "base_station_region", ValueType::INT, "", (int)0x0 },
	{ "tuner_type", ValueType::INT, "", (int)0x0 },
	{ "input_line", ValueType::INT, "", (int)0x0 },
	{ "tv_channel", ValueType::INT, "", (int)0x0 },
	{ "bitrate_home", ValueType::INT, "", (int)0x0 },
	{ "bitrate_away", ValueType::INT, "", (int)0x0 },
	{ "channel_setting_jp", ValueType::BIN, "", 24 },  // all zero
	{ "channel_setting_us", ValueType::BIN, "", 68 },  // all zero
	{ "channel_setting_us_catv", ValueType::BIN, "", 125 },  // all zero
	{ "overwrite_netav_setting", ValueType::INT, "", (int)0x1 },
	{ "screen_mode", ValueType::INT, "", (int)0x0 },
	{ "remocon_setting_region", ValueType::INT, "", (int)0x0 },
	{ "remocon_setting", ValueType::BIN, "", 96 },  // all zero
	{ "remocon_setting_revision", ValueType::STR, "0000.00", 8 },  // created with room for 8 bytes
	{ "external_tuner_channel", ValueType::INT, "", (int)0x0 },
	{ "ssid", ValueType::STR, "", 1 },  // created with room for 33 bytes
	{ "audio_gain", ValueType::INT, "", (int)0x0 },
	{ "broadcast_standard_video1", ValueType::INT, "", (int)0x0 },
	{ "broadcast_standard_video2", ValueType::INT, "", (int)0x0 },
	{ "version", ValueType::INT, "", (int)0x0 },
	{ "tv_channel_range", ValueType::INT, "", (int)0x0 },
	{ "tuner_type_no", ValueType::INT, "", (int)0x0 },
	{ "input_line_no", ValueType::INT, "", (int)0x0 },
	{ "audio_channel", ValueType::INT, "", (int)0x0 },
	{ "shared_remocon_setting", ValueType::BIN, "", 96 },  // all zero
};

// /CONFIG/RSS
static const KeyValue tree_CONFIG_RSS[] = {
	{ "download_items", ValueType::INT, "", (int)0x5 },
};

// /CONFIG/ALARM
static const KeyValue tree_CONFIG_ALARM[] = {
	{ "alarm_0_time", ValueType::INT, "", (int)0xffffffff },
	{ "alarm_1_time", ValueType::INT, "", (int)0xffffffff },
};

// /CONFIG/PREMO
static const KeyValue tree_CONFIG_PREMO[] = {
	{ "guide_page", ValueType::INT, "", (int)0x1 },
	{ "response", ValueType::INT, "", (int)0x0 },
	{ "ps3_name", ValueType::STR, "", 1 },  // created with room for 128 bytes
	{ "ps3_mac", ValueType::BIN, "", 6 },  // all zero
	{ "ps3_keytype", ValueType::INT, "", (int)0x0 },
	{ "ps3_key", ValueType::BIN, "", 16 },  // all zero
	{ "custom_video_bitrate1", ValueType::INT, "", (int)0x0 },
	{ "custom_video_bitrate2", ValueType::INT, "", (int)0x0 },
	{ "custom_video_buffer1", ValueType::INT, "", (int)0x0 },
	{ "custom_video_buffer2", ValueType::INT, "", (int)0x0 },
	{ "setting_internet", ValueType::INT, "", (int)0x0 },
	{ "button_assign", ValueType::INT, "", (int)0x0 },
	{ "flags", ValueType::INT, "", (int)0x0 },
};

// /CONFIG/CAMERA
static const KeyValue tree_CONFIG_CAMERA[] = {
	{ "still_size", ValueType::INT, "", (int)0x1 },
	{ "movie_size", ValueType::INT, "", (int)0x1 },
	{ "still_quality", ValueType::INT, "", (int)0x0 },
	{ "movie_quality", ValueType::INT, "", (int)0x0 },
	{ "movie_fps", ValueType::INT, "", (int)0x1 },
	{ "white_balance", ValueType::INT, "", (int)0x0 },
	{ "exposure_bias", ValueType::INT, "", (int)0x0 },
	{ "shutter_sound_mode", ValueType::INT, "", (int)0x0 },
	{ "file_folder", ValueType::INT, "", (int)0x65 },
	{ "file_number", ValueType::INT, "", (int)0x1 },
	{ "msid", ValueType::BIN, "", 16 },  // all zero
	{ "still_effect", ValueType::INT, "", (int)0x0 },
};

// /CONFIG/DISPLAY
static const KeyValue tree_CONFIG_DISPLAY[] = {
	{ "aspect_ratio", ValueType::INT, "", (int)0x0 },
	{ "scan_mode", ValueType::INT, "", (int)0x0 },
	{ "screensaver_start_time", ValueType::INT, "", (int)0x258 },
};

// /CONFIG/NP
static const KeyValue tree_CONFIG_NP[] = {
	{ "env", ValueType::STR, "np", 3 },  // created with room for 9 bytes
	{ "account_id", ValueType::BIN, "", 16 },  // all zero
	{ "login_id", ValueType::STR, "", 1 },  // created with room for 65 bytes
	{ "password", ValueType::STR, "", 1 },  // created with room for 31 bytes
	{ "auto_sign_in_enable", ValueType::INT, "", (int)0x0 },
};

// /CONFIG/SYSTEM
static const KeyValue tree_CONFIG_SYSTEM[] = {
	{ "owner_name", ValueType::STR, "PSPC", 5 },  // nickname (4.01 leaves it empty for the first-boot setup); created with room for 128 bytes
	{ "backlight_brightness", ValueType::INT, "", (int)0x2 },
	{ "umd_autoboot", ValueType::INT, "", (int)0x1 },
	{ "usb_charge", ValueType::INT, "", (int)0x1 },
	{ "umd_cache", ValueType::INT, "", (int)0x1 },
	{ "XMB", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_SYSTEM_XMB), tree_CONFIG_SYSTEM_XMB },
	{ "SOUND", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_SYSTEM_SOUND), tree_CONFIG_SYSTEM_SOUND },
	{ "POWER_SAVING", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_SYSTEM_POWER_SAVING), tree_CONFIG_SYSTEM_POWER_SAVING },
	{ "LOCK", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_SYSTEM_LOCK), tree_CONFIG_SYSTEM_LOCK },
	{ "CHARACTER_SET", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_SYSTEM_CHARACTER_SET), tree_CONFIG_SYSTEM_CHARACTER_SET },
};

// /CONFIG/DATE
static const KeyValue tree_CONFIG_DATE[] = {
	{ "time_format", ValueType::INT, "", (int)0x0 },
	{ "date_format", ValueType::INT, "", (int)0x2 },
	{ "summer_time", ValueType::INT, "", (int)0x0 },
	{ "time_zone_offset", ValueType::INT, "", (int)0x3c },
	{ "time_zone_area", ValueType::STR, "belgium", 8 },  // created with room for 48 bytes
};

// /CONFIG/NETWORK
static const KeyValue tree_CONFIG_NETWORK[] = {
	{ "ADHOC", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_NETWORK_ADHOC), tree_CONFIG_NETWORK_ADHOC },
	{ "INFRASTRUCTURE", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_NETWORK_INFRASTRUCTURE), tree_CONFIG_NETWORK_INFRASTRUCTURE },
	{ "GO_MESSENGER", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_NETWORK_GO_MESSENGER), tree_CONFIG_NETWORK_GO_MESSENGER },
};

// /DATA/FONT
static const KeyValue tree_DATA_FONT[] = {
	{ "path_name", ValueType::STR, "flash0:/font", 13 },  // created with room for 64 bytes
	{ "num_fonts", ValueType::INT, "", (int)0x12 },
	{ "PROPERTY", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT_PROPERTY), tree_DATA_FONT_PROPERTY },
};

// /SYSPROFILE/RESOLUTION
static const KeyValue tree_SYSPROFILE_RESOLUTION[] = {
	{ "horizontal", ValueType::INT, "", (int)0x2012 },
	{ "vertical", ValueType::INT, "", (int)0x2012 },
};

// /REGISTRY
static const KeyValue tree_REGISTRY[] = {
	{ "category_version", ValueType::INT, "", (int)0x40 },  // 4.01's schema: the VSH upgrades older registries to it
};

// /CONFIG
static const KeyValue tree_CONFIG[] = {
	{ "VIDEO", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_VIDEO), tree_CONFIG_VIDEO },
	{ "PHOTO", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_PHOTO), tree_CONFIG_PHOTO },
	{ "MUSIC", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_MUSIC), tree_CONFIG_MUSIC },
	{ "BROWSER", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_BROWSER), tree_CONFIG_BROWSER },
	{ "LFTV", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_LFTV), tree_CONFIG_LFTV },
	{ "RSS", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_RSS), tree_CONFIG_RSS },
	{ "ALARM", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_ALARM), tree_CONFIG_ALARM },
	{ "PREMO", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_PREMO), tree_CONFIG_PREMO },
	{ "CAMERA", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_CAMERA), tree_CONFIG_CAMERA },
	{ "DISPLAY", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_DISPLAY), tree_CONFIG_DISPLAY },
	{ "NP", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_NP), tree_CONFIG_NP },
	{ "SYSTEM", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_SYSTEM), tree_CONFIG_SYSTEM },
	{ "DATE", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_DATE), tree_CONFIG_DATE },
	{ "NETWORK", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG_NETWORK), tree_CONFIG_NETWORK },
};

// /DATA
static const KeyValue tree_DATA[] = {
	{ "FONT", ValueType::DIR, "", ARRAY_SIZE(tree_DATA_FONT), tree_DATA_FONT },
};

// /SYSPROFILE
static const KeyValue tree_SYSPROFILE[] = {
	{ "sound_reduction", ValueType::INT, "", (int)0x0 },
	{ "RESOLUTION", ValueType::DIR, "", ARRAY_SIZE(tree_SYSPROFILE_RESOLUTION), tree_SYSPROFILE_RESOLUTION },
};

static const KeyValue ROOT[] = {
	{ "REGISTRY", ValueType::DIR, "", ARRAY_SIZE(tree_REGISTRY), tree_REGISTRY },
	{ "CONFIG", ValueType::DIR, "", ARRAY_SIZE(tree_CONFIG), tree_CONFIG },
	{ "DATA", ValueType::DIR, "", ARRAY_SIZE(tree_DATA), tree_DATA },
	{ "SYSPROFILE", ValueType::DIR, "", ARRAY_SIZE(tree_SYSPROFILE), tree_SYSPROFILE },
};

#undef ARRAY_SIZE

// A key as the guest sees it: the dump, live system settings and guest writes merged.
struct RegKey {
	std::string name;
	ValueType type = ValueType::INT;
	std::string data;  // STR (including its NUL) / BIN payload
	s32 intValue = 0;  // INT value

	u32 Size() const {
		switch (type) {
		case ValueType::INT: return 4;
		case ValueType::STR:
		case ValueType::BIN: return (u32)data.size();
		default: return 0;
		}
	}
};

RegKey FromStatic(const KeyValue &kv) {
	RegKey k;
	k.name = kv.name;
	k.type = kv.type;
	switch (kv.type) {
	case ValueType::INT: k.intValue = kv.intValue; break;
	case ValueType::STR:
		k.data.assign((size_t)kv.intValue, '\0');
		memcpy(&k.data[0], kv.strValue, std::min(strlen(kv.strValue), (size_t)kv.intValue));
		break;
	case ValueType::BIN:
		// An empty literal stands for all-zero data; otherwise the literal holds every byte.
		k.data.assign((size_t)kv.intValue, '\0');
		if (kv.strValue[0]) memcpy(&k.data[0], kv.strValue, (size_t)kv.intValue);
		break;
	default: break;
	}
	return k;
}

// Guest-written keys, by category path (no leading or trailing '/', e.g. "DATA/FONT"), in creation
// order. Font plugins (CLANNAD's chfont.prx) register their fonts this way at boot.
std::map<std::string, std::vector<RegKey>> g_overlay;

std::string NormalizePath(const std::string &path) {
	std::string p = path;
	while (!p.empty() && p.front() == '/') p.erase(p.begin());
	while (!p.empty() && p.back() == '/') p.pop_back();
	return p;
}

std::vector<std::string> SplitPath(const std::string &path) {
	std::vector<std::string> parts;
	size_t start = 0;
	while (start <= path.size()) {
		size_t slash = path.find('/', start);
		if (slash == std::string::npos) slash = path.size();
		if (slash > start) parts.push_back(path.substr(start, slash - start));
		start = slash + 1;
	}
	return parts;
}

const KeyValue *LookupStatic(const std::string &path, int *count) {
	const KeyValue *dir = ROOT;
	int n = (int)(sizeof(ROOT) / sizeof(ROOT[0]));
	for (const std::string &part : SplitPath(path)) {
		const KeyValue *next = nullptr;
		for (int i = 0; i < n; i++) {
			if (part == dir[i].name) {
				if (dir[i].type != ValueType::DIR) return nullptr;
				next = &dir[i];
				break;
			}
		}
		if (!next) return nullptr;
		n = next->intValue;
		dir = next->dirContents;
	}
	*count = n;
	return dir;
}

// One of the XMB's time zone areas for each offset from UTC (sysconf_plugin.prx 4.01 lists 109
// areas with their offsets in minutes), to pick the area matching the host's zone.
const char *TimeZoneAreaFor(s32 minutes) {
	static const struct {
		s32 minutes;
		const char *area;
	} kAreas[] = {
		{-660, "samoa"}, {-600, "united_states_hawaii"}, {-540, "united_states_alaska"},
		{-480, "pacific_us_canada"}, {-420, "mountain_us_canada"}, {-360, "central_us_canada"},
		{-300, "eastern_us_canada"}, {-240, "venezuela"}, {-210, "canada_newfoundland"},
		{-180, "sao_paulo_brazil"}, {-60, "portugal_azores"}, {0, "united_kingdom"}, {60, "belgium"},
		{120, "greece"}, {180, "kuwait"}, {210, "iran"}, {240, "united_arab_emirates"},
		{270, "afghanistan"}, {300, "pakistan"}, {330, "india"}, {345, "nepal"},
		{360, "astana_kazakstan"}, {420, "thailand"}, {480, "malaysia"}, {540, "japan"},
		{570, "australia_adelaide"}, {600, "australia_sydney"}, {720, "new_zealand"},
	};
	for (const auto &a : kAreas)
		if (a.minutes == minutes) return a.area;
	return nullptr;
}

// The merged contents of one category (path normalized). Doesn't check that the path exists.
std::vector<RegKey> MergedCategory(const std::string &path) {
	std::vector<RegKey> keys;
	int count = 0;
	if (const KeyValue *kv = LookupStatic(path, &count)) {
		for (int i = 0; i < count; i++) keys.push_back(FromStatic(kv[i]));
	}
	if (path == "CONFIG/SYSTEM/XMB") {
		for (RegKey &k : keys) {
			if (k.name == "language") k.intValue = systemparam::Language();
			if (k.name == "button_assign") k.intValue = systemparam::ButtonMode();
		}
	}
	// The host's time zone, so the XMB's Date & Time settings (which convert with the zone of
	// time_zone_area) agree with the clock PSPC shows; with a window also the default formats
	// (headless runs keep the region's, like sceUtilityGetSystemParamInt).
	if (path == "CONFIG/DATE") {
		TIME_ZONE_INFORMATION tzi{};
		DWORD r = GetTimeZoneInformation(&tzi);
		const s32 standard = -(s32)tzi.Bias;
		const char *area = TimeZoneAreaFor(standard);
		for (RegKey &k : keys) {
			if (k.name == "time_zone_offset") k.intValue = standard;
			if (k.name == "summer_time") k.intValue = r == TIME_ZONE_ID_DAYLIGHT ? 1 : 0;
			if (k.name == "time_zone_area" && area) k.data = std::string(area) + '\0';
			if ((k.name == "date_format" || k.name == "time_format") && !host::IsHeadless()) k.intValue = 0;
		}
	}
	auto it = g_overlay.find(path);
	if (it != g_overlay.end()) {
		for (const RegKey &o : it->second) {
			auto existing = std::find_if(keys.begin(), keys.end(), [&](const RegKey &k) { return k.name == o.name; });
			if (existing != keys.end()) *existing = o;
			else keys.push_back(o);
		}
	}
	return keys;
}

// Looks up a category: every component must be a directory of its parent.
bool LookupCategory(const std::string &rawPath, std::vector<RegKey> *keys) {
	std::string path = NormalizePath(rawPath);
	std::string cur;
	for (const std::string &part : SplitPath(path)) {
		std::vector<RegKey> parent = MergedCategory(cur);
		auto it = std::find_if(parent.begin(), parent.end(), [&](const RegKey &k) { return k.name == part; });
		if (it == parent.end() || it->type != ValueType::DIR) return false;
		cur = cur.empty() ? part : cur + "/" + part;
	}
	*keys = MergedCategory(cur);
	return true;
}

bool g_overlayDirty = false;

void SetOverlay(const std::string &path, const RegKey &key) {
	auto &list = g_overlay[path];
	auto it = std::find_if(list.begin(), list.end(), [&](const RegKey &k) { return k.name == key.name; });
	if (it != list.end()) *it = key;
	else list.push_back(key);
	g_overlayDirty = true;
}

// ---- persistence (the XMB's settings) ----
//
// The XMB's writes (System Settings: nickname, language, theme...) survive in
// flash1:/registry/pspc.reg, one key per line: category <TAB> name <TAB> type <TAB> value (INT in
// decimal, STR/BIN in hex). Only the XMB saves; the XMB and the games it launches load it (a game
// started on its own sees the built-in registry, so test results don't depend on it).
constexpr const char *kRegFile = "flash1:/registry/pspc.reg";
bool g_persistLoaded = false;

bool PersistLoads() { return vsh::Active() || vsh::ReturnToXmb(); }

void LoadPersisted() {
	if (g_persistLoaded) return;
	g_persistLoaded = true;
	if (!PersistLoads()) return;
	std::vector<u8> data;
	if (!io::ReadFile(kRegFile, data)) return;
	std::string text(data.begin(), data.end());
	size_t pos = 0, n = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(pos, end - pos);
		pos = end + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		std::vector<std::string> f;
		size_t s = 0;
		for (int i = 0; i < 3; i++) {
			size_t t = line.find('\t', s);
			if (t == std::string::npos) break;
			f.push_back(line.substr(s, t - s));
			s = t + 1;
		}
		if (f.size() != 3) continue;
		f.push_back(line.substr(s));
		RegKey k;
		k.name = f[1];
		k.type = (ValueType)atoi(f[2].c_str());
		if (k.type == ValueType::INT) {
			k.intValue = (s32)strtol(f[3].c_str(), nullptr, 10);
		} else if (k.type == ValueType::STR || k.type == ValueType::BIN) {
			for (size_t i = 0; i + 1 < f[3].size(); i += 2) k.data.push_back((char)strtoul(f[3].substr(i, 2).c_str(), nullptr, 16));
		} else if (k.type != ValueType::DIR) {
			continue;
		}
		auto &list = g_overlay[f[0]];
		auto it = std::find_if(list.begin(), list.end(), [&](const RegKey &o) { return o.name == k.name; });
		if (it != list.end()) *it = k;
		else list.push_back(k);
		n++;
	}
	LOGI(LOG, "%zu registry keys from %s", n, kRegFile);
}

void SavePersisted() {
	if (!g_overlayDirty || !vsh::Active()) return;
	g_overlayDirty = false;
	std::string out = "# PSPC registry: keys the XMB set (category, name, type, value)\n";
	char hex[3];
	for (const auto &cat : g_overlay) {
		for (const RegKey &k : cat.second) {
			out += cat.first + "\t" + k.name + "\t" + std::to_string((int)k.type) + "\t";
			if (k.type == ValueType::INT) out += std::to_string(k.intValue);
			for (char ch : k.data) {
				if (k.type != ValueType::STR && k.type != ValueType::BIN) break;
				snprintf(hex, sizeof(hex), "%02x", (u8)ch);
				out += hex;
			}
			out += "\n";
		}
	}
	std::shared_ptr<io::Device> dev = io::MountedDevice("flash1");
	if (!dev) return;
	dev->Mkdir("/registry", 0777);
	int h = dev->Open("/registry/pspc.reg", io::PSP_O_WRONLY | io::PSP_O_CREAT | io::PSP_O_TRUNC, 0777);
	if (h < 0) {
		LOGW(LOG, "Cannot write %s", kRegFile);
		return;
	}
	dev->Write(h, out.data(), (s64)out.size());
	dev->Close(h);
}

// ================================================================ sceReg

constexpr int REG_KEYNAME_SIZE = 27;  // bytes per name in sceRegGetKeys / GetCategoryListAtRoot

struct OpenCategory {
	std::string path;  // normalized
	int mode;
};

int g_openRegistryCount = 0;
int g_handleGen = 1337;
std::map<int, OpenCategory> g_openCategories;

// The registry (only /system exists) always has handle 0.
int sceRegOpenRegistry(u32 regParamAddr, int mode, u32 regHandleAddr) {
	LoadPersisted();
	if (mem::IsValidRange(regHandleAddr, 4)) GWrite32(regHandleAddr, 0);
	g_openRegistryCount++;
	return 0;
}

int sceRegCloseRegistry(int regHandle) {
	if (regHandle != 0) return (int)SCE_REG_ERROR_REGISTRY_NOT_FOUND;
	if (g_openRegistryCount > 0) g_openRegistryCount--;
	if (g_openRegistryCount == 0) g_openCategories.clear();
	return 0;
}

int sceRegFlushRegistry(int regHandle) {
	if (regHandle != 0) return (int)SCE_REG_ERROR_REGISTRY_NOT_FOUND;
	SavePersisted();
	return 0;
}

// Deleting the system registry: refused.
int sceRegRemoveRegistry(u32 regParamAddr) { return (int)SCE_REG_ERROR_PERMISSION_FAILURE; }

int sceRegOpenCategory(int regHandle, const char *name, int mode, u32 catHandleAddr) {
	const bool haveOut = mem::IsValidRange(catHandleAddr, 4);
	if (!haveOut) return -1;
	if (!name) return (int)SCE_REG_ERROR_INVALID_NAME;
	auto fail = [&](u32 err) {
		GWrite32(catHandleAddr, 0xFFFFFFFF);
		return (int)err;
	};
	if (regHandle != 0) return fail(SCE_REG_ERROR_REGISTRY_NOT_FOUND);
	if (NormalizePath(name).empty()) return fail(SCE_REG_ERROR_INVALID_PATH);
	std::vector<RegKey> keys;
	if (!LookupCategory(name, &keys)) return fail(SCE_REG_ERROR_CATEGORY_NOT_FOUND);
	// Categories the dump marks as inaccessible from user mode.
	if (keys.size() == 1 && keys[0].type == ValueType::FAIL) return fail((u32)keys[0].intValue);
	int handle = g_handleGen++;
	g_openCategories[handle] = OpenCategory{NormalizePath(name), mode};
	GWrite32(catHandleAddr, (u32)handle);
	LOGD(LOG, "sceRegOpenCategory(%s) -> %d", name, handle);
	return 0;
}

int sceRegCloseCategory(int catHandle) {
	if (!g_openCategories.erase(catHandle)) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	SavePersisted();
	return 0;
}

int sceRegFlushCategory(int catHandle) {
	if (!g_openCategories.count(catHandle)) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	SavePersisted();
	return 0;
}

int sceRegRemoveCategory(int regHandle, const char *name) {
	if (regHandle != 0) return (int)SCE_REG_ERROR_REGISTRY_NOT_FOUND;
	return (int)SCE_REG_ERROR_PERMISSION_FAILURE;
}

// The open category's keys, or nullptr.
bool CategoryKeys(int catHandle, std::vector<RegKey> *keys, std::string *path = nullptr) {
	auto it = g_openCategories.find(catHandle);
	if (it == g_openCategories.end()) return false;
	if (path) *path = it->second.path;
	return LookupCategory(it->second.path, keys);
}

int sceRegGetKeysNum(int catHandle, u32 numAddr) {
	if (!mem::IsValidRange(numAddr, 4)) return -1;
	std::vector<RegKey> keys;
	if (!CategoryKeys(catHandle, &keys)) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	GWrite32(numAddr, (u32)keys.size());
	return 0;
}

int sceRegGetKeys(int catHandle, u32 bufAddr, int num) {
	if (num < 0 || !mem::IsValidRange(bufAddr, (u32)num * REG_KEYNAME_SIZE)) return -1;
	std::vector<RegKey> keys;
	if (!CategoryKeys(catHandle, &keys)) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	int n = std::min(num, (int)keys.size());
	for (int i = 0; i < n; i++) {
		char name[REG_KEYNAME_SIZE] = {};
		strncpy(name, keys[i].name.c_str(), REG_KEYNAME_SIZE);
		mem::WriteBytes(bufAddr + (u32)i * REG_KEYNAME_SIZE, name, REG_KEYNAME_SIZE);
	}
	return 0;
}

int FindKey(const std::vector<RegKey> &keys, const char *name) {
	for (size_t i = 0; i < keys.size(); i++) {
		if (keys[i].name == name) return (int)i;
	}
	return -1;
}

void WriteKeyInfo(const RegKey &k, u32 typeAddr, u32 sizeAddr) {
	if (mem::IsValidRange(typeAddr, 4)) GWrite32(typeAddr, (u32)k.type);
	if (mem::IsValidRange(sizeAddr, 4)) GWrite32(sizeAddr, k.Size());
}

// The key handle is the key's index in its category.
int sceRegGetKeyInfo(int catHandle, const char *name, u32 keyHandleAddr, u32 typeAddr, u32 sizeAddr) {
	if (!name) return -1;
	std::vector<RegKey> keys;
	if (!CategoryKeys(catHandle, &keys)) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	int i = FindKey(keys, name);
	if (i < 0) return -1;
	if (mem::IsValidRange(keyHandleAddr, 4)) GWrite32(keyHandleAddr, (u32)i);
	WriteKeyInfo(keys[i], typeAddr, sizeAddr);
	return 0;
}

int sceRegGetKeyInfoByName(int catHandle, const char *name, u32 typeAddr, u32 sizeAddr) {
	if (!name) return -1;
	std::vector<RegKey> keys;
	if (!CategoryKeys(catHandle, &keys)) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	int i = FindKey(keys, name);
	if (i < 0) return -1;
	WriteKeyInfo(keys[i], typeAddr, sizeAddr);
	return 0;
}

// Copies a value out: an INT needs room for all four bytes; STR / BIN copy what fits.
int CopyValue(const RegKey &k, u32 bufAddr, u32 size) {
	switch (k.type) {
	case ValueType::INT:
		if (size >= 4) GWrite32(bufAddr, (u32)k.intValue);
		return 0;
	case ValueType::STR:
	case ValueType::BIN: {
		u32 n = std::min(size, (u32)k.data.size());
		if (n) mem::WriteBytes(bufAddr, k.data.data(), n);
		return 0;
	}
	default: return 0;
	}
}

int sceRegGetKeyValue(int catHandle, int keyHandle, u32 bufAddr, u32 size) {
	if (!mem::IsValidRange(bufAddr, size)) return -1;
	std::vector<RegKey> keys;
	if (!CategoryKeys(catHandle, &keys)) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	if (keyHandle < 0 || keyHandle >= (int)keys.size()) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	return CopyValue(keys[keyHandle], bufAddr, size);
}

int sceRegGetKeyValueByName(int catHandle, const char *name, u32 bufAddr, u32 size) {
	if (!name) return -1;
	if (!mem::IsValidRange(bufAddr, size)) return -1;
	std::vector<RegKey> keys;
	if (!CategoryKeys(catHandle, &keys)) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	int i = FindKey(keys, name);
	if (i < 0) return -1;
	return CopyValue(keys[i], bufAddr, size);
}

int sceRegSetKeyValue(int catHandle, const char *name, u32 bufAddr, u32 size) {
	if (!name) return -1;
	std::vector<RegKey> keys;
	std::string path;
	if (!CategoryKeys(catHandle, &keys, &path)) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	int i = FindKey(keys, name);
	RegKey k;
	if (i >= 0) {
		k = keys[i];
	} else {
		// A key that doesn't exist yet: a 4-byte value is an INT, anything else a string.
		k.name = name;
		k.type = size == 4 ? ValueType::INT : ValueType::STR;
	}
	switch (k.type) {
	case ValueType::INT:
		// Callers like chfont.prx pass their whole staging buffer's size for INT keys.
		if (size < 4 || !mem::IsValidRange(bufAddr, 4)) return -1;
		k.intValue = (s32)GRead32(bufAddr);
		break;
	case ValueType::STR:
		if (!mem::IsValidRange(bufAddr, size)) return -1;
		k.data.assign((const char *)GPtr(bufAddr), size);
		if (k.data.empty() || k.data.back() != '\0') k.data.push_back('\0');
		break;
	case ValueType::BIN:
		if (!mem::IsValidRange(bufAddr, size)) return -1;
		k.data.assign((const char *)GPtr(bufAddr), size);
		break;
	default: return -1;
	}
	SetOverlay(path, k);
	LOGD(LOG, "sceRegSetKeyValue(%s/%s)", path.c_str(), name);
	return 0;
}

int sceRegCreateKey(int catHandle, const char *name, int type, u32 size) {
	if (!name) return -1;
	std::vector<RegKey> keys;
	std::string path;
	if (!CategoryKeys(catHandle, &keys, &path)) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	if (type < (int)ValueType::DIR || type > (int)ValueType::BIN) return -1;
	RegKey k;
	k.name = name;
	k.type = (ValueType)type;
	if (k.type == ValueType::STR) k.data.assign(std::max<u32>(size, 1), '\0');
	if (k.type == ValueType::BIN) k.data.assign(size, '\0');
	SetOverlay(path, k);
	LOGD(LOG, "sceRegCreateKey(%s/%s, type %d)", path.c_str(), name, type);
	return 0;
}

int sceRegRemoveKey(int catHandle, int key) {
	if (!g_openCategories.count(catHandle)) return (int)SCE_REG_ERROR_CATEGORY_NOT_FOUND;
	return (int)SCE_REG_ERROR_PERMISSION_FAILURE;
}

int sceRegGetCategoryNumAtRoot(int regHandle, u32 numPtr) {
	if (regHandle != 0) return (int)SCE_REG_ERROR_REGISTRY_NOT_FOUND;
	if (!mem::IsValidRange(numPtr, 4)) return -1;
	GWrite32(numPtr, (u32)(sizeof(ROOT) / sizeof(ROOT[0])));
	return 0;
}

int sceRegGetCategoryListAtRoot(int regHandle, u32 bufPtr, int num) {
	if (regHandle != 0) return (int)SCE_REG_ERROR_REGISTRY_NOT_FOUND;
	num = std::min(num, (int)(sizeof(ROOT) / sizeof(ROOT[0])));
	if (num < 0 || !mem::IsValidRange(bufPtr, (u32)num * REG_KEYNAME_SIZE)) return -1;
	for (int i = 0; i < num; i++) {
		char name[REG_KEYNAME_SIZE] = {};
		strncpy(name, ROOT[i].name, REG_KEYNAME_SIZE);
		mem::WriteBytes(bufPtr + (u32)i * REG_KEYNAME_SIZE, name, REG_KEYNAME_SIZE);
	}
	return 0;
}

int sceRegExit() { return 0; }
int sceRegInit() { return 0; }

// ================================================================ sceOpenPSID
// A fixed console ID: the MAC address followed by fixed bytes.

void FillOpenPSID(u32 addr) {
	u8 id[16] = {0x10, 0x02, 0xA3, 0x44, 0x13, 0xF5, 0x93, 0xB0, 0xCC, 0x6E, 0xD1, 0x32, 0x27, 0x85, 0x0F, 0x9D};
	memcpy(id, net::kLocalMac, 6);
	mem::WriteBytes(addr, id, sizeof(id));
}

int sceOpenPSIDGetOpenPSID(u32 idAddr) {
	if (!mem::IsValidRange(idAddr, 16)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	FillOpenPSID(idAddr);
	return 0;
}
int sceOpenPSIDGetPSID(u32 idAddr, u32) { return sceOpenPSIDGetOpenPSID(idAddr); }
int sceOpenPSIDGetProductCode(u32) { return 0; }

// ================================================================ sceUsb
// No cable: the USB drivers start and activate, but the bus never reports a connection.

enum UsbState : u32 {
	USB_STATUS_STOPPED = 0x001,
	USB_STATUS_STARTED = 0x002,
	USB_STATUS_DISCONNECTED = 0x010,
	USB_STATUS_CONNECTED = 0x020,
	USB_STATUS_DEACTIVATED = 0x100,
	USB_STATUS_ACTIVATED = 0x200,
	USB_CONNECTION_NOT_ESTABLISHED = 0x001,  // sceUsbGetState (sceUsbGetDrvState uses 1 and 2 for stopped/started)
};
constexpr u32 SCE_USB_ERROR_DRIVER_NOT_STARTED = 0x80243007;
constexpr SceUID USB_WAIT_ID = 0x55534200;  // wait id for WAITTYPE_HLE ("USB")

int g_usbStartedDrivers = 0;
bool g_usbActivated = false;

// sceUsbGetState's bits: 0x001 connection not established / 0x002 established, 0x010 cable
// disconnected / 0x020 connected, 0x100 deactivated / 0x200 activated. With no cable (and no
// accessory such as the camera, which the XMB's camera plugin waits for as an established
// connection) the connection is never established.
u32 UsbCurrentState() {
	if (g_usbStartedDrivers == 0) return 0;
	return USB_CONNECTION_NOT_ESTABLISHED | USB_STATUS_DISCONNECTED | (g_usbActivated ? USB_STATUS_ACTIVATED : USB_STATUS_DEACTIVATED);
}

bool UsbMatches(u32 state, u32 mode) {
	u32 match = state & UsbCurrentState();
	return mode == 0 ? match == state : match != 0;
}

// Wakes the waits that the new state satisfies.
void UsbStateChanged() {
	std::vector<kernel::PspThread *> wake;
	kernel::ForEach<kernel::PspThread>([&](kernel::PspThread *t) {
		if (t->IsWaitingFor(kernel::WAITTYPE_HLE, USB_WAIT_ID) && UsbMatches(t->waitValue[0], t->waitValue[1])) wake.push_back(t);
	});
	for (auto *t : wake) sched::Wake(t, (s32)UsbCurrentState());
}

int sceUsbStart(const char *driverName, u32 argsSize, u32 argsPtr) {
	g_usbStartedDrivers++;
	UsbStateChanged();
	return 0;
}
int sceUsbStop(const char *driverName, u32 argsSize, u32 argsPtr) {
	if (g_usbStartedDrivers > 0) g_usbStartedDrivers--;
	if (g_usbStartedDrivers == 0) g_usbActivated = false;
	UsbStateChanged();
	return 0;
}
int sceUsbGetState() {
	if (g_usbStartedDrivers == 0) return (int)SCE_USB_ERROR_DRIVER_NOT_STARTED;
	return (int)UsbCurrentState();
}
int sceUsbActivate(u32 pid) {
	g_usbActivated = true;
	UsbStateChanged();
	return 0;
}
int sceUsbDeactivate(u32 pid) {
	g_usbActivated = false;
	UsbStateChanged();
	return 0;
}
int sceUsbGetDrvState(const char *driverName) { return g_usbStartedDrivers ? (int)USB_STATUS_STARTED : (int)USB_STATUS_STOPPED; }
int sceUsbGetDrvList(u32, u32, u32) { return 0; }

int UsbWaitState(u32 state, u32 mode, u32 timeoutPtr, bool cb) {
	if (mode >= 2) return (int)SCE_KERNEL_ERROR_ILLEGAL_MODE;
	if (state == 0) return (int)SCE_KERNEL_ERROR_EVF_ILPAT;
	if (UsbMatches(state, mode)) return (int)UsbCurrentState();
	kernel::PspThread *t = sched::Current();
	if (!t) return (int)SCE_KERNEL_ERROR_ILLEGAL_MODE;
	t->waitValue[0] = state;
	t->waitValue[1] = mode;
	s32 r = sched::Wait(kernel::WAITTYPE_HLE, USB_WAIT_ID, timeoutPtr, cb);
	if (r == (s32)SCE_KERNEL_ERROR_WAIT_TIMEOUT) return (int)SCE_ERROR_USB_WAIT_TIMEOUT;
	return r;
}
int sceUsbWaitState(u32 state, u32 mode, u32 timeoutPtr) { return UsbWaitState(state, mode, timeoutPtr, false); }
int sceUsbWaitStateCB(u32 state, u32 mode, u32 timeoutPtr) { return UsbWaitState(state, mode, timeoutPtr, true); }
int sceUsbWaitCancel() {
	std::vector<kernel::PspThread *> wake;
	kernel::ForEach<kernel::PspThread>([&](kernel::PspThread *t) {
		if (t->IsWaitingFor(kernel::WAITTYPE_HLE, USB_WAIT_ID)) wake.push_back(t);
	});
	for (auto *t : wake) sched::Wake(t, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL);
	return 0;
}
int sceUsb_8BFC3DE8(u32) { return 0; }

// USB mass storage: nothing is mounted by a host.
int sceUsbstorGetStatus() { return 0; }
int sceUsbstorBootOk() { return 0; }
int sceUsbstorBootGetDataSize() { return 0; }

// Accessories (no authentication needed with nothing plugged in, as PPSSPP).
int sceUsbAccGetAuthStat() { return 0; }
int sceUsbAccGetInfo(u32) { return 0; }

// ================================================================ sceChkreg_driver

int sceChkregCheckRegion(int, int) { return 1; }  // region matches
int sceChkregGetPsCode(u32 ptr) {
	static const u8 values[8] = {1, 0, 5, 0, 1, 0, 1, 0};
	if (mem::IsValidRange(ptr, 8)) mem::WriteBytes(ptr, values, 8);
	return 0;
}
int sceChkregGetPsFlags(u32 flagsPtr, int index) {
	if (mem::IsValidRange(flagsPtr, 4) && !(flagsPtr & 3)) GWrite32(flagsPtr, 1);
	return 0;
}
int sceChkregGetPspModel() { return 1; }

}  // namespace

void Register_RegAndSystemLibraries() {
	static const hle::HleFunction sceReg[] = {
		HLEFN(0x92E41280, sceRegOpenRegistry),
		HLEFN(0xFA8A5739, sceRegCloseRegistry),
		HLEFN(0xDEDA92BF, sceRegRemoveRegistry),
		HLEFN(0x1D8A762E, sceRegOpenCategory),
		HLEFN(0x0CAE832B, sceRegCloseCategory),
		HLEFN(0x39461B4D, sceRegFlushRegistry),
		HLEFN(0x0D69BF40, sceRegFlushCategory),
		HLEFN(0x57641A81, sceRegCreateKey),
		HLEFN(0x17768E14, sceRegSetKeyValue),
		HLEFN(0xD4475AA8, sceRegGetKeyInfo),
		HLEFN(0x28A8E98A, sceRegGetKeyValue),
		HLEFN(0x2C0DB9DD, sceRegGetKeysNum),
		HLEFN(0x2D211135, sceRegGetKeys),
		HLEFN(0xC5768D02, sceRegGetKeyInfoByName),
		HLEFN(0x30BE0259, sceRegGetKeyValueByName),
		HLEFN(0x4CA16893, sceRegRemoveCategory),
		HLEFN(0x3615BC87, sceRegRemoveKey),
		HLEFN(0x9B25EDF1, sceRegExit),
		HLEFN(0xBE8C1263, sceRegGetCategoryNumAtRoot),
		HLEFN(0x835ECE6F, sceRegGetCategoryListAtRoot),
	};
	hle::RegisterModule("sceReg", sceReg);

	// The kernel library: the user NIDs, plus the 6.xx kernel NIDs.
	static const hle::HleFunction sceRegDriver[] = {
		HLEFN(0x92E41280, sceRegOpenRegistry),
		HLEFN(0xDBA46704, sceRegOpenRegistry),
		HLEFN(0xFA8A5739, sceRegCloseRegistry),
		HLEFN(0x49D77D65, sceRegCloseRegistry),
		HLEFN(0xDEDA92BF, sceRegRemoveRegistry),
		HLEFN(0x1D8A762E, sceRegOpenCategory),
		HLEFN(0x4F471457, sceRegOpenCategory),
		HLEFN(0x0CAE832B, sceRegCloseCategory),
		HLEFN(0xFC742751, sceRegCloseCategory),
		HLEFN(0x39461B4D, sceRegFlushRegistry),
		HLEFN(0x5FD4764A, sceRegFlushRegistry),
		HLEFN(0x0D69BF40, sceRegFlushCategory),
		HLEFN(0xD743A608, sceRegFlushCategory),
		HLEFN(0x57641A81, sceRegCreateKey),
		HLEFN(0x3B6CA1E6, sceRegCreateKey),
		HLEFN(0x17768E14, sceRegSetKeyValue),
		HLEFN(0x49C70163, sceRegSetKeyValue),
		HLEFN(0xD4475AA8, sceRegGetKeyInfo),
		HLEFN(0x9980519F, sceRegGetKeyInfo),
		HLEFN(0x28A8E98A, sceRegGetKeyValue),
		HLEFN(0xF4A3E396, sceRegGetKeyValue),
		HLEFN(0x2C0DB9DD, sceRegGetKeysNum),
		HLEFN(0x2D211135, sceRegGetKeys),
		HLEFN(0xC5768D02, sceRegGetKeyInfoByName),
		HLEFN(0xF2619407, sceRegGetKeyInfoByName),
		HLEFN(0x30BE0259, sceRegGetKeyValueByName),
		HLEFN(0x38415B9F, sceRegGetKeyValueByName),
		HLEFN(0x4CA16893, sceRegRemoveCategory),
		HLEFN(0x61DB9D06, sceRegRemoveCategory),
		HLEFN(0x3615BC87, sceRegRemoveKey),
		HLEFN(0x9B25EDF1, sceRegExit),
		HLEFN(0x98279CF1, sceRegInit),
	};
	hle::RegisterModule("sceReg_driver", sceRegDriver);

	static const hle::HleFunction sceOpenPSID[] = {
		HLEFN(0xC69BEBCE, sceOpenPSIDGetOpenPSID),
		HLEFN(0xB29330DE, sceOpenPSIDGetProductCode),
	};
	hle::RegisterModule("sceOpenPSID", sceOpenPSID);
	static const hle::HleFunction sceOpenPSIDDriver[] = {
		HLEFN(0x19D579F0, sceOpenPSIDGetPSID),
		HLEFN(0xC69BEBCE, sceOpenPSIDGetOpenPSID),
	};
	hle::RegisterModule("sceOpenPSID_driver", sceOpenPSIDDriver);

	static const hle::HleFunction sceUsb[] = {
		HLEFN(0x8BFC3DE8, sceUsb_8BFC3DE8),
		HLEFN(0xAE5DE6AF, sceUsbStart),
		HLEFN(0xC2464FA0, sceUsbStop),
		HLEFN(0xC21645A4, sceUsbGetState),
		HLEFN(0x4E537366, sceUsbGetDrvList),
		HLEFN(0x112CC951, sceUsbGetDrvState),
		HLEFN(0x586DB82C, sceUsbActivate),
		HLEFN(0xC572A9C8, sceUsbDeactivate),
		HLEFN(0x5BE0E002, sceUsbWaitState),
		HLEFN(0x616F2B61, sceUsbWaitStateCB),
		HLEFN(0x1C360735, sceUsbWaitCancel),
	};
	hle::RegisterModule("sceUsb", sceUsb);
	static const hle::HleFunction sceUsbDriver[] = {
		HLEFN(0xAE5DE6AF, sceUsbStart),
		HLEFN(0xC2464FA0, sceUsbStop),
		HLEFN(0xC21645A4, sceUsbGetState),
		HLEFN(0x4E537366, sceUsbGetDrvList),
		HLEFN(0x112CC951, sceUsbGetDrvState),
		HLEFN(0x586DB82C, sceUsbActivate),
		HLEFN(0xE20B23A6, sceUsbActivate),  // sceUsbActivateWithCharging
		HLEFN(0xC572A9C8, sceUsbDeactivate),
		HLEFN(0x5BE0E002, sceUsbWaitState),
		HLEFN(0x1C360735, sceUsbWaitCancel),
	};
	hle::RegisterModule("sceUsb_driver", sceUsbDriver);

	static const hle::HleFunction sceUsbstor[] = {
		HLEFN(0x60066CFE, sceUsbstorGetStatus),
	};
	hle::RegisterModule("sceUsbstor", sceUsbstor);
	static const hle::HleFunction sceUsbstorBoot[] = {
		HLEFN(0xE58818A8, sceUsbstorBootOk),  // sceUsbstorBootSetCapacity
		HLEFN(0x594BBF95, sceUsbstorBootOk),  // sceUsbstorBootSetLoadAddr
		HLEFN(0x6D865ECD, sceUsbstorBootGetDataSize),
		HLEFN(0xA1119F0D, sceUsbstorBootOk),  // sceUsbstorBootSetStatus
		HLEFN(0x1F080078, sceUsbstorBootOk),  // sceUsbstorBootRegisterNotify
		HLEFN(0xA55C9E16, sceUsbstorBootOk),  // sceUsbstorBootUnregisterNotify
	};
	hle::RegisterModule("sceUsbstorBoot", sceUsbstorBoot);
	static const hle::HleFunction sceUsbAcc[] = {
		HLEFN(0x79A1C743, sceUsbAccGetAuthStat),
		HLEFN(0x0CD7D4AA, sceUsbAccGetInfo),
	};
	hle::RegisterModule("sceUsbAcc", sceUsbAcc);

	static const hle::HleFunction sceChkreg[] = {
		HLEFN(0x54495B19, sceChkregCheckRegion),
		HLEFN(0x59F8491D, sceChkregGetPsCode),
		HLEFN(0x6894A027, sceChkregGetPsFlags),
		HLEFN(0x7939C851, sceChkregGetPspModel),
	};
	hle::RegisterModule("sceChkreg_driver", sceChkreg);
}
