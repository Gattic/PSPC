# Third-party code and references

| Component | Where | License | Use |
|---|---|---|---|
| PPSSPP (https://github.com/hrydgard/ppsspp) | ported into `src/` where marked "Portions derived from PPSSPP" | GPL-2.0-or-later | HLE behaviour, relocation handling, VFPU semantics and bit-exact math, error codes, GE semantics |
| PPSSPP `ext/at3_standalone` (FFmpeg ATRAC3/3+ decoders) | `src/audio/third_party/at3_standalone` | LGPL-2.1-or-later | ATRAC decoding |
| libkirk (draan) | `tools/pspc-decrypt/libkirk` | GPL-3.0-or-later | KIRK crypto in the separate `pspc-decrypt` tool only |
| pspdecrypt (John-K) | reference for `tools/pspc-decrypt` | GPL-3.0 | PSAR/IPL handling in `pspc-decrypt` only |
| SDL2 | `third_party/SDL2-*` (downloaded) | zlib | window, input, audio |
| Windows Media Foundation, WIC | part of Windows | OS components | MP3/AAC/H.264 decoding, JPEG |
| PPSSPP replacement PGF fonts | `reference/ppsspp/assets/flash0/font`, copied only into local build output | see PPSSPP | stand-in system fonts until the user supplies firmware (not redistributed) |
| pspautotests (https://github.com/hrydgard/pspautotests) | `reference/` (not redistributed) | see repository | test corpus and hardware-recorded expectations |
| pspsdk, uOFW | `reference/` (not redistributed) | BSD / MIT | NIDs, headers, firmware documentation |
| AnyPS5 | `reference/AnyPS5` (not redistributed) | GPL-2.0-only | architectural reference |
| pspdecompiler | `reference/pspdecompiler`, built out-of-tree by `tools/build_pspdecompiler.cmd` | AFL-2.0 | analysis tool only (not linked) |
| Ghidra + ghidra-allegrex | installed separately | Apache-2.0 | analysis tool only |

Sony firmware, decrypted modules, Ghidra databases of Sony code and game files are never part of
the repository (`private/`, `Games/` and `reference/` are git-ignored).
