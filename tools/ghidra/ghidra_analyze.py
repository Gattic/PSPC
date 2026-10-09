#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 PSPC contributors
"""Run the Ghidra headless pipeline on PSP PRX/ELF modules.

For every input module this imports it into a Ghidra project under
private/ghidra/ with the ghidra-allegrex "PSP Executable (ELF)" loader and the
Allegrex:LE:32:default language, names imports/exports from data/nids.csv
(PspcApplyNids.java, before auto-analysis), runs auto-analysis and exports
private/fw/401/analysis/<stem>.json and private/fw/401/decomp/<stem>.c
(PspcExportAnalysis.java).

Usually started through tools/ghidra_analyze.cmd.

    python -I tools/ghidra/ghidra_analyze.py [options] <prx|dir> [<prx|dir> ...]

Kernel modules (attribute 0x1000) are loaded at image base 0x88000000, all
others at 0x08804000.  Files sharing a project folder and image base are
imported by one analyzeHeadless invocation.
"""

import argparse
import glob
import os
import struct
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEFAULT_GHIDRA = r"C:\Users\yoelr\Tools\ghidra_12.1.3_PUBLIC"
LANGUAGE = "Allegrex:LE:32:default"
LOADER = "PspElfLoader"
KERNEL_BASE = 0x88000000
USER_BASE = 0x08804000


def find_java_home(explicit):
    if explicit:
        return explicit
    env = os.environ.get("PSPC_JAVA_HOME")
    if env:
        return env
    cands = []
    for root in (r"C:\Program Files\Eclipse Adoptium", r"C:\Program Files\Java",
                 r"C:\Program Files\Microsoft", r"C:\Program Files\Zulu"):
        cands += glob.glob(os.path.join(root, "jdk-21*"))
    cands = [c for c in cands if os.path.exists(os.path.join(c, "bin", "java.exe"))]
    if cands:
        return sorted(cands)[-1]
    jh = os.environ.get("JAVA_HOME")
    return jh


def module_kind(path):
    """Return (attr, is_elf) by reading SceModuleInfo; attr None if unknown."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF":
        return None, False
    e_type = struct.unpack_from("<H", d, 16)[0]
    shoff, = struct.unpack_from("<I", d, 0x20)
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)
    phoff, = struct.unpack_from("<I", d, 0x1C)
    off = None
    if shoff and shnum and shoff + shnum * 40 <= len(d) and shstrndx < shnum:
        strtab = struct.unpack_from("<I", d, shoff + shstrndx * 40 + 16)[0]
        for i in range(shnum):
            nm, = struct.unpack_from("<I", d, shoff + i * 40)
            end = d.find(b"\0", strtab + nm)
            if d[strtab + nm:end] == b".rodata.sceModuleInfo":
                off = struct.unpack_from("<I", d, shoff + i * 40 + 16)[0]
    if off is None and e_type in (0xFFA0, 0xFFA1):
        off = struct.unpack_from("<I", d, phoff + 12)[0] & 0x7FFFFFFF
    if off is None or off + 2 > len(d):
        return None, True
    return struct.unpack_from("<H", d, off)[0], True


def collect(inputs):
    out = []
    for p in inputs:
        if os.path.isdir(p):
            for n in sorted(os.listdir(p)):
                q = os.path.join(p, n)
                if os.path.isfile(q) and n.lower().endswith((".prx", ".elf", ".bin")):
                    out.append(q)
        else:
            out.extend(sorted(glob.glob(p)) or [p])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("inputs", nargs="+", help="PRX/ELF files or directories")
    ap.add_argument("--ghidra", default=os.environ.get("GHIDRA_INSTALL_DIR", DEFAULT_GHIDRA))
    ap.add_argument("--java-home", help="JDK 21 home (default: newest jdk-21* found)")
    ap.add_argument("--project-dir", default=os.path.join(REPO, "private", "ghidra"))
    ap.add_argument("--project", default="pspc_fw401")
    ap.add_argument("--folder", help="project folder (default: name of each file's directory)")
    ap.add_argument("--out", default=os.path.join(REPO, "private", "fw", "401"),
                    help="output root: <out>/analysis/<stem>.json, <out>/decomp/<stem>.c")
    ap.add_argument("--nids", default=os.path.join(REPO, "data", "nids.csv"))
    ap.add_argument("--no-decomp", action="store_true", help="skip the decompiled C export")
    ap.add_argument("--timeout", type=int, default=1800, help="analysis timeout per file (s)")
    ap.add_argument("--max-cpu", type=int, default=0)
    ap.add_argument("--raw-base", type=lambda x: int(x, 0),
                    help="import non-ELF inputs as raw Allegrex code at this address "
                         "(e.g. 0x88FC0000 for systemctrl's reboot payload)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    files = collect(args.inputs)
    if not files:
        sys.exit("no input files")
    headless = os.path.join(args.ghidra, "support", "analyzeHeadless.bat")
    if not os.path.exists(headless):
        sys.exit("analyzeHeadless.bat not found under %s" % args.ghidra)
    java_home = find_java_home(args.java_home)
    if not java_home or not os.path.exists(os.path.join(java_home, "bin", "java.exe")):
        sys.exit("JDK 21 not found; install it or pass --java-home / set PSPC_JAVA_HOME")
    env = dict(os.environ)
    env["JAVA_HOME"] = java_home
    env["PATH"] = os.path.join(java_home, "bin") + os.pathsep + env.get("PATH", "")

    groups = {}
    for f in files:
        attr, is_elf = module_kind(f)
        folder = args.folder or os.path.basename(os.path.dirname(os.path.abspath(f))) or "root"
        if not is_elf:
            if args.raw_base is None:
                print("skip (not an ELF/PRX; use --raw-base to import raw code): %s" % f)
                continue
            groups.setdefault((folder, args.raw_base, True), []).append(os.path.abspath(f))
            continue
        base = KERNEL_BASE if attr is not None and attr & 0x1000 else USER_BASE
        groups.setdefault((folder, base, False), []).append(os.path.abspath(f))

    os.makedirs(args.project_dir, exist_ok=True)
    log_dir = os.path.join(args.project_dir, "logs")
    os.makedirs(log_dir, exist_ok=True)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    rc_all = 0
    for (folder, base, raw), paths in sorted(groups.items()):
        stamp = time.strftime("%Y%m%d-%H%M%S")
        cmd = [headless, args.project_dir, "%s/%s" % (args.project, folder)]
        for p in paths:
            cmd += ["-import", p]
        if raw:
            loader = ["-loader", "BinaryLoader", "-loader-baseAddr", "0x%08x" % base]
        else:
            loader = ["-loader", LOADER, "-loader-imagebase", "%08x" % base]
        cmd += ["-overwrite"] + loader + [
                "-processor", LANGUAGE,
                "-scriptPath", script_dir,
                "-preScript", "PspcApplyNids.java", os.path.abspath(args.nids),
                "-postScript", "PspcExportAnalysis.java", os.path.abspath(args.out)]
        if args.no_decomp:
            cmd.append("nodecomp")
        cmd += ["-analysisTimeoutPerFile", str(args.timeout),
                "-log", os.path.join(log_dir, "%s-%s-%08x.log" % (stamp, folder, base)),
                "-scriptlog", os.path.join(log_dir, "%s-%s-%08x.script.log" % (stamp, folder, base))]
        if args.max_cpu:
            cmd += ["-max-cpu", str(args.max_cpu)]
        print("== %s @ 0x%08X: %s" % (folder, base, ", ".join(os.path.basename(p) for p in paths)))
        if args.dry_run:
            print(subprocess.list2cmdline(cmd))
            continue
        rc = subprocess.call(cmd, env=env)
        if rc:
            print("analyzeHeadless exited with %d" % rc, file=sys.stderr)
            rc_all = rc
    sys.exit(rc_all)


if __name__ == "__main__":
    main()
