// SPDX-License-Identifier: GPL-2.0-or-later
//
// M33 vshctrl's ISO browsing, reproduced natively (docs/m33-behaviors.md section 3): while the
// XMB runs, every UMD image in ms0:/ISO appears in the Game menu as a memory stick game. The
// memory stick's file system gets an overlay: listing ms0:/PSP/GAME adds one folder per image,
// "MMMMMISO<n>" (vshctrl's names), holding a virtual EBOOT.PBP made of the image's PARAM.SFO
// (as a memory stick game), ICON0/ICON1/PIC0/PIC1/SND0. Launching that EBOOT.PBP boots the image.
// The listing also has vshctrl's GAME150 / GAME4XX entries, and pspc's Games folder: its UMD
// images and its EBOOT.PBP game folders (read-only, launched from where they are).
#pragma once

#include <string>
#include <vector>

namespace m33 {

// Installs the overlay on the mounted ms0: (call after io::Init and the mounts).
// extraDirs: host folders whose UMD images are listed too (pspc's Games folder).
void InstallIsoBrowser(const std::vector<std::string> &extraDirs = {});
// The host path of the UMD image a virtual "ms0:/PSP/GAME/MMMMMISO<n>/..." path stands for.
bool IsoForVirtualGamePath(const std::string &pspPath, std::string *hostPath);
// The host file behind a game path the overlay adds: "ms0:/PSP/GAME/<name>__150/..." and
// "<name>__4XX/..." (vshctrl's GAME150 / GAME4XX folders), and the EBOOT.PBP game folders of
// pspc's Games folder, which appear in ms0:/PSP/GAME under their own names.
bool HostPathForVirtualGame(const std::string &pspPath, std::string *hostPath);

}  // namespace m33
