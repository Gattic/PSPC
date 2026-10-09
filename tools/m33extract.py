#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 PSPC contributors
"""Extract the modules embedded in the 4.01 M33 installer (UPDATE/EBOOT.PBP).

The installer's DATA.PSP is a plain PRX that carries, inside its .data:

* ``~PSP`` containers (0x150-byte header + gzip stream) holding the M33
  modules that end up in flash0 (systemctrl, vshctrl, satelite, ...).
* Plain (unpacked) ELF/PRX helper modules used by the installer itself
  (u235.prx, the second-stage updater, its kernel drivers, the downloader).
* Boot configuration files (``pspbt?nf*.bin``, magic 0x0F803001) and a
  Spanish recovery translation.
* Nested PBP files (PUPD.PBP, DL.PBP) that the installer writes to the
  Memory Stick.

Destination file names are not guessed: the installer's ``uranium235``
module contains two tables of ``{const char *path, void *data, u32 size}``
(one per PSP model) that it walks when writing flash0.  We locate such tables
generically by scanning every embedded ELF's loadable segment for triples
whose first word points at a ``flash0:/``/``flash1:/``/``ms0:/`` string, and
we additionally recognise direct ``write(path, buf, size)`` call sites by a
small constant tracker over ``lui``/``addiu``/``ori`` sequences.

Usage:
    python -I tools/m33extract.py reference/401M33/UPDATE/EBOOT.PBP [-o private/fw/401/m33]

Outputs:
    <out>/<flash0 basename>           plain PRX / btcnf / txt files
    <out>/installer/<name>            installer helper ELFs, PBPs, btcnf
    <out>/manifest.json               description of every extracted file
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

PSP_MAGIC = b"~PSP"
ELF_MAGIC = b"\x7fELF"
BTCNF_MAGIC = 0x0F803001
PBP_MAGIC = b"\x00PBP"
PSP_HEADER_SIZE = 0x150

# Fallback names for ~PSP modules that the installer does not write to flash0
# through one of its path tables (keyed by the module name in the header).
FALLBACK_NAMES = {
    "pspIplUpdate": "ipl_update.prx",
}

# Names for the installer's own helper ELFs, keyed by (module name, container).
# The container is the PBP the ELF is the DATA.PSP of, or the module whose
# .data it lives in.
INSTALLER_NAMES = {
    "uranium235_module": "u235.prx",              # written as ms0:/PSP/GAME/UPDATE/u235.prx
    "Plutonium_Driver": "plutonium_driver.prx",
    "pspDegeneration_Driver": "degeneration_driver.prx",
    "TheDownloader": "dl_data.psp.prx",           # DATA.PSP of DL.PBP
}


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def cstr(b, o, maxlen=256):
    end = b.find(b"\0", o, o + maxlen)
    if end < 0:
        end = min(len(b), o + maxlen)
    return b[o:end].decode("latin-1")


def sha1(data):
    return hashlib.sha1(data).hexdigest()


# --------------------------------------------------------------------------
# ELF helpers (just enough to size and map an embedded ELF)
# --------------------------------------------------------------------------

class EmbeddedElf:
    def __init__(self, blob, off):
        self.off = off
        hdr = struct.unpack_from("<4sBBBB8xHHIIIIIHHHHHH", blob, off)
        (_, self.ei_class, self.ei_data, _, _, self.e_type, self.e_machine, _,
         self.e_entry, self.e_phoff, self.e_shoff, self.e_flags, _, self.e_phentsize,
         self.e_phnum, self.e_shentsize, self.e_shnum, self.e_shstrndx) = hdr
        self.phdrs = []
        for i in range(self.e_phnum):
            self.phdrs.append(struct.unpack_from("<8I", blob, off + self.e_phoff + i * 32))
        self.shdrs = []
        if self.e_shoff and self.e_shnum:
            for i in range(self.e_shnum):
                self.shdrs.append(struct.unpack_from("<10I", blob, off + self.e_shoff + i * 40))
        end = max(self.e_phoff + 32 * self.e_phnum, 0x34)
        for p in self.phdrs:
            end = max(end, p[1] + p[4])
        for s in self.shdrs:
            if s[1] != 8:  # SHT_NOBITS has no file data
                end = max(end, s[4] + s[5])
        if self.shdrs:
            end = max(end, self.e_shoff + self.e_shnum * 40)
        self.size = end
        self.blob = blob
        # module info: for PRX the first program header's p_paddr is the file
        # offset of SceModuleInfo (bit 31 set for kernel modules).
        self.modname = ""
        self.attr = 0
        if self.phdrs:
            mi = off + (self.phdrs[0][3] & 0x7FFFFFFF)
            if mi + 32 <= len(blob):
                self.attr = u16(blob, mi)
                self.modname = cstr(blob, mi + 4, 28)

    @staticmethod
    def looks_valid(blob, off):
        if blob[off:off + 4] != ELF_MAGIC or off + 0x34 > len(blob):
            return False
        cls, data = blob[off + 4], blob[off + 5]
        machine = u16(blob, off + 18)
        phoff = u32(blob, off + 0x1C)
        phnum = u16(blob, off + 0x2C)
        return cls == 1 and data == 1 and machine == 8 and 0 < phnum < 16 and phoff < 0x1000

    def load_segments(self):
        """Yield (vaddr, file_offset_abs, filesz) for PT_LOAD segments."""
        for p in self.phdrs:
            if p[0] == 1 and p[4]:
                yield p[2], self.off + p[1], p[4]

    def vaddr_to_abs(self, va):
        for vaddr, foff, fsz in self.load_segments():
            if vaddr <= va < vaddr + fsz:
                return foff + (va - vaddr)
        return None

    def in_text(self, abs_off):
        return any(s <= abs_off < s + n for s, n, _ in self.text_ranges())

    def text_ranges(self):
        """Executable sections (abs offset, size, vaddr)."""
        out = []
        for s in self.shdrs:
            if s[1] == 1 and (s[2] & 4) and s[5]:
                out.append((self.off + s[4], s[5], s[3]))
        if not out:
            for p in self.phdrs:
                if p[0] == 1 and (p[6] & 1):
                    out.append((self.off + p[1], p[4], p[2]))
        return out


# --------------------------------------------------------------------------
# Scanners
# --------------------------------------------------------------------------

def find_all(blob, pat):
    i = blob.find(pat)
    while i >= 0:
        yield i
        i = blob.find(pat, i + 1)


def scan_psp_containers(blob):
    found = []
    for off in find_all(blob, PSP_MAGIC):
        if off + PSP_HEADER_SIZE + 2 > len(blob):
            continue
        if blob[off + PSP_HEADER_SIZE:off + PSP_HEADER_SIZE + 2] != b"\x1f\x8b":
            continue
        hdr = {
            "attribute": u16(blob, off + 4),
            "comp_attribute": u16(blob, off + 6),
            "module_version": "%d.%d" % (blob[off + 9], blob[off + 8]),
            "module_name": cstr(blob, off + 0xA, 28),
            "elf_size": u32(blob, off + 0x28),
            "psp_size": u32(blob, off + 0x2C),
            "entry": u32(blob, off + 0x30),
            "modinfo_offset": u32(blob, off + 0x34),
            "comp_size": u32(blob, off + 0xB0),
            "decrypt_mode": u32(blob, off + 0x7C),
            "devkit_version": u32(blob, off + 0x78),
            "tag": u32(blob, off + 0xD0),
            "oe_tag": u32(blob, off + 0x130),
        }
        start = off + PSP_HEADER_SIZE
        end = off + hdr["psp_size"] if hdr["psp_size"] > PSP_HEADER_SIZE else len(blob)
        d = zlib.decompressobj(16 + zlib.MAX_WBITS)
        try:
            elf = d.decompress(blob[start:end]) + d.flush()
        except zlib.error as e:
            print("warning: ~PSP at 0x%x: gzip error %s" % (off, e), file=sys.stderr)
            continue
        if not elf.startswith(ELF_MAGIC):
            print("warning: ~PSP at 0x%x does not contain an ELF" % off, file=sys.stderr)
            continue
        if hdr["elf_size"] and len(elf) != hdr["elf_size"]:
            print("warning: ~PSP at 0x%x: elf_size %d but got %d bytes"
                  % (off, hdr["elf_size"], len(elf)), file=sys.stderr)
        found.append((off, hdr, elf))
    return found


def scan_elves(blob):
    out = []
    for off in find_all(blob, ELF_MAGIC):
        if EmbeddedElf.looks_valid(blob, off):
            try:
                out.append(EmbeddedElf(blob, off))
            except struct.error:
                pass
    return out


def scan_btcnf(blob):
    out = []
    magic = struct.pack("<I", BTCNF_MAGIC)
    for off in find_all(blob, magic):
        if off + 0x40 > len(blob):
            continue
        nmodes, nmods = u32(blob, off + 0x14), u32(blob, off + 0x24)
        modestart, modstart = u32(blob, off + 0x10), u32(blob, off + 0x20)
        namestart, nameend = u32(blob, off + 0x30), u32(blob, off + 0x34)
        if not (0 < nmodes < 64 and 0 < nmods < 512 and modestart == 0x40
                and namestart < nameend < 0x10000 and off + nameend <= len(blob)):
            continue
        out.append((off, nameend))
    return out


def scan_pbps(blob):
    out = []
    for off in find_all(blob, PBP_MAGIC):
        if off + 0x28 > len(blob):
            continue
        offs = struct.unpack_from("<8I", blob, off + 8)
        if offs[0] != 0x28 or list(offs) != sorted(offs):
            continue
        out.append((off, offs))
    return out


PATH_PREFIXES = (b"flash0:/", b"flash1:/", b"ms0:/")


def scan_path_tables(blob, elf):
    """Find {path*, data*, size} triples inside an ELF's loadable segments."""
    hits = []
    for vaddr, foff, fsz in elf.load_segments():
        for pos in range(foff, foff + fsz - 12 + 1, 4):
            p, d, n = struct.unpack_from("<3I", blob, pos)
            if not (0 < n < 0x1000000):
                continue
            pa = elf.vaddr_to_abs(p)
            da = elf.vaddr_to_abs(d)
            if pa is None or da is None:
                continue
            if not blob.startswith(PATH_PREFIXES, pa):
                continue
            hits.append({"path": cstr(blob, pa), "data_abs": da, "size": n,
                         "table_entry_vaddr": vaddr + (pos - foff)})
    return hits


GPR_A0, GPR_A1, GPR_A2 = 4, 5, 6


def scan_call_sites(blob, elf):
    """Recognise ``f(path_const, data_const, size_const)`` call sites.

    Tracks constants loaded by lui/addiu/ori into a0..a2 over the instructions
    preceding each jal (and its delay slot).  The window is reset at
    unconditional jumps and returns.
    """
    hits = []
    for tstart, tsize, tva in elf.text_ranges():
        regs = {}
        for pos in range(tstart, tstart + tsize - 7, 4):
            w = u32(blob, pos)
            op = w >> 26
            rs, rt, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 0x03:  # jal: evaluate including the delay slot
                ds = u32(blob, pos + 4)
                tmp = dict(regs)
                _track(tmp, ds)
                if all(r in tmp for r in (GPR_A0, GPR_A1, GPR_A2)):
                    pa = elf.vaddr_to_abs(tmp[GPR_A0])
                    da = elf.vaddr_to_abs(tmp[GPR_A1])
                    n = tmp[GPR_A2]
                    if (pa is not None and da is not None and 0 < n < 0x1000000
                            and blob.startswith(PATH_PREFIXES, pa)
                            and not elf.in_text(da)):
                        hits.append({"path": cstr(blob, pa), "data_abs": da, "size": n,
                                     "call_site_vaddr": tva + (pos - tstart)})
                regs = {}
                continue
            if op == 0x02 or w == 0x03E00008:  # j / jr ra
                regs = {}
                continue
            _track(regs, w)
    return hits


def _track(regs, w):
    op = w >> 26
    rs, rt, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if op == 0x0F:                       # lui
        regs[rt] = (imm << 16) & 0xFFFFFFFF
    elif op == 0x09:                     # addiu
        if rs == 0:
            regs[rt] = simm & 0xFFFFFFFF
        elif rs in regs:
            regs[rt] = (regs[rs] + simm) & 0xFFFFFFFF
        else:
            regs.pop(rt, None)
    elif op == 0x0D:                     # ori
        if rs == 0:
            regs[rt] = imm
        elif rs in regs:
            regs[rt] = regs[rs] | imm
        else:
            regs.pop(rt, None)
    elif op == 0 and (w & 0x3F) in (0x21, 0x25) and ((w >> 16) & 31) == 0:  # move rd, rs
        rd = (w >> 11) & 31
        if rs in regs:
            regs[rd] = regs[rs]
        else:
            regs.pop(rd, None)
    else:
        # Conservatively forget the destination of anything else.
        if op == 0:
            regs.pop((w >> 11) & 31, None)
        elif op not in (0x2B, 0x29, 0x28, 0x04, 0x05, 0x01, 0x06, 0x07):  # stores/branches
            regs.pop(rt, None)


# --------------------------------------------------------------------------
# Main extraction
# --------------------------------------------------------------------------

def parse_btcnf(data):
    """Return a compact description of a pspbtcnf file (modes and modules)."""
    nmodes, nmods = u32(data, 0x14), u32(data, 0x24)
    modestart, modstart, namestart = u32(data, 0x10), u32(data, 0x20), u32(data, 0x30)
    mods = []
    for i in range(nmods):
        e = modstart + i * 0x20
        mods.append({"path": cstr(data, namestart + u32(data, e)), "flags": u32(data, e + 8)})
    modes = []
    for i in range(nmodes):
        e = modestart + i * 0x20
        count, first = u16(data, e), u16(data, e + 2)
        mask, key = u32(data, e + 4), u32(data, e + 8)
        modes.append({"key": key, "mask": mask, "first": first, "count": count})
    return {"devkit": "0x%08X" % u32(data, 4), "modes": modes, "modules": mods}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pbp", help="path to the M33 installer UPDATE/EBOOT.PBP")
    ap.add_argument("-o", "--out", default=os.path.join("private", "fw", "401", "m33"),
                    help="output directory (default: private/fw/401/m33)")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    blob = open(args.pbp, "rb").read()
    if not blob.startswith(PBP_MAGIC):
        sys.exit("%s: not a PBP file" % args.pbp)
    pbp_offs = struct.unpack_from("<8I", blob, 8)
    data_psp_off, psar_off = pbp_offs[6], pbp_offs[7]

    out_dir = args.out
    inst_dir = os.path.join(out_dir, "installer")
    os.makedirs(inst_dir, exist_ok=True)

    manifest = {
        "source": os.path.basename(args.pbp),
        "source_sha1": sha1(blob),
        "source_size": len(blob),
        "pbp_sections": dict(zip(["PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "PIC0.PNG",
                                  "PIC1.PNG", "SND0.AT3", "DATA.PSP", "DATA.PSAR"],
                                 ["0x%x" % o for o in pbp_offs])),
        "modules": [],
        "installer": [],
        "other": [],
    }

    def log(msg):
        if not args.quiet:
            print(msg)

    # 1. Find every embedded ELF and every path table / write call site.
    elves = scan_elves(blob)
    targets = {}  # abs data offset -> list of {"path", "how"}
    for elf in elves:
        for h in scan_path_tables(blob, elf):
            targets.setdefault(h["data_abs"], []).append(
                {"path": h["path"], "size": h["size"],
                 "how": "path table in %s at vaddr 0x%x" % (elf.modname, h["table_entry_vaddr"])})
        for h in scan_call_sites(blob, elf):
            targets.setdefault(h["data_abs"], []).append(
                {"path": h["path"], "size": h["size"],
                 "how": "write call in %s at vaddr 0x%x" % (elf.modname, h["call_site_vaddr"])})

    used_names = set()

    def unique(dirname, name):
        base, ext = os.path.splitext(name)
        cand, k = name, 1
        while (dirname, cand.lower()) in used_names:
            k += 1
            cand = "%s_%d%s" % (base, k, ext)
        used_names.add((dirname, cand.lower()))
        return cand

    def write(dirname, name, data):
        path = os.path.join(dirname, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def rel(path):
        return os.path.relpath(path, out_dir).replace("\\", "/")

    # 2. ~PSP containers -> plain PRX files.
    for off, hdr, elf in scan_psp_containers(blob):
        t = targets.get(off, [])
        flash_paths = sorted({x["path"] for x in t})
        if flash_paths:
            name = os.path.basename(flash_paths[0])
        else:
            name = FALLBACK_NAMES.get(hdr["module_name"],
                                      hdr["module_name"].lower().replace(" ", "_") + ".prx")
        name = unique(out_dir, name)
        path = write(out_dir, name, elf)
        e = EmbeddedElf(elf, 0)
        entry = {
            "file": rel(path),
            "module_name": hdr["module_name"],
            "attributes": "0x%04X" % hdr["attribute"],
            "module_version": hdr["module_version"],
            "kind": "kernel" if hdr["attribute"] & 0x1000 else ("vsh" if hdr["attribute"] & 0x800 else "user"),
            "size": len(elf),
            "sha1": sha1(elf),
            "source_offset": "0x%x" % off,
            "packed_size": hdr["psp_size"],
            "packed_sha1": sha1(blob[off:off + hdr["psp_size"]]),
            "decrypt_mode": hdr["decrypt_mode"],
            "tag": "0x%08X" % hdr["tag"],
            "oe_tag": "0x%08X" % hdr["oe_tag"],
            "flash_paths": flash_paths,
            "name_evidence": sorted({x["how"] for x in t}) or ["fallback: module name"],
            "elf_type": "0x%04X" % e.e_type,
        }
        manifest["modules"].append(entry)
        log("module    %-24s %-22s attr=%s size=%6d from 0x%x" % (
            name, hdr["module_name"], entry["attributes"], len(elf), off))

    # 3. Plain ELFs embedded in the installer (and the installer itself).
    pbps = scan_pbps(blob)
    pbp_data_psp = {}  # abs ELF offset -> PBP offset
    for poff, offs in pbps:
        pbp_data_psp[poff + offs[6]] = poff
    for elf in elves:
        if elf.off == data_psp_off:
            name, container = "data.psp.prx", "EBOOT.PBP DATA.PSP"
        elif elf.off in pbp_data_psp and elf.modname == "plutonium_updater":
            name, container = "pupd_data.psp.prx", "PUPD.PBP DATA.PSP"
        else:
            name = INSTALLER_NAMES.get(elf.modname, elf.modname.lower() + ".prx")
            container = None
            for other in elves:
                if other is not elf and other.off < elf.off < other.off + other.size:
                    if container is None or other.off > container[0]:
                        container = (other.off, other.modname)
            container = ("inside %s" % container[1]) if container else "top level"
        name = unique(inst_dir, name)
        data = blob[elf.off:elf.off + elf.size]
        path = write(inst_dir, name, data)
        manifest["installer"].append({
            "file": rel(path),
            "module_name": elf.modname,
            "attributes": "0x%04X" % elf.attr,
            "kind": "kernel" if elf.attr & 0x1000 else ("vsh" if elf.attr & 0x800 else "user"),
            "size": len(data),
            "sha1": sha1(data),
            "source_offset": "0x%x" % elf.off,
            "container": container,
            "elf_type": "0x%04X" % elf.e_type,
        })
        log("installer %-24s %-22s attr=0x%04X size=%6d from 0x%x" % (
            name, elf.modname, elf.attr, len(data), elf.off))

    # 4. Embedded PBPs (other than the input itself).
    pbp_names = {"plutonium_updater": "PUPD.PBP", "TheDownloader": "DL.PBP"}
    for poff, offs in pbps:
        if poff == 0:
            continue
        size = offs[7]  # DATA.PSAR is empty in these PBPs: size = PSAR offset
        inner = poff + offs[6]
        modname = next((e.modname for e in elves if e.off == inner), "")
        name = unique(inst_dir, pbp_names.get(modname, "pbp_%x.pbp" % poff))
        data = blob[poff:poff + size]
        path = write(inst_dir, name, data)
        manifest["other"].append({"file": rel(path), "type": "pbp", "size": size,
                                  "sha1": sha1(data), "source_offset": "0x%x" % poff,
                                  "data_psp_module": modname})
        log("pbp       %-24s size=%6d from 0x%x" % (name, size, poff))

    # 5. Boot configuration files.
    for off, size in scan_btcnf(blob):
        t = targets.get(off, [])
        flash_paths = sorted({x["path"] for x in t})
        data = blob[off:off + size]
        if flash_paths:
            dirname, name = out_dir, os.path.basename(flash_paths[0])
        else:
            # The updater driver's own configs are preceded by their file name.
            pre = blob[max(0, off - 0x40):off]
            k = pre.rfind(b"pspbt")
            name = cstr(pre, k) if k >= 0 else "btcnf_%x.bin" % off
            inside = [e for e in elves if e.off < off < e.off + e.size]
            container = max(inside, key=lambda e: e.off).modname if inside else "installer"
            dirname, name = inst_dir, "%s_%s" % (container.lower(), name)
        name = unique(dirname, name)
        path = write(dirname, name, data)
        info = parse_btcnf(data)
        manifest["other"].append({"file": rel(path), "type": "btcnf", "size": size,
                                  "sha1": sha1(data), "source_offset": "0x%x" % off,
                                  "flash_paths": flash_paths,
                                  "modes": len(info["modes"]),
                                  "module_count": len(info["modules"])})
        log("btcnf     %-24s modes=%d modules=%d from 0x%x" % (
            name, len(info["modes"]), len(info["modules"]), off))

    # 6. Anything else referenced by a write call / path table (e.g. the
    #    recovery translation) that we have not emitted yet.
    emitted = {int(x["source_offset"], 16) for x in manifest["modules"] + manifest["other"]}
    for da, t in sorted(targets.items()):
        if da in emitted:
            continue
        size = t[0]["size"]
        name = unique(out_dir, os.path.basename(t[0]["path"]))
        data = blob[da:da + size]
        path = write(out_dir, name, data)
        manifest["other"].append({"file": rel(path), "type": "file", "size": size,
                                  "sha1": sha1(data), "source_offset": "0x%x" % da,
                                  "flash_paths": sorted({x["path"] for x in t}),
                                  "name_evidence": sorted({x["how"] for x in t})})
        log("file      %-24s size=%6d from 0x%x" % (name, size, da))

    # 7. Payloads embedded (gzip) inside the extracted modules themselves, e.g.
    #    systemctrl's reboot.bin patch code.  Written to <out>/payloads/.
    pay_dir = os.path.join(out_dir, "payloads")
    manifest["payloads"] = []
    for entry in manifest["modules"]:
        mdata = open(os.path.join(out_dir, entry["file"]), "rb").read()
        for poff in find_all(mdata, b"\x1f\x8b\x08"):
            d = zlib.decompressobj(16 + zlib.MAX_WBITS)
            try:
                payload = d.decompress(mdata[poff:]) + d.flush()
            except zlib.error:
                continue
            if not payload or not d.eof:
                continue
            os.makedirs(pay_dir, exist_ok=True)
            stem = os.path.splitext(os.path.basename(entry["file"]))[0]
            name = unique(pay_dir, "%s_gz_%x.bin" % (stem, poff))
            path = write(pay_dir, name, payload)
            info = {"file": rel(path), "container": entry["file"], "container_offset": "0x%x" % poff,
                    "compressed_size": len(mdata[poff:]) - len(d.unused_data),
                    "size": len(payload), "sha1": sha1(payload)}
            # A leading MIPS "j" gives the load segment of raw code payloads.
            w0 = u32(payload, 0) if len(payload) >= 4 else 0
            if w0 >> 26 == 2:
                info["first_jump_target_low28"] = "0x%07X" % ((w0 & 0x3FFFFFF) << 2)
            manifest["payloads"].append(info)
            log("payload   %-24s size=%6d from %s+0x%x" % (name, len(payload), entry["file"], poff))

    with open(os.path.join(out_dir, "manifest.json"), "w", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    log("wrote %s (%d modules, %d installer ELFs, %d other)" % (
        os.path.join(out_dir, "manifest.json"), len(manifest["modules"]),
        len(manifest["installer"]), len(manifest["other"])))
    if len(manifest["modules"]) != 11:
        print("warning: expected 11 ~PSP modules, found %d" % len(manifest["modules"]),
              file=sys.stderr)


if __name__ == "__main__":
    main()
