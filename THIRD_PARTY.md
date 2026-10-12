# Third-party code and references

| Component | Where | License | Use |
|---|---|---|---|
| PPSSPP (https://github.com/hrydgard/ppsspp) | ported into `src/` where marked "Portions derived from PPSSPP" | GPL-2.0-or-later | HLE behaviour, relocation handling, VFPU semantics and bit-exact math, error codes, GE semantics |
| PPSSPP `ext/at3_standalone` (FFmpeg ATRAC3/3+ decoders) | `src/audio/third_party/at3_standalone` | LGPL-2.1-or-later | ATRAC decoding |
| FFmpeg `libavcodec/aactab.c` (the AAC Huffman codebooks and scalefactor band tables only) | `src/audio/third_party/aac_tables` | LGPL-2.1-or-later | measuring AAC raw access units (`src/audio/aac_frame.cpp`) |
| libkirk (draan) | `tools/pspc-decrypt/libkirk` | GPL-3.0-or-later | KIRK crypto in the separate `pspc-decrypt` tool only (installed as its own program) |
| pspdecrypt (John-K) | reference for `tools/pspc-decrypt` | GPL-3.0 | PSAR/IPL handling in `pspc-decrypt` only |
| SDL2 | `third_party/SDL2-*` (downloaded) | zlib | window, input, audio |
| Python (CPython 3.12 runtime and standard library, with the OpenSSL, SQLite and libffi DLLs and the Visual C++ runtime DLLs it ships with) | `python\` in the installer, built by `tools/make_installer.py` from a local Python install (not in the repository) | PSF License (`python\LICENSE.txt` covers the bundled libraries); the VC++ runtime under Microsoft's redistribution terms | runs the servers, firmware installer and AOT builds |
| [LLVM/MinGW 20260616 (LLVM 22.1.8)](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260616), including Clang, LLD, compiler-rt, libc++, libunwind and mingw-w64 | `aot/toolchain/` in the installer, fetched and hash-verified by `tools/fetch_aot.py` | Apache-2.0 with LLVM exceptions and the mingw-w64 component licenses; bundled in `aot/toolchain/LICENSE.TXT` and `aot/toolchain/x86_64-w64-mingw32/share/mingw32/` | self-contained x64 AOT compiler and libraries; uses Windows 10/11's UCRT, with no Visual Studio or Windows SDK dependency |
| Windows Media Foundation, WIC | part of Windows | OS components | MP3/AAC/H.264 decoding, JPEG |
| PPSSPP replacement PGF fonts | `reference/ppsspp/assets/flash0/font`, copied only into local build output | see PPSSPP | stand-in system fonts until the user supplies firmware (not redistributed) |
| pspautotests (https://github.com/hrydgard/pspautotests) | `reference/` (not redistributed) | see repository | test corpus and hardware-recorded expectations |
| pspsdk, uOFW | `reference/` (not redistributed) | BSD / MIT | NIDs, headers, firmware documentation |
| AnyPS5 | `reference/AnyPS5` (not redistributed) | GPL-2.0-only | architectural reference |
| pspdecompiler | `reference/pspdecompiler`, built out-of-tree by `tools/build_pspdecompiler.cmd` | AFL-2.0 | analysis tool only (not linked) |
| Ghidra + ghidra-allegrex | installed separately | Apache-2.0 | analysis tool only |

Sony firmware, decrypted modules, Ghidra databases of Sony code and game files are never part of
the repository (`private/`, `Games/` and `reference/` are git-ignored).
