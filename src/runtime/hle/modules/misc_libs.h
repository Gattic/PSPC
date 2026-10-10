// SPDX-License-Identifier: GPL-2.0-or-later
// Registration entry points of the system libraries grouped under Register_Misc() (misc.cpp).
#pragma once

// net.cpp: sceNet, sceNetInet, sceNetApctl, sceNetResolver, sceNetAdhoc*, sceWlan, sceHttp, sceSsl,
// sceParseUri, sceParseHttp (offline semantics).
void Register_NetLibraries();
// ccc.cpp: sceCcc character set conversion.
void Register_CccLibrary();
// reg.cpp: sceReg, sceOpenPSID, sceUsb, sceUsbstor and other small system libraries.
void Register_RegAndSystemLibraries();
// font.cpp: sceLibFont (PGF fonts).
void Register_FontLibraries();
// media_*.cpp: sceMpeg, sceMpegbase, scePsmf, scePsmfPlayer, sceVideocodec, sceJpeg, sceMp4.
void Register_MediaLibraries();
// chnnlsv.cpp: sceChnnlsv (savedata MAC/cipher) and the kernel "semaphore" library (KIRK), through
// the pspc-decrypt KIRK service.
void Register_ChnnlsvLibraries();
