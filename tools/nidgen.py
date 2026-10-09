# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 PSPC contributors
"""Build the PSPC NID database from the local reference trees.

Usage:
    python -I tools/nidgen.py [--pspsdk DIR] [--ppsspp DIR] [--uofw DIR]
        [--m33 DIR] [-o data/nids.csv] [--json data/nids_by_library.json]

Harvesters (every input tree is only read, never written):
  pspsdk  src/**/*.S   IMPORT_START/IMPORT_FUNC[_WITH_ALIAS] and
                       STUB_START/STUB_FUNC[_WITH_ALIAS]/STUB_END stubs
  ppsspp  Core/**      ``HLEFunction NAME[] = {...}`` tables, joined to their
                       ``RegisterHLEModule("lib", ..., NAME)`` calls and the
                       ``{"lib", ARRAY_SIZE(NAME), NAME}`` moduleList entries
                       (tables and registrations may live in different files)
  uofw    **/*.exp     PSP_EXPORT_START / PSP_EXPORT_{FUNC,VAR}[_NID|_HASH]
          include/modulemgr_nids.h   syslib NIDs (only hash-verified names)
  m33     (4.01 M33 tree) SDK/lib/*.a stub archives (ar of MIPS ELF REL
          objects: .rodata.sceNid holds the NIDs, the .sceStub.text symbols
          give the names, the ``__stub_module_<lib>`` symbol / relocation or
          .rodata.sceResident string gives the library), plus any *.S stubs
          and *.exp export files; SDK/include/*.h is used to cross-check.

Outputs:
  nids.csv               library,nid,name,source,hash_ok  (one row per
                         (library, nid), sorted by library then nid)
  nids_by_library.json   {"<library>": {"0xNID": "name", ...}, ...}

When several names compete for one (library, nid) the winner is the first by:
hash-verified name (SHA-1 NID matches), real name over a ``Lib_XXXXXXXX``
placeholder, source priority uofw > pspsdk > ppsspp > m33, primary name over
an alias, first seen.
"""

import argparse
import csv
import hashlib
import json
import os
import re
import struct
import sys
from collections import Counter, defaultdict

PRIORITY = {"uofw": 0, "pspsdk": 1, "ppsspp": 2, "m33": 3}
MAX_SOURCES = 3
SKIP_PPSSPP_LIBS = {"FakeSysCalls", "pspeDebug"}  # emulator-only pseudo libraries


def nid_of(name):
    return struct.unpack("<I", hashlib.sha1(name.encode()).digest()[:4])[0]


_PLACEHOLDER = re.compile(r"^\w*_[0-9A-Fa-f]{8}$")


def is_placeholder(name, nid=None):
    """``Lib_XXXXXXXX``-style unknown name (the hex is usually, not always, the NID)."""
    return bool(_PLACEHOLDER.match(name))


def relpath(path, root):
    return os.path.relpath(path, root).replace(os.sep, "/")


def walk_files(root, exts, skip_dirs=(".git",)):
    for dp, dns, fns in os.walk(root):
        dns[:] = sorted(d for d in dns if d not in skip_dirs)
        for fn in sorted(fns):
            if fn.endswith(exts):
                yield os.path.join(dp, fn)


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", errors="replace")


class Harvest:
    """Candidate (library, nid, name) records with provenance."""

    def __init__(self):
        self.recs = []          # (lib, nid, name, source, is_alias, order)
        self.warnings = []
        self.notes = []

    def note(self, msg):
        self.notes.append(msg)

    def add(self, lib, nid, name, source, alias=False):
        if not lib or not name:
            return
        self.recs.append((lib, nid & 0xFFFFFFFF, name, source, alias, len(self.recs)))

    def warn(self, msg):
        self.warnings.append(msg)


# --------------------------------------------------------------------------
# Comment stripping (C, C++ and GAS-with-cpp sources)
# --------------------------------------------------------------------------

def strip_c_comments(text):
    """Remove // and /* */ comments while keeping string/char literals."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            seg = text[i:n if j < 0 else j + 2]
            out.append("\n" * seg.count("\n"))
            i = n if j < 0 else j + 2
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


# --------------------------------------------------------------------------
# GAS stub sources (pspsdk, M33 samples)
# --------------------------------------------------------------------------

_HEX = r"(0[xX][0-9A-Fa-f]+)"
_ID = r"([A-Za-z_.$][\w.$]*)"
RE_IMPORT_FUNC = re.compile(r'\bIMPORT_FUNC\s+"([^"]+)"\s*,\s*' + _HEX + r"\s*,\s*" + _ID)
RE_IMPORT_ALIAS = re.compile(r'\bIMPORT_FUNC_WITH_ALIAS\s+"([^"]+)"\s*,\s*' + _HEX
                             + r"\s*,\s*" + _ID + r"\s*,\s*" + _ID)
RE_STUB_START = re.compile(r'\bSTUB_START\s+"([^"]+)"')
RE_STUB_FUNC = re.compile(r"\bSTUB_FUNC\s+" + _HEX + r"\s*,\s*" + _ID)
RE_STUB_ALIAS = re.compile(r"\bSTUB_FUNC_WITH_ALIAS\s+" + _HEX + r"\s*,\s*" + _ID + r"\s*,\s*" + _ID)
RE_STUB_END = re.compile(r"\bSTUB_END\b")


def harvest_stub_source(h, path, tag):
    lib = None
    for line in strip_c_comments(read_text(path)).splitlines():
        line = line.split("#", 1)[0]    # cpp directive or MIPS GAS '#' comment
        m = RE_IMPORT_ALIAS.search(line)
        if m:
            h.add(m.group(1), int(m.group(2), 16), m.group(3), tag)
            h.add(m.group(1), int(m.group(2), 16), m.group(4), tag, alias=True)
            continue
        m = RE_IMPORT_FUNC.search(line)
        if m:
            h.add(m.group(1), int(m.group(2), 16), m.group(3), tag)
            continue
        m = RE_STUB_START.search(line)
        if m:
            lib = m.group(1)
            continue
        m = RE_STUB_ALIAS.search(line)
        if m:
            if lib is None:
                h.warn("%s: STUB_FUNC_WITH_ALIAS outside STUB_START" % tag)
                continue
            h.add(lib, int(m.group(1), 16), m.group(2), tag)
            h.add(lib, int(m.group(1), 16), m.group(3), tag, alias=True)
            continue
        m = RE_STUB_FUNC.search(line)
        if m:
            if lib is None:
                h.warn("%s: STUB_FUNC outside STUB_START" % tag)
                continue
            h.add(lib, int(m.group(1), 16), m.group(2), tag)
            continue
        if RE_STUB_END.search(line):
            lib = None


def harvest_pspsdk(h, root):
    src = os.path.join(root, "src")
    for p in walk_files(src, (".S", ".s")):
        harvest_stub_source(h, p, "pspsdk:" + relpath(p, root))


# --------------------------------------------------------------------------
# Export files (uofw, M33 samples)
# --------------------------------------------------------------------------

RE_EXP_START = re.compile(r"PSP_EXPORT_START\s*\(\s*(\w+)\s*,")
RE_EXP_NID = re.compile(r"PSP_EXPORT_(?:FUNC|VAR)_NID\s*\(\s*([^,()]+?)\s*,\s*" + _HEX + r"\s*\)")
RE_EXP_HASH = re.compile(r"PSP_EXPORT_(?:FUNC|VAR)(?:_HASH)?\s*\(\s*(\w+)\s*\)")
RE_EXP_END = re.compile(r"PSP_EXPORT_END\b")


def harvest_exp(h, path, tag):
    lib = None
    for line in read_text(path).splitlines():
        line = line.split("#", 1)[0]
        m = RE_EXP_START.search(line)
        if m:
            lib = m.group(1)
            continue
        if RE_EXP_END.search(line):
            lib = None
            continue
        m = RE_EXP_NID.search(line)
        if m:
            if lib:
                # uofw exports an address expression once ("intInfo + 116"); keep it compact
                h.add(lib, int(m.group(2), 16), re.sub(r"\s+", "", m.group(1)), tag)
            continue
        m = RE_EXP_HASH.search(line)
        if m and lib:
            h.add(lib, nid_of(m.group(1)), m.group(1), tag)
        elif re.search(r"PSP_EXPORT_(FUNC|VAR)", line):
            h.warn("%s: unparsed export line: %s" % (tag, line.strip()))


RE_NID_DEFINE = re.compile(r"#\s*define\s+NID_(\w+)\s+" + _HEX)


def harvest_modulemgr_nids(h, path, tag):
    """syslib NIDs from uofw's modulemgr_nids.h; only hash-verified names."""
    for m in RE_NID_DEFINE.finditer(read_text(path)):
        base, nid = m.group(1).lower(), int(m.group(2), 16)
        for cand in (base, re.sub(r"_param$", "_parameter", base)):
            if nid_of(cand) == nid:
                h.add("syslib", nid, cand, tag)
                break


def harvest_uofw(h, root):
    for p in walk_files(root, (".exp",)):
        harvest_exp(h, p, "uofw:" + relpath(p, root))
    mm = os.path.join(root, "include", "modulemgr_nids.h")
    if os.path.isfile(mm):
        harvest_modulemgr_nids(h, mm, "uofw:" + relpath(mm, root))


# --------------------------------------------------------------------------
# PPSSPP HLE tables
# --------------------------------------------------------------------------

RE_TABLE = re.compile(r"\bHLEFunction\s+(\w+)\s*\[\s*\w*\s*\]\s*=\s*\{")
RE_ENTRY = re.compile(r"\{\s*" + _HEX + r"\s*,[^\"{}]*?,\s*\"([^\"]*)\"")
RE_REGISTER = re.compile(r'\bRegisterHLEModule\s*\(\s*"([^"]+)"\s*,[^;]*?,\s*&?\s*(\w+)\s*\)\s*;')
RE_MODLIST = re.compile(r'\{\s*"([^"]+)"\s*,\s*(?:ARRAY_SIZE|std::size)\s*\(\s*(\w+)\s*\)\s*,\s*(\w+)\s*\}')
RE_MODMETA = re.compile(r"\bHLEModuleMeta\s+\w+\s*\[\s*\]\s*=\s*\{")
RE_MODMETA_ENT = re.compile(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"')


def brace_block(text, start):
    """Return text[start:end] where text[start] == '{' and end follows its match."""
    depth, i, n = 0, start, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    return text[start:]


def harvest_ppsspp(h, root):
    core = os.path.join(root, "Core")
    tables = defaultdict(dict)      # table name -> {file: [(nid, name)]}
    regs = []                       # (lib, table, file)
    modnames = set()                # PRX module names that ppsspp also registers as "libraries"
    for p in walk_files(core, (".cpp", ".h")):
        text = read_text(p)
        if "HLEFunction" not in text and "RegisterHLEModule" not in text:
            continue
        text = strip_c_comments(text)
        rel = relpath(p, root)
        for m in RE_TABLE.finditer(text):
            body = brace_block(text, m.end() - 1)
            ents = []
            for e in RE_ENTRY.finditer(body):
                name = e.group(2).strip()
                if not name or name in ("nullptr", "NULL"):
                    continue
                ents.append((int(e.group(1), 16), name))
            tables[m.group(1)][rel] = ents
        for m in RE_REGISTER.finditer(text):
            regs.append((m.group(1), m.group(2), rel))
        for m in RE_MODLIST.finditer(text):
            if m.group(2) == m.group(3):
                regs.append((m.group(1), m.group(3), rel))
        for m in RE_MODMETA.finditer(text):
            for e in RE_MODMETA_ENT.finditer(brace_block(text, m.end() - 1)):
                if e.group(1) != e.group(2):
                    modnames.add(e.group(1))
    by_table = defaultdict(set)
    for lib, tname, _rel in regs:
        by_table[tname].add(lib)
    used = set()
    for lib, tname, rel in regs:
        # A module name registered with the same table as a real library is an alias.
        alias_of = sorted(by_table[tname] - modnames - {lib}) if lib in modnames else []
        if lib in SKIP_PPSSPP_LIBS or alias_of:
            if alias_of:
                h.note("ppsspp: skipped module-name registration %s (same table as %s)"
                       % (lib, ", ".join(alias_of)))
            used.update((tname, f) for f in tables.get(tname, {}))
            continue
        defs = tables.get(tname)
        if not defs:
            h.warn("ppsspp: %s registers %s with unknown table %s" % (rel, lib, tname))
            continue
        files = [rel] if rel in defs else sorted(defs)
        if len(files) > 1:
            h.warn("ppsspp: table %s for %s is defined in several files: %s" % (tname, lib, files))
        for f in files:
            used.add((tname, f))
            for nid, name in defs[f]:
                h.add(lib, nid, name, "ppsspp:" + f)
    for tname, defs in sorted(tables.items()):
        for f in sorted(defs):
            if (tname, f) not in used and defs[f]:
                h.warn("ppsspp: table %s in %s (%d entries) is never registered" % (tname, f, len(defs[f])))


# --------------------------------------------------------------------------
# ar archives of MIPS ELF stub objects (M33 SDK)
# --------------------------------------------------------------------------

def ar_members(data):
    if data[:8] != b"!<arch>\n":
        raise ValueError("not an ar archive")
    off, longnames = 8, b""
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        if hdr[58:60] != b"`\n":
            raise ValueError("bad ar member header at 0x%x" % off)
        name = hdr[:16].decode("latin-1").rstrip()
        size = int(hdr[48:58].decode("ascii").strip())
        body = data[off + 60:off + 60 + size]
        off += 60 + size + (size & 1)
        if name in ("/", "/SYM64/", "__.SYMDEF", "__.SYMDEF SORTED"):
            continue
        if name == "//":
            longnames = body
            continue
        if name.startswith("#1/"):                      # BSD long name
            ln = int(name[3:])
            name, body = body[:ln].rstrip(b"\0").decode("latin-1"), body[ln:]
        elif name.startswith("/") and name[1:].isdigit():  # GNU long name
            i = int(name[1:])
            end = longnames.find(b"\n", i)
            name = longnames[i:end if end >= 0 else None].decode("latin-1")
        yield name.rstrip("/"), body


def elf_stub_entries(d):
    """Yield (lib, nid, name, is_alias) from one relocatable stub object."""
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        raise ValueError("not a 32-bit LE ELF")
    e_type, e_machine = struct.unpack_from("<HH", d, 16)
    if e_type != 1 or e_machine != 8:
        raise ValueError("not a MIPS relocatable object")
    e_shoff, = struct.unpack_from("<I", d, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = [struct.unpack_from("<10I", d, e_shoff + i * e_shentsize) for i in range(e_shnum)]

    def cstr(off):
        return d[off:d.index(b"\0", off)].decode("latin-1")

    names = [cstr(secs[e_shstrndx][4] + s[0]) for s in secs]
    idx = {n: i for i, n in enumerate(names)}
    if ".sceStub.text" not in idx and ".rodata.sceNid" not in idx:
        return
    if ".sceStub.text" not in idx or ".rodata.sceNid" not in idx:
        raise ValueError("object has only one of .sceStub.text/.rodata.sceNid")
    text_i, nid_s = idx[".sceStub.text"], secs[idx[".rodata.sceNid"]]
    nids = struct.unpack_from("<%dI" % (nid_s[5] // 4), d, nid_s[4])
    if secs[text_i][5] != 8 * len(nids):
        raise ValueError(".sceStub.text size %d != 8 * %d NIDs" % (secs[text_i][5], len(nids)))

    syms = []                                   # (name, value, size, bind, shndx)
    for s in secs:
        if s[1] == 2:                           # SHT_SYMTAB
            strtab = secs[s[6]][4]
            for k in range(s[5] // 16):
                st_name, val, size, info, _o, shndx = struct.unpack_from("<IIIBBH", d, s[4] + 16 * k)
                syms.append((cstr(strtab + st_name), val, size, info >> 4, info & 15, shndx))
    stub_text_relocs = {}                       # offset -> symbol name
    for s in secs:
        if s[1] in (9, 4) and s[7] == text_i:   # SHT_REL / SHT_RELA on .sceStub.text
            ent = 8 if s[1] == 9 else 12
            for k in range(s[5] // ent):
                r_off, r_info = struct.unpack_from("<II", d, s[4] + ent * k)
                if (r_info >> 8) < len(syms):
                    stub_text_relocs[r_off] = syms[r_info >> 8][0]

    libs = {n[len("__stub_module_"):] for n, *_ in syms if n.startswith("__stub_module_")}
    if ".rodata.sceResident" in idx:
        rs = secs[idx[".rodata.sceResident"]]
        for part in d[rs[4] + 4:rs[4] + rs[5]].split(b"\0"):
            if part:
                libs.add(part.decode("latin-1"))

    funcs = [(n, v, sz) for n, v, sz, bind, typ, shndx in syms
             if shndx == text_i and bind in (1, 2) and typ in (0, 2) and n
             and not n.startswith("__stub_")]
    by_slot = defaultdict(list)
    for n, v, sz in funcs:
        if v % 8:
            raise ValueError("stub symbol %s at unaligned offset %d" % (n, v))
        by_slot[v // 8].append((n, sz))
    for slot, ents in sorted(by_slot.items()):
        if slot >= len(nids):
            raise ValueError("stub symbol beyond NID table")
        lib = None
        target = stub_text_relocs.get(slot * 8, "")
        if target.startswith("__stub_module_"):
            lib = target[len("__stub_module_"):]
        elif len(libs) == 1:
            lib = next(iter(libs))
        if lib is None:
            raise ValueError("cannot determine library for %s" % ents[0][0])
        ents.sort(key=lambda e: (e[1] == 0, e[0]))   # sized symbol = primary name
        for k, (n, _sz) in enumerate(ents):
            yield lib, nids[slot], n, k > 0


def harvest_ar(h, path, tag):
    with open(path, "rb") as f:
        data = f.read()
    names = []
    try:
        for mname, body in ar_members(data):
            try:
                for lib, nid, name, alias in elf_stub_entries(body):
                    h.add(lib, nid, name, tag, alias)
                    names.append(name)
            except (ValueError, struct.error, IndexError) as e:
                h.warn("%s(%s): %s" % (tag, mname, e))
    except ValueError as e:
        h.warn("%s: %s" % (tag, e))
    return names


RE_PROTO = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def harvest_m33(h, root):
    archive_names = set()
    for p in walk_files(root, (".a",)):
        archive_names.update(harvest_ar(h, p, "m33:" + relpath(p, root)))
    for p in walk_files(root, (".S", ".s")):
        harvest_stub_source(h, p, "m33:" + relpath(p, root))
    for p in walk_files(root, (".exp",)):
        harvest_exp(h, p, "m33:" + relpath(p, root))
    return archive_names, header_names(root)


def header_names(root):
    """Identifiers followed by '(' in every *.h under root (prototype cross-check)."""
    names = set()
    for p in walk_files(root, (".h",)):
        names.update(RE_PROTO.findall(strip_c_comments(read_text(p))))
    return names


# --------------------------------------------------------------------------
# Merge, write, report
# --------------------------------------------------------------------------

def family(src):
    return src.split(":", 1)[0]


def src_key(src):
    return (PRIORITY.get(family(src), 99), src)


def merge(recs):
    groups = defaultdict(dict)  # (lib, nid) -> name -> info
    for lib, nid, name, src, alias, order in recs:
        info = groups[(lib, nid)].setdefault(name, {"sources": set(), "alias": True, "order": order})
        info["sources"].add(src)
        info["alias"] = info["alias"] and alias
    rows, conflicts = [], []
    for (lib, nid), cands in groups.items():
        def rank(item):
            name, info = item
            return (nid_of(name) != nid, is_placeholder(name, nid),
                    min(PRIORITY.get(family(s), 99) for s in info["sources"]),
                    info["alias"], info["order"])
        ordered = sorted(cands.items(), key=rank)
        name, info = ordered[0]
        srcs = sorted(info["sources"], key=src_key)
        rows.append({"library": lib, "nid": nid, "name": name, "sources": srcs,
                     "hash_ok": int(nid_of(name) == nid),
                     "placeholder": is_placeholder(name, nid)})
        losers = ordered[1:]
        if losers:
            conflicts.append((lib, nid, name, [(n, sorted(i["sources"], key=src_key)[0]) for n, i in losers]))
    rows.sort(key=lambda r: (r["library"], r["nid"]))
    conflicts.sort()
    return rows, conflicts


def format_sources(srcs):
    if len(srcs) <= MAX_SOURCES:
        return ";".join(srcs)
    return ";".join(srcs[:MAX_SOURCES]) + ";+%d" % (len(srcs) - MAX_SOURCES)


def write_outputs(rows, csv_path, json_path):
    for p in (csv_path, json_path):
        d = os.path.dirname(os.path.abspath(p))
        os.makedirs(d, exist_ok=True)
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["library", "nid", "name", "source", "hash_ok"])
        for r in rows:
            w.writerow([r["library"], "0x%08X" % r["nid"], r["name"],
                        format_sources(r["sources"]), r["hash_ok"]])
    by_lib = {}
    for r in rows:
        by_lib.setdefault(r["library"], {})["0x%08X" % r["nid"]] = r["name"]
    with open(json_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(by_lib, f, indent=1, sort_keys=True)
        f.write("\n")


def report(h, rows, conflicts, m33_check):
    pr = print
    pr("== harvested candidates per source family")
    cand = Counter(family(r[3]) for r in h.recs)
    for fam in sorted(cand, key=lambda f: PRIORITY.get(f, 99)):
        pr("  %-7s %6d" % (fam, cand[fam]))

    pr("== nids.csv: %d rows, %d libraries" % (len(rows), len({r['library'] for r in rows})))
    contrib, sole, won_ok = Counter(), Counter(), Counter()
    for r in rows:
        fams = {family(s) for s in r["sources"]}
        for fam in fams:
            contrib[fam] += 1
            won_ok[fam] += r["hash_ok"]
        if len(fams) == 1:
            sole[next(iter(fams))] += 1
    pr("  %-7s %6s %6s %8s" % ("source", "rows", "only", "hash_ok"))
    for fam in sorted(contrib, key=lambda f: PRIORITY.get(f, 99)):
        pr("  %-7s %6d %6d %8d" % (fam, contrib[fam], sole[fam], won_ok[fam]))
    ok = sum(r["hash_ok"] for r in rows)
    ph = sum(r["placeholder"] for r in rows)
    pr("  hash_ok: %d/%d (%.1f%%); placeholder names kept (no better name): %d"
       % (ok, len(rows), 100.0 * ok / max(1, len(rows)), ph))

    per_lib = Counter(r["library"] for r in rows)
    lib_ok = Counter()
    for r in rows:
        lib_ok[r["library"]] += r["hash_ok"]
    pr("== top 20 libraries")
    for lib, n in per_lib.most_common(20):
        pr("  %-32s %5d  hash_ok %5.1f%%" % (lib, n, 100.0 * lib_ok[lib] / n))

    nonhash = sorted((lib for lib, n in per_lib.items() if n >= 5 and lib_ok[lib] / n < 0.5),
                     key=lambda l: (lib_ok[l] / per_lib[l], l))
    pr("== libraries with >=5 rows and <50%% hash-verified NIDs (randomised / non-hash): %d" % len(nonhash))
    for lib in nonhash:
        fams = Counter(family(s) for r in rows if r["library"] == lib for s in r["sources"])
        pr("  %-32s %4d rows %5.1f%% ok  [%s]" % (lib, per_lib[lib], 100.0 * lib_ok[lib] / per_lib[lib],
                                             ",".join("%s=%d" % kv for kv in sorted(fams.items()))))

    pr("== (library, nid) name conflicts resolved: %d" % len(conflicts))
    for lib, nid, name, losers in conflicts[:15]:
        pr("  %s 0x%08X %s  (dropped: %s)" % (lib, nid, name,
                                            ", ".join("%s[%s]" % (n, s) for n, s in losers)))
    if len(conflicts) > 15:
        pr("  ...")

    multi = defaultdict(set)
    for r in rows:
        if not r["placeholder"]:
            multi[(r["library"], r["name"])].add(r["nid"])
    multi_libs = Counter(lib for (lib, _n), s in multi.items() if len(s) > 1)
    pr("== (library, name) pairs with several NIDs (firmware-version NID changes): %d"
       % sum(multi_libs.values()))
    for lib, n in multi_libs.most_common(12):
        pr("  %-32s %4d" % (lib, n))

    folded = defaultdict(set)
    for lib in per_lib:
        folded[re.sub(r"[^a-z0-9]", "", lib.lower())].add(lib)
    variants = [sorted(v) for v in folded.values() if len(v) > 1]
    pr("== library spelling variants (case/punctuation-insensitive): %d" % len(variants))
    for v in sorted(variants):
        pr("  " + " | ".join("%s(%d)" % (l, per_lib[l]) for l in v))

    if m33_check is not None:
        names, m33_protos, sdk_protos = m33_check
        real = sorted(n for n in names if not is_placeholder(n))
        in_m33 = [n for n in real if n in m33_protos]
        in_sdk = [n for n in real if n not in m33_protos and n in sdk_protos]
        nowhere = [n for n in real if n not in m33_protos and n not in sdk_protos]
        pr("== M33 SDK/lib cross-check: %d stub names (%d placeholders); prototype in M33 "
           "SDK/include: %d, only in pspsdk headers: %d, in neither: %d"
           % (len(names), len(names) - len(real), len(in_m33), len(in_sdk), len(nowhere)))
        if nowhere:
            nowhere.sort(key=lambda n: (n.startswith("sce"), n))   # CFW-specific names first
            pr("  in neither: " + ", ".join(nowhere[:12])
               + (", ... +%d more" % (len(nowhere) - 12) if len(nowhere) > 12 else ""))
        cfw = sorted(n for n in m33_protos if re.match(r"(sctrl|vctrl|ku[A-Z]|pspUsbDevice)", n)
                     and n not in names)
        if cfw:
            pr("  CFW prototypes in SDK/include without a stub: " + ", ".join(cfw))

    for n in h.notes:
        pr("note: " + n)
    if h.warnings:
        pr("== warnings: %d" % len(h.warnings))
        for w in h.warnings:
            pr("  " + w)


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ref = os.path.join(os.path.dirname(here), "reference")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pspsdk", default=os.path.join(ref, "pspsdk"), help="pspsdk tree")
    ap.add_argument("--ppsspp", default=os.path.join(ref, "ppsspp"), help="PPSSPP tree")
    ap.add_argument("--uofw", default=os.path.join(ref, "uofw"), help="uOFW tree")
    ap.add_argument("--m33", default=os.path.join(ref, "401M33"), help="4.01 M33 tree (with SDK/)")
    ap.add_argument("-o", "--csv", default=os.path.join("data", "nids.csv"), help="output CSV")
    ap.add_argument("--json", default=os.path.join("data", "nids_by_library.json"), help="output JSON")
    args = ap.parse_args(argv)

    h = Harvest()
    m33_check = None
    for label, path, fn in (("uofw", args.uofw, harvest_uofw), ("pspsdk", args.pspsdk, harvest_pspsdk),
                            ("ppsspp", args.ppsspp, harvest_ppsspp), ("m33", args.m33, harvest_m33)):
        if not os.path.isdir(path):
            print("warning: %s tree not found: %s" % (label, path), file=sys.stderr)
            continue
        res = fn(h, path)
        if label == "m33":
            sdk_hdrs = os.path.join(args.pspsdk, "src")
            m33_check = res + (header_names(sdk_hdrs) if os.path.isdir(sdk_hdrs) else set(),)
    if not h.recs:
        print("error: nothing harvested", file=sys.stderr)
        return 1
    rows, conflicts = merge(h.recs)
    write_outputs(rows, args.csv, args.json)
    print("wrote %s and %s" % (args.csv, args.json))
    report(h, rows, conflicts, m33_check)
    return 0


if __name__ == "__main__":
    sys.exit(main())
