This is the atrac3/atrac3+ decoders from ffmpeg, extracted to be standalone from ffmpeg.

Copied into PSPC from PPSSPP's ext/at3_standalone (https://github.com/hrydgard/ppsspp).
License: LGPL-2.1-or-later (see LICENSE and the header of each file).
PSPC changes: compat.cpp logs through PSPC's logger; mem.cpp uses _aligned_malloc;
ppsspp_config.h is a small shim providing the PPSSPP_ARCH() macro atrac3plusdsp.cpp uses.
