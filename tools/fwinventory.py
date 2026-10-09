#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 PSPC contributors
"""Inventory PSP ELF/PRX modules: headers, module info, imports, exports, relocations.

Usage:
    python -I tools/fwinventory.py <file-or-dir> [<file-or-dir> ...]
        [-o private/fw/401/inventory.json] [--md out.md] [--dot out.dot]
        [--nids data/nids.csv] [--relocs]

For every input it records the ELF header, program and section headers, the
PSP module info (from ``.rodata.sceModuleInfo`` when section headers exist,
otherwise from the first program header's ``p_paddr`` for PRX files), the
library exports (``.lib.ent``) and imports (``.lib.stub``) with their NIDs,
the PSP type-A relocation sections (``SHT 0x700000A0``) and type-B
relocations (program header type ``0x700000A1``), a histogram of relocation
types and the SHA-1 of the file.

Also understood:
  * ``~PSP`` containers whose payload is gzip (M33/psp-packer format) are
    unpacked in memory first; encrypted ones are listed as such.
  * PBP files: the DATA.PSP member is inventoried.
  * ``pspbtcnf`` boot configuration files (magic 0x0F803001): modes and
    module lists.

Outputs (paths default next to the JSON):
  inventory.json   machine-readable inventory
  inventory.md     human-readable summary
  imports.dot      Graphviz import dependency graph (module -> provider)
"""

import argparse
import csv
import hashlib
import json
import os
import struct
import sys
import zlib
from collections import Counter, defaultdict

SHT_PRXRELOC = 0x700000A0
PT_PRXRELOC = 0x700000A0
PT_PRXRELOC2 = 0x700000A1
BTCNF_MAGIC = 0x0F803001

SYSLIB_NIDS = {
    0xD3744BE0: "module_bootstart",
    0x2F064FA6: "module_reboot_before",
    0xADF12745: "module_reboot_phase",
    0xD632ACDB: "module_start",
    0xCEE8593C: "module_stop",
    0xF01D73A7: "module_info",
    0x0F7C276C: "module_start_thread_parameter",
    0xCF0CC697: "module_stop_thread_parameter",
    0x11B97506: "module_sdk_version",
}

# MIPS relocation type names as used in PSP type-A relocation sections.
RELOC_A_NAMES = {
    0: "R_MIPS_NONE", 1: "R_MIPS_16", 2: "R_MIPS_32", 3: "R_MIPS_REL32",
    4: "R_MIPS_26", 5: "R_MIPS_HI16", 6: "R_MIPS_LO16", 7: "R_MIPS_GPREL16",
    8: "R_MIPS_LITERAL", 9: "R_MIPS_GOT16", 10: "R_MIPS_PC16", 11: "R_MIPS_CALL16",
    12: "R_MIPS_GPREL32",
}
# Type-B (PT 0x700000A1) relocation types after the type table lookup.
RELOC_B_NAMES = {
    0: "NONE", 1: "LO16(1)", 2: "R_MIPS_32", 3: "R_MIPS_26", 4: "R_MIPS_HI16",
    5: "R_MIPS_LO16", 6: "J26", 7: "JAL26",
}

ELF_TYPES = {1: "REL", 2: "EXEC", 3: "DYN", 0xFFA0: "PRX", 0xFFA1: "PRX2"}


def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def cstr(b, o, maxlen=256):
    if o is None or o < 0 or o >= len(b):
        return None
    end = b.find(b"\0", o, o + maxlen)
    if end < 0:
        end = min(len(b), o + maxlen)
    return b[o:end].decode("latin-1")


def hx(v):
    return "0x%08X" % v


# --------------------------------------------------------------------------
# NID database
# --------------------------------------------------------------------------

class NidDb:
    def __init__(self, path=None):
        self.by_lib = {}
        self.by_nid = defaultdict(set)
        if path and os.path.exists(path):
            with open(path, newline="", encoding="utf-8") as f:
                for row in csv.DictReader(f):
                    try:
                        nid = int(row["nid"], 16)
                    except (KeyError, ValueError):
                        continue
                    self.by_lib[(row["library"], nid)] = row["name"]
                    self.by_nid[nid].add(row["name"])

    def name(self, lib, nid):
        if lib is None and nid in SYSLIB_NIDS:
            return SYSLIB_NIDS[nid]
        n = self.by_lib.get((lib, nid))
        if n:
            return n
        cands = self.by_nid.get(nid)
        if cands and len(cands) == 1:
            return next(iter(cands))
        if nid in SYSLIB_NIDS:
            return SYSLIB_NIDS[nid]
        return None


# --------------------------------------------------------------------------
# ELF / PRX parsing
# --------------------------------------------------------------------------

class Elf:
    def __init__(self, data):
        self.data = data
        if data[:4] != b"\x7fELF":
            raise ValueError("not an ELF file")
        if data[4] != 1 or data[5] != 1:
            raise ValueError("not a 32-bit little-endian ELF")
        (self.e_type, self.e_machine, self.e_version, self.e_entry, self.e_phoff,
         self.e_shoff, self.e_flags, self.e_ehsize, self.e_phentsize, self.e_phnum,
         self.e_shentsize, self.e_shnum, self.e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
        self.phdrs = []
        for i in range(self.e_phnum):
            o = self.e_phoff + i * self.e_phentsize
            if o + 32 > len(data):
                break
            t, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<8I", data, o)
            self.phdrs.append(dict(type=t, offset=off, vaddr=va, paddr=pa, filesz=fsz,
                                   memsz=msz, flags=fl, align=al))
        self.shdrs = []
        if self.e_shoff and self.e_shnum and self.e_shoff + self.e_shnum * 40 <= len(data):
            for i in range(self.e_shnum):
                nm, t, fl, addr, off, sz, link, info, al, es = struct.unpack_from(
                    "<10I", data, self.e_shoff + i * 40)
                self.shdrs.append(dict(name_off=nm, type=t, flags=fl, addr=addr, offset=off,
                                       size=sz, link=link, info=info, addralign=al, entsize=es))
            if self.e_shstrndx < len(self.shdrs):
                so = self.shdrs[self.e_shstrndx]["offset"]
                for s in self.shdrs:
                    s["name"] = cstr(data, so + s["name_off"], 64) or ""
        for s in self.shdrs:
            s.setdefault("name", "")
        self.is_prx = self.e_type in (0xFFA0, 0xFFA1)

    def loads(self):
        return [p for p in self.phdrs if p["type"] == 1]

    def v2o(self, va):
        """Translate a virtual address to a file offset (None if not file-backed)."""
        for p in self.loads():
            if p["vaddr"] <= va < p["vaddr"] + p["filesz"]:
                return p["offset"] + (va - p["vaddr"])
        return None

    def section(self, name):
        for s in self.shdrs:
            if s["name"] == name:
                return s
        return None

    def str_at(self, va, maxlen=256):
        o = self.v2o(va)
        return cstr(self.data, o, maxlen) if o is not None else None


def parse_modinfo(elf):
    d = elf.data
    s = elf.section(".rodata.sceModuleInfo")
    where = None
    if s is not None and s["type"] != 8:
        off = s["offset"]
        where = ".rodata.sceModuleInfo"
    elif elf.is_prx and elf.phdrs:
        off = elf.phdrs[0]["paddr"] & 0x7FFFFFFF
        where = "phdr[0].p_paddr"
    else:
        return None
    if off + 0x34 > len(d):
        return None
    attr = u16(d, off)
    return {
        "found_in": where,
        "file_offset": off,
        "attribute": attr,
        "version": "%d.%d" % (d[off + 3], d[off + 2]),
        "name": cstr(d, off + 4, 28),
        "gp": u32(d, off + 0x20),
        "ent_top": u32(d, off + 0x24),
        "ent_end": u32(d, off + 0x28),
        "stub_top": u32(d, off + 0x2C),
        "stub_end": u32(d, off + 0x30),
        "kernel_flag_in_paddr": bool(elf.phdrs and (elf.phdrs[0]["paddr"] & 0x80000000)),
    }


def mode_of(attr):
    if attr & 0x1000:
        return "kernel"
    if attr & 0x0800:
        return "vsh"
    return "user"


def parse_exports(elf, mi, nids):
    d = elf.data
    out = []
    va = mi["ent_top"]
    guard = 0
    while va < mi["ent_end"] and guard < 512:
        guard += 1
        o = elf.v2o(va)
        if o is None or o + 16 > len(d):
            break
        name_va, ver, attr = u32(d, o), u16(d, o + 4), u16(d, o + 6)
        size, nvars, nfuncs, tbl = u8(d, o + 8), u8(d, o + 9), u16(d, o + 10), u32(d, o + 12)
        if size < 4:
            break
        lib = elf.str_at(name_va) if name_va else None
        t = elf.v2o(tbl)
        n = nfuncs + nvars
        funcs, vars_ = [], []
        if t is not None and t + 8 * n <= len(d):
            for i in range(n):
                nid, addr = u32(d, t + 4 * i), u32(d, t + 4 * (n + i))
                e = {"nid": hx(nid), "address": hx(addr)}
                nm = nids.name(lib, nid)
                if nm:
                    e["name"] = nm
                (funcs if i < nfuncs else vars_).append(e)
        out.append({"library": lib or "syslib", "version": "0x%04X" % ver, "attribute": "0x%04X" % attr,
                    "syscall_export": bool(attr & 0x4000), "functions": funcs, "variables": vars_})
        va += size * 4
    return out


def parse_imports(elf, mi, nids):
    d = elf.data
    out = []
    va = mi["stub_top"]
    guard = 0
    while va < mi["stub_end"] and guard < 1024:
        guard += 1
        o = elf.v2o(va)
        if o is None or o + 20 > len(d):
            break
        name_va, ver, attr = u32(d, o), u16(d, o + 4), u16(d, o + 6)
        size, nvars, nfuncs = u8(d, o + 8), u8(d, o + 9), u16(d, o + 10)
        nid_va, stub_va = u32(d, o + 12), u32(d, o + 16)
        var_va = u32(d, o + 20) if size >= 6 and o + 24 <= len(d) else 0
        if size < 5:
            break
        lib = elf.str_at(name_va) if name_va else None
        funcs, vars_ = [], []
        t = elf.v2o(nid_va)
        if t is not None:
            for i in range(nfuncs):
                if t + 4 * i + 4 > len(d):
                    break
                nid = u32(d, t + 4 * i)
                e = {"nid": hx(nid), "stub": hx(stub_va + 8 * i)}
                nm = nids.name(lib, nid)
                if nm:
                    e["name"] = nm
                funcs.append(e)
        if nvars and var_va:
            t = elf.v2o(var_va)
            if t is not None:
                for i in range(nvars):
                    if t + 8 * i + 8 > len(d):
                        break
                    e = {"nid": hx(u32(d, t + 8 * i + 4)), "reloc_list": hx(u32(d, t + 8 * i))}
                    nm = nids.name(lib, u32(d, t + 8 * i + 4))
                    if nm:
                        e["name"] = nm
                    vars_.append(e)
        out.append({"library": lib, "version": "0x%04X" % ver, "attribute": "0x%04X" % attr,
                    "functions": funcs, "variables": vars_})
        va += size * 4
    return out


def parse_relocs_a(elf, keep):
    """Type-A relocations: Elf32_Rel entries in SHT_PRXRELOC sections (or PT_PRXRELOC)."""
    d = elf.data
    hist = Counter()
    sections = []
    entries = []
    srcs = [(s["name"], s["offset"], s["size"], s["info"]) for s in elf.shdrs if s["type"] == SHT_PRXRELOC]
    if not srcs:
        srcs = [("phdr[%d]" % i, p["offset"], p["filesz"], None)
                for i, p in enumerate(elf.phdrs) if p["type"] == PT_PRXRELOC]
    for name, off, size, info in srcs:
        n = size // 8
        sections.append({"name": name, "count": n, "applies_to": info})
        for i in range(n):
            if off + 8 * i + 8 > len(d):
                break
            r_off, r_info = struct.unpack_from("<II", d, off + 8 * i)
            t = r_info & 0xFF
            hist[RELOC_A_NAMES.get(t, "type_%d" % t)] += 1
            if keep:
                entries.append([r_off, t, (r_info >> 8) & 0xFF, (r_info >> 16) & 0xFF])
    return sections, hist, entries


def parse_relocs_b(elf, keep):
    """Type-B relocations (PT 0x700000A1), following the PSP loader's bit-packed format."""
    d = elf.data
    hist = Counter()
    entries = []
    segs = []
    for rel_seg, p in enumerate(elf.phdrs):
        if p["type"] != PT_PRXRELOC2:
            continue
        buf, end = p["offset"], p["offset"] + p["filesz"]
        if end > len(d) or p["filesz"] < 4:
            continue
        flag_bits, type_bits = d[buf + 2], d[buf + 3]
        seg_bits = 1
        while (1 << seg_bits) < rel_seg:
            seg_bits += 1
        buf += 4
        flag_table = d[buf:buf + d[buf]]
        buf += d[buf]
        type_table = d[buf:buf + d[buf]]
        buf += d[buf]
        count = 0
        rel_base = 0
        off_seg = 0
        bad = False
        while buf + 2 <= end:
            cmd = d[buf] | (d[buf + 1] << 8)
            buf += 2
            flag = ((cmd << (16 - flag_bits)) & 0xFFFF) >> (16 - flag_bits)
            if flag >= len(flag_table):
                bad = True
                break
            flag = flag_table[flag]
            seg = ((cmd << (16 - seg_bits - flag_bits)) & 0xFFFF) >> (16 - seg_bits)
            typ = ((cmd << (16 - type_bits - seg_bits - flag_bits)) & 0xFFFF) >> (16 - type_bits)
            if typ >= len(type_table):
                bad = True
                break
            typ = type_table[typ]
            if (flag & 1) == 0:
                off_seg = seg
                if (flag & 6) == 0:
                    rel_base = cmd >> (seg_bits + flag_bits)
                elif (flag & 6) == 4:
                    rel_base = u32(d, buf)
                    buf += 4
                continue
            addr_seg = seg
            sh = type_bits + seg_bits + flag_bits
            if (flag & 6) == 0:
                ro = cmd | 0xFFFF0000 if cmd & 0x8000 else cmd
                ro = (ro >> sh) | (0xFFFF0000 if cmd & 0x8000 else 0)
                rel_base = (rel_base + ro) & 0xFFFFFFFF
            elif (flag & 6) == 2:
                ro = cmd | 0xFFFF0000 if cmd & 0x8000 else cmd
                ro >>= sh
                ro = ((ro << 16) | d[buf] | (d[buf + 1] << 8)) & 0xFFFFFFFF
                buf += 2
                rel_base = (rel_base + ro) & 0xFFFFFFFF
            elif (flag & 6) == 4:
                rel_base = u32(d, buf)
                buf += 4
            if (flag & 0x38) == 0x10:
                buf += 2
            hist[RELOC_B_NAMES.get(typ, "type_%d" % typ)] += 1
            count += 1
            if keep:
                entries.append([rel_base, typ, off_seg, addr_seg])
        segs.append({"phdr": rel_seg, "count": count, "flag_bits": flag_bits, "type_bits": type_bits,
                     "seg_bits": seg_bits, "decode_error": bad})
    return segs, hist, entries


def inventory_elf(data, nids, keep_relocs):
    elf = Elf(data)
    info = {
        "format": "elf",
        "elf": {
            "type": "0x%04X" % elf.e_type,
            "type_name": ELF_TYPES.get(elf.e_type, "?"),
            "machine": elf.e_machine,
            "entry": hx(elf.e_entry),
            "flags": hx(elf.e_flags),
            "phnum": elf.e_phnum,
            "shnum": elf.e_shnum,
        },
        "program_headers": [
            {"type": hx(p["type"]), "offset": hx(p["offset"]), "vaddr": hx(p["vaddr"]),
             "paddr": hx(p["paddr"]), "filesz": p["filesz"], "memsz": p["memsz"],
             "flags": p["flags"], "align": p["align"]} for p in elf.phdrs],
        "sections": [
            {"name": s["name"], "type": hx(s["type"]), "flags": hx(s["flags"]), "addr": hx(s["addr"]),
             "offset": hx(s["offset"]), "size": s["size"], "info": s["info"]} for s in elf.shdrs],
    }
    text = sum(s["size"] for s in elf.shdrs if s["flags"] & 4)
    if not elf.shdrs:
        text = sum(p["filesz"] for p in elf.loads() if p["flags"] & 1)
    info["text_size"] = text
    info["mem_size"] = sum(p["memsz"] for p in elf.loads())
    mi = parse_modinfo(elf)
    if mi:
        info["module_info"] = dict(mi, attribute="0x%04X" % mi["attribute"], gp=hx(mi["gp"]),
                                   ent_top=hx(mi["ent_top"]), ent_end=hx(mi["ent_end"]),
                                   stub_top=hx(mi["stub_top"]), stub_end=hx(mi["stub_end"]),
                                   mode=mode_of(mi["attribute"]))
        info["exports"] = parse_exports(elf, mi, nids)
        info["imports"] = parse_imports(elf, mi, nids)
    else:
        info["exports"], info["imports"] = [], []
    sa, ha, ea = parse_relocs_a(elf, keep_relocs)
    sb, hb, eb = parse_relocs_b(elf, keep_relocs)
    info["relocations"] = {
        "type_a_sections": sa,
        "type_b_segments": sb,
        "histogram_a": dict(sorted(ha.items())),
        "histogram_b": dict(sorted(hb.items())),
        "total": sum(ha.values()) + sum(hb.values()),
    }
    if keep_relocs:
        info["relocations"]["entries_a"] = ea  # [r_offset, type, ofs_base, addr_base]
        info["relocations"]["entries_b"] = eb  # [offset, type, off_seg, addr_seg]
    return info


def parse_btcnf(data):
    nmodes, nmods = u32(data, 0x14), u32(data, 0x24)
    modestart, modstart, namestart = u32(data, 0x10), u32(data, 0x20), u32(data, 0x30)
    mods = []
    for i in range(nmods):
        e = modstart + i * 0x20
        mods.append({"path": cstr(data, namestart + u32(data, e)), "flags": "0x%08X" % u32(data, e + 8)})
    modes = []
    for i in range(nmodes):
        e = modestart + i * 0x20
        count, first, mask, key = u16(data, e), u16(data, e + 2), u32(data, e + 4), u32(data, e + 8)
        members = [m["path"] for m in mods[first:first + count] if int(m["flags"], 16) & mask & 0xFFFF]
        modes.append({"key": "0x%X" % key, "mask": "0x%X" % mask, "first": first, "count": count,
                      "modules": members})
    return {"format": "btcnf", "devkit": "0x%08X" % u32(data, 4), "modes": modes, "modules": mods}


def inventory_file(path, data, nids, keep_relocs):
    rec = {"path": path, "size": len(data), "sha1": hashlib.sha1(data).hexdigest()}
    try:
        if data[:4] == b"\x7fELF":
            rec.update(inventory_elf(data, nids, keep_relocs))
        elif data[:4] == b"~PSP":
            rec["container"] = {"type": "~PSP", "module_name": cstr(data, 0xA, 28),
                                "attribute": "0x%04X" % u16(data, 4),
                                "tag": "0x%08X" % u32(data, 0xD0),
                                "decrypt_mode": u32(data, 0x7C)}
            if data[0x150:0x152] == b"\x1f\x8b":
                elf = zlib.decompress(data[0x150:], 16 + zlib.MAX_WBITS)
                rec["container"]["unpacked_sha1"] = hashlib.sha1(elf).hexdigest()
                rec.update(inventory_elf(elf, nids, keep_relocs))
            else:
                rec["format"] = "encrypted"
        elif data[:4] == b"\x00PBP":
            offs = struct.unpack_from("<8I", data, 8)
            inner = data[offs[6]:offs[7]]
            rec["container"] = {"type": "PBP", "data_psp_offset": hx(offs[6]), "data_psp_size": len(inner)}
            if inner[:4] == b"\x7fELF":
                rec.update(inventory_elf(inner, nids, keep_relocs))
            else:
                rec["format"] = "encrypted" if inner[:4] == b"~PSP" else "unknown"
        elif len(data) >= 0x40 and u32(data, 0) == BTCNF_MAGIC:
            rec.update(parse_btcnf(data))
        else:
            return None
    except (ValueError, struct.error, zlib.error, IndexError) as e:
        rec["format"] = "error"
        rec["error"] = str(e)
    return rec


def collect(inputs):
    files = []
    for p in inputs:
        if os.path.isdir(p):
            for root, dirs, names in os.walk(p):
                dirs.sort()
                for n in sorted(names):
                    files.append(os.path.join(root, n))
        elif os.path.isfile(p):
            files.append(p)
        else:
            print("warning: %s does not exist" % p, file=sys.stderr)
    return files


# --------------------------------------------------------------------------
# Reports
# --------------------------------------------------------------------------

def module_key(rec):
    mi = rec.get("module_info")
    return mi["name"] if mi else os.path.basename(rec["path"])


def build_providers(mods):
    prov = defaultdict(list)
    for r in mods:
        for e in r.get("exports", []):
            if e["library"] != "syslib":
                prov[e["library"]].append(r)
    return prov


def write_dot(path, mods):
    prov = build_providers(mods)
    lines = ["// Generated by tools/fwinventory.py", "digraph imports {", "  rankdir=LR;",
             '  node [shape=box, fontname="Helvetica", fontsize=10];',
             '  edge [fontname="Helvetica", fontsize=8];']
    ids = {}

    def nid_of(label):
        if label not in ids:
            ids[label] = "n%d" % len(ids)
        return ids[label]

    colors = {"kernel": "#f4cccc", "vsh": "#cfe2f3", "user": "#d9ead3"}
    for r in mods:
        mi = r["module_info"]
        lbl = "%s\\n%s" % (mi["name"], os.path.basename(r["path"]))
        lines.append('  %s [label="%s", style=filled, fillcolor="%s"];' % (
            nid_of(("m", r["path"])), lbl, colors.get(mi["mode"], "white")))
    external = set()
    edges = Counter()
    for r in mods:
        src = nid_of(("m", r["path"]))
        for imp in r.get("imports", []):
            lib = imp["library"] or "?"
            ps = [p for p in prov.get(lib, []) if p is not r]
            if ps:
                for p in ps:
                    edges[(src, nid_of(("m", p["path"])), lib)] += len(imp["functions"])
            else:
                external.add(lib)
                edges[(src, nid_of(("l", lib)), None)] += len(imp["functions"])
    for lib in sorted(external):
        lines.append('  %s [label="%s", shape=ellipse, style=dashed];' % (nid_of(("l", lib)), lib))
    for (s, t, lib), n in sorted(edges.items()):
        if lib:
            lines.append('  %s -> %s [label="%s (%d)"];' % (s, t, lib, n))
        else:
            lines.append('  %s -> %s [label="%d", style=dashed];' % (s, t, n))
    lines.append("}")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def write_md(path, records, root):
    mods = [r for r in records if r.get("module_info")]
    lines = ["# Firmware module inventory", "",
             "Generated by `tools/fwinventory.py`. %d files, %d modules." % (len(records), len(mods)), ""]
    lines += ["| File | Module | Ver | Mode | Attr | Text | Exports (libs/funcs/vars) | "
              "Imports (libs/funcs) | Relocs A/B | SHA-1 |",
              "|---|---|---|---|---|---|---|---|---|---|"]
    for r in mods:
        mi = r["module_info"]
        ex = [e for e in r["exports"] if e["library"] != "syslib"]
        lines.append("| %s | %s | %s | %s | %s | %d | %d / %d / %d | %d / %d | %d / %d | `%s` |" % (
            os.path.relpath(r["path"], root).replace("\\", "/"), mi["name"], mi["version"], mi["mode"],
            mi["attribute"], r["text_size"], len(ex), sum(len(e["functions"]) for e in ex),
            sum(len(e["variables"]) for e in ex), len(r["imports"]),
            sum(len(i["functions"]) for i in r["imports"]),
            sum(r["relocations"]["histogram_a"].values()), sum(r["relocations"]["histogram_b"].values()),
            r["sha1"][:12]))
    others = [r for r in records if not r.get("module_info")]
    if others:
        lines += ["", "## Other files", "", "| File | Format | Size | SHA-1 |", "|---|---|---|---|"]
        for r in others:
            lines.append("| %s | %s | %d | `%s` |" % (os.path.relpath(r["path"], root).replace("\\", "/"),
                                                     r.get("format", "?"), r["size"], r["sha1"][:12]))
    for r in mods:
        mi = r["module_info"]
        lines += ["", "## %s (`%s`)" % (mi["name"], os.path.basename(r["path"])), ""]
        lines.append("Attribute %s (%s), version %s, entry %s, gp %s, %d relocations (%s)." % (
            mi["attribute"], mi["mode"], mi["version"], r["elf"]["entry"], mi["gp"],
            r["relocations"]["total"],
            ", ".join("%s %d" % kv for kv in list(r["relocations"]["histogram_a"].items()) +
                      list(r["relocations"]["histogram_b"].items())) or "none"))
        if r["exports"]:
            lines += ["", "Exports:", ""]
            for e in r["exports"]:
                fn = ", ".join("%s%s" % (f["nid"], " " + f["name"] if "name" in f else "") for f in e["functions"])
                vn = ", ".join("%s%s" % (f["nid"], " " + f["name"] if "name" in f else "") for f in e["variables"])
                lines.append("- **%s** (attr %s): %s%s" % (e["library"], e["attribute"], fn or "-",
                                                           ("; vars: " + vn) if vn else ""))
        if r["imports"]:
            lines += ["", "Imports:", ""]
            for i in r["imports"]:
                named = sum(1 for f in i["functions"] if "name" in f)
                lines.append("- %s: %d functions (%d named)%s" % (
                    i["library"], len(i["functions"]), named,
                    ", %d variables" % len(i["variables"]) if i["variables"] else ""))
    bt = [r for r in records if r.get("format") == "btcnf"]
    for r in bt:
        lines += ["", "## Boot config `%s`" % os.path.basename(r["path"]), ""]
        for m in r["modes"]:
            lines.append("- mode key %s mask %s: %d modules: %s" % (
                m["key"], m["mask"], len(m["modules"]), ", ".join(os.path.basename(x) for x in m["modules"])))
    # Imports by NID
    by_nid = defaultdict(set)
    names = {}
    for r in mods:
        for i in r["imports"]:
            for f in i["functions"]:
                k = (i["library"] or "?", f["nid"])
                by_nid[k].add(r["module_info"]["name"])
                if "name" in f:
                    names[k] = f["name"]
    lines += ["", "## Imports by NID", "", "| Library | NID | Name | Imported by |", "|---|---|---|---|"]
    for k in sorted(by_nid):
        lines.append("| %s | %s | %s | %s |" % (k[0], k[1], names.get(k, ""), ", ".join(sorted(by_nid[k]))))
    with open(path, "w", newline="\n", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description="Inventory PSP ELF/PRX modules.")
    ap.add_argument("inputs", nargs="+", help="files or directories to scan")
    ap.add_argument("-o", "--out", default=os.path.join("private", "fw", "401", "inventory.json"))
    ap.add_argument("--md", help="markdown summary path (default: <out>.md)")
    ap.add_argument("--dot", help="Graphviz import graph path (default: <out dir>/imports.dot)")
    ap.add_argument("--nids", default=os.path.join("data", "nids.csv"), help="NID database CSV")
    ap.add_argument("--relocs", action="store_true", help="include every relocation entry in the JSON")
    args = ap.parse_args()

    nids = NidDb(args.nids)
    files = collect(args.inputs)
    root = os.path.commonpath([os.path.abspath(p) if os.path.isdir(p) else os.path.dirname(os.path.abspath(p))
                               for p in args.inputs])
    records = []
    for p in files:
        with open(p, "rb") as f:
            data = f.read()
        rec = inventory_file(p, data, nids, args.relocs)
        if rec is not None:
            rec["path"] = os.path.relpath(os.path.abspath(p), root).replace("\\", "/")
            records.append(rec)
    out = {"generator": "tools/fwinventory.py", "root": root.replace("\\", "/"),
           "nid_db": args.nids if os.path.exists(args.nids) else None, "files": records}
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="\n") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    md = args.md or os.path.splitext(args.out)[0] + ".md"
    dot = args.dot or os.path.join(os.path.dirname(os.path.abspath(args.out)), "imports.dot")
    # Paths in reports are relative to root.
    for r in records:
        r["path"] = os.path.join(root, r["path"])
    write_md(md, records, root)
    write_dot(dot, [r for r in records if r.get("module_info")])
    mods = [r for r in records if r.get("module_info")]
    print("%d files, %d modules -> %s, %s, %s" % (len(records), len(mods), args.out, md, dot))
    for r in mods:
        mi = r["module_info"]
        ex = [e for e in r["exports"] if e["library"] != "syslib"]
        print("  %-26s %-24s %-6s exports %2d libs/%3d funcs  imports %2d libs/%3d funcs  relocs %d" % (
            os.path.relpath(r["path"], root).replace("\\", "/"), mi["name"], mi["mode"], len(ex),
            sum(len(e["functions"]) for e in ex), len(r["imports"]),
            sum(len(i["functions"]) for i in r["imports"]), r["relocations"]["total"]))


if __name__ == "__main__":
    main()
