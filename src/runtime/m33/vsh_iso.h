// SPDX-License-Identifier: GPL-2.0-or-later
//
// M33 vshctrl's ISO browsing, reproduced natively (docs/m33-behaviors.md section 3): while the
// XMB runs, every UMD image in ms0:/ISO appears in the Game menu as a memory stick game. The
// memory stick's file system gets an overlay: listing ms0:/PSP/GAME adds one folder per image,
// "MMMMMISO<n>" (vshctrl's names), holding a virtual EBOOT.PBP made of the image's PARAM.SFO
// (as a memory stick game), ICON0/ICON1/PIC0/PIC1/SND0. Launching that EBOOT.PBP boots the image.
#pragma once

#include <string>

namespace m33 {

// Installs the overlay on the mounted ms0: (call after io::Init and the mounts).
void InstallIsoBrowser();
// The host path of the UMD image a virtual "ms0:/PSP/GAME/MMMMMISO<n>/..." path stands for.
bool IsoForVirtualGamePath(const std::string &pspPath, std::string *hostPath);

}  // namespace m33
