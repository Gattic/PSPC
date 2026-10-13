#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
r"""Builds the PSPC installer: a per-user Windows Installer package, PSPC-<version>-x64.msi.

Usage: make_installer.py [--build-dir build/RelWithDebInfo] [--python <Python 3.12 folder>]
                         [--version x.y.z] [--out-dir build/installer] [--icon <.ico|.exe>]
                         [--no-verify]

Requirements: Windows and 64-bit Python 3.12, run by its full path with -I, e.g.
    %LOCALAPPDATA%\Programs\Python\Python312\python.exe -I tools\make_installer.py
The package is written with the standard library's msilib (Python 3.13 removed it, so this script
refuses to run there); no WiX, Inno Setup or NSIS is needed. PSPC must already be built: the
binaries in <build dir>/bin are packaged as they are (they link the C runtime statically, so no
Visual C++ redistributable is needed).
Run tools/fetch_aot.py once for the portable release AOT toolchain, or supply --aot-toolchain.

What it does:
  1. Stages the install tree in <out dir>/stage: exactly the installed layout below.
  2. Builds a private, minimal Python runtime into stage/python from --python (default: the Python
     running this script), laid out like python.org's "embeddable package": python.exe,
     pythonw.exe, the core DLLs, the extension modules the game servers need (and the DLLs they
     import), the standard library compiled into python3XY.zip, and a python3XY._pth that puts
     ..\server and ..\tools on sys.path, so `python -m pspc_network ...` / `-m pspc_medius ...` / `-m pspc_gamespy ...`
     work from any folder. The servers' .py files get unchecked-hash .pyc files (used as they are,
     never revalidated) so Python never writes __pycache__ folders that an uninstall would leave
     behind.
     Then it runs the staged runtime (from another folder, with a bare PATH and a hostile
     PYTHONHOME/PYTHONPATH) to prove it is self-contained and that the servers start (--help).
  3. Writes <out dir>/PSPC-<version>-x64.msi: every file in one cabinet embedded in the package.

Installed layout (INSTALLDIR = %LOCALAPPDATA%\Programs\PSPC; msiexec /i <msi> INSTALLDIR=<dir>):
    pspc.exe, pspc-launcher.exe, pspc-iso.exe, pspc-decrypt.exe, pspc-recomp.exe, SDL2.dll
    data\        data/* of the repository
    server\      pspc_network\, pspc_medius\, pspc_gamespy\ (the servers' .py files), README.md
    tools\       pspc_install_fw.py, pspc_build.py
    aot\         matching semantics sources/headers, helper library and portable LLVM/MinGW
    python\      the private Python runtime (python.exe -m pspc_network ...)
    docs\        docs/*.md
    Games\       empty: where the user puts games
    plugins\     empty (seplugins\): PSPC's M33 plugins folder, so none above the install is used
    README.md, LICENSE.txt, THIRD_PARTY.md
PSPC writes its own files next to pspc.exe at run time (pspc.ini, ms0, flash0/flash1 once the user
installs firmware, cache, screenshots, servers\): that is why the install is per user, into a folder
the user can write, never Program Files. An uninstall removes only what the MSI installed; those
files (and the folders holding them) stay. Never packaged: anything of <build dir>/bin besides the
six binaries above - in particular flash0/flash1 (Sony's copyrighted firmware) and the
developer's ms0, cache, pspc.ini and screenshots. The stage is checked for them before packaging.

The package: per user (no elevation prompt, ALLUSERS unset), 64-bit, Start menu and desktop
shortcuts "PSPC" to pspc-launcher.exe, an Add/Remove Programs entry, and a short wizard in the
standard Windows Installer look: Welcome (Install/Cancel), progress, "PSPC is installed" with a
"Start PSPC now" check box; Repair/Remove when PSPC is already installed. HKCU\Software\PSPC holds
InstallDir and Version (a later package installs into the same folder). Every build gets a new
ProductCode; the fixed UPGRADE_CODE below makes a newer (or equal-version) package replace the
installed one (major upgrade: the old one is removed first) and an older one refuse to install.
Silent:  msiexec /i PSPC-<version>-x64.msi /qn        msiexec /x PSPC-<version>-x64.msi /qn

The version comes from project(PSPC VERSION x.y.z) in CMakeLists.txt (0.1.0 if it has none);
--version overrides it (e.g. to test upgrades).
"""
import os
import sys

if sys.version_info >= (3, 13):
    sys.exit('make_installer.py needs Python 3.12: it writes the MSI with msilib, which Python %d.%d no longer '
             'has (removed in 3.13). Run it with Python 3.12, e.g.\n'
             '  %%LOCALAPPDATA%%\\Programs\\Python\\Python312\\python.exe -I tools\\make_installer.py'
             % sys.version_info[:2])
if os.name != 'nt':
    sys.exit('make_installer.py runs on Windows only (msilib uses the Windows Installer API).')

import argparse
import ctypes
import importlib.util
import pathlib
import py_compile
import re
import shutil
import struct
import subprocess
import tempfile
import uuid
import warnings
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import pspc_build
import test_release_aot

with warnings.catch_warnings():
    warnings.simplefilter('ignore', DeprecationWarning)  # "msilib is deprecated" - we know
    import msilib
    from msilib import schema, sequence, text as msitext

ROOT = pathlib.Path(__file__).resolve().parent.parent

# The UpgradeCode tells Windows Installer that every PSPC package is the same product. All PSPC
# MSIs, forever, must carry this one: with a different UpgradeCode a new version would install
# side by side with the old one instead of replacing it. NEVER CHANGE IT.
UPGRADE_CODE = '{C04FBDED-1D91-442C-AC4C-B4598342A5D5}'

PRODUCT = 'PSPC'
MANUFACTURER = 'PSPC Project'
URL = 'https://github.com/Gattic/PSPC'
DEFAULT_VERSION = '0.1.0'

BINARIES = ('pspc.exe', 'pspc-launcher.exe', 'pspc-iso.exe', 'pspc-decrypt.exe', 'pspc-recomp.exe', 'SDL2.dll')
LAUNCHER = 'pspc-launcher.exe'
TOP_FILES = set(BINARIES) | {'README.md', 'LICENSE.txt', 'THIRD_PARTY.md'}
TOP_DIRS = {'data', 'server', 'tools', 'python', 'docs', 'Games', 'plugins', 'aot'}
SERVER_PACKAGES = ('pspc_network', 'pspc_medius', 'pspc_gamespy')
# What pspc and the developer keep in bin\ (and what PSPC writes at run time): never packaged.
FORBIDDEN_NAMES = {'flash0', 'flash1', 'ms0', 'cache', 'pspc.ini', 'screenshots', 'servers',
                   'pspc_unittests.exe', 'pspc_decrypt_tests.exe'}
FORBIDDEN_SUFFIXES = {'.pdb', '.ilk', '.exp', '.lib', '.sqlite', '.sqlite-wal', '.sqlite-shm', '.log'}

# The private Python runtime. Extension modules (DLLs\<name>.pyd) the servers and tools can need;
# the DLLs they import (sqlite3.dll, libssl-3.dll, libcrypto-3.dll, libffi-8.dll) come along.
PY_EXTENSIONS = ('_asyncio', '_bz2', '_ctypes', '_decimal', '_elementtree', '_hashlib', '_lzma', '_multiprocessing',
                 '_overlapped', '_queue', '_socket', '_sqlite3', '_ssl', '_uuid', 'pyexpat', 'select', 'unicodedata')
PY_OPTIONAL_EXTENSIONS = ('_wmi', '_zoneinfo')  # small; platform and zoneinfo use them when present
# Standard library parts left out of python3XY.zip (folder names match at any depth).
LIB_EXCLUDE_DIRS = {'test', 'tests', 'idle_test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'venv', 'lib2to3',
                    'pydoc_data', 'site-packages', '__pycache__', 'msilib'}
LIB_EXCLUDE_FILES = {'turtle.py'}

# Runs inside the staged runtime: everything must come from the install, nothing from the Python
# the runtime was built from (PYTHONHOME/PYTHONPATH point there on purpose; the ._pth must win).
RUNTIME_CHECK = r'''
import os, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(sys.executable))).lower() + os.sep
outside = [p for p in sys.path if not os.path.abspath(p).lower().startswith(root)]
if outside:
    sys.exit('sys.path reaches outside the install: %r' % outside)
import argparse, asyncio, base64, getpass, hashlib, hmac, html, http.server, json, logging, secrets
import socketserver, sqlite3, ssl, struct, subprocess, threading, urllib.request, zlib
hashlib.scrypt(b'x', salt=b'y', n=16, r=8, p=1)
for name in EXTENSIONS:
    __import__(name)
sqlite3.connect(':memory:').execute('create table t(x)')
ssl.create_default_context()
asyncio.run(asyncio.sleep(0))
foreign = sorted(m.__file__ for m in list(sys.modules.values())
                 if getattr(m, '__file__', None) and not os.path.abspath(m.__file__).lower().startswith(root))
if foreign:
    sys.exit('modules loaded from outside the install: %r' % foreign)
print('ok', sys.version.split()[0], ssl.OPENSSL_VERSION, 'SQLite', sqlite3.sqlite_version)
'''


class BuildError(Exception):
    pass


# ---------------------------------------------------------------------------------------------
# Version


def project_version():
    """x.y.z from project(PSPC VERSION x.y.z ...) in CMakeLists.txt, or DEFAULT_VERSION."""
    cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8', errors='replace')
    m = re.search(r'^\s*project\s*\(\s*PSPC\b([^)]*)\)', cmake, re.I | re.M)
    if m:
        v = re.search(r'\bVERSION\s+([0-9]+(?:\.[0-9]+){0,3})', m.group(1), re.I)
        if v:
            return v.group(1)
    print('warning: CMakeLists.txt has no project(PSPC VERSION x.y.z ...); using version %s' % DEFAULT_VERSION,
          file=sys.stderr)
    return DEFAULT_VERSION


def msi_version(v):
    """A ProductVersion Windows Installer accepts: major.minor.build (255.255.65535 at most)."""
    parts = v.split('.')
    if not 1 <= len(parts) <= 4 or not all(p.isdigit() for p in parts):
        raise BuildError('bad version %r: expected x.y.z' % v)
    nums = [int(p) for p in parts] + [0] * (3 - len(parts))
    if nums[0] > 255 or nums[1] > 255 or any(n > 65535 for n in nums[2:]):
        raise BuildError('bad version %r: Windows Installer allows 255.255.65535 at most' % v)
    if len(nums) == 4:
        print('warning: Windows Installer ignores the 4th version field when it decides about upgrades',
              file=sys.stderr)
    return '.'.join(str(n) for n in nums)


# ---------------------------------------------------------------------------------------------
# PE files: imports (for the runtime's DLLs) and the icon resource (for shortcuts and ARP)


class PE:
    def __init__(self, path):
        self.data = d = pathlib.Path(path).read_bytes()
        if d[:2] != b'MZ':
            raise ValueError('%s: not a PE file' % path)
        pe, = struct.unpack_from('<I', d, 0x3C)
        if d[pe:pe + 4] != b'PE\0\0':
            raise ValueError('%s: not a PE file' % path)
        self.machine, nsections = struct.unpack_from('<HH', d, pe + 4)
        opt_size, = struct.unpack_from('<H', d, pe + 20)
        opt = pe + 24
        magic, = struct.unpack_from('<H', d, opt)
        dirs = opt + (112 if magic == 0x20B else 96)
        ndirs, = struct.unpack_from('<I', d, dirs - 4)
        self.dirs = [struct.unpack_from('<II', d, dirs + 8 * i) for i in range(min(ndirs, 16))]
        self.sections = []
        for i in range(nsections):
            vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', d, opt + opt_size + 40 * i + 8)
            self.sections.append((va, max(vsize, rawsize), rawptr, rawsize))

    def directory(self, index):
        return self.dirs[index] if index < len(self.dirs) else (0, 0)

    def offset(self, rva):
        for va, size, rawptr, rawsize in self.sections:
            if va <= rva < va + size and rva - va < rawsize:
                return rawptr + rva - va
        raise ValueError('RVA %#x outside the file' % rva)

    def cstr(self, rva):
        o = self.offset(rva)
        return self.data[o:self.data.index(b'\0', o)].decode('ascii', 'replace')

    def imports(self):
        """Names of the DLLs this file imports (normal and delay-loaded)."""
        names = []
        for index, size, name_at in ((1, 20, 12), (13, 32, 4)):
            rva, _ = self.directory(index)
            if not rva:
                continue
            o = self.offset(rva)
            while any(self.data[o:o + size]):
                name_rva, = struct.unpack_from('<I', self.data, o + name_at)
                names.append(self.cstr(name_rva))
                o += size
        return names

    def resources(self, rtype):
        """{name: bytes} of the resources of one integer type (first language of each)."""
        rva, _ = self.directory(2)
        if not rva:
            return {}
        base = self.offset(rva)

        def entries(at):
            named, ids = struct.unpack_from('<HH', self.data, base + at + 12)
            for i in range(named + ids):
                yield struct.unpack_from('<II', self.data, base + at + 16 + 8 * i)

        out = {}
        for tid, tdir in entries(0):
            if tid != rtype or not tdir & 0x80000000:
                continue
            for name, ndir in entries(tdir & 0x7FFFFFFF):
                if not ndir & 0x80000000:
                    continue
                for _lang, leaf in entries(ndir & 0x7FFFFFFF):
                    if leaf & 0x80000000:
                        continue
                    data_rva, size = struct.unpack_from('<II', self.data, base + leaf)
                    o = self.offset(data_rva)
                    out.setdefault(name, self.data[o:o + size])
        return out


def extract_icon(exe):
    """The exe's first icon group as .ico file bytes, or None if it has none."""
    pe = PE(exe)
    groups = pe.resources(14)  # RT_GROUP_ICON
    if not groups:
        return None
    group = next(iter(groups.values()))  # resource order: what Explorer shows for the exe
    images = pe.resources(3)  # RT_ICON
    _, kind, count = struct.unpack_from('<HHH', group, 0)
    entries = []
    for i in range(count):
        w, h, colors, reserved, planes, bits, _size, nid = struct.unpack_from('<BBBBHHIH', group, 6 + 14 * i)
        if nid in images:
            entries.append((w, h, colors, reserved, planes, bits, images[nid]))
    if kind != 1 or not entries:
        return None
    head = struct.pack('<HHH', 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    table, blobs = b'', b''
    for w, h, colors, reserved, planes, bits, image in entries:
        table += struct.pack('<BBBBHHII', w, h, colors, reserved, planes, bits, len(image), offset + len(blobs))
        blobs += image
    return head + table + blobs


def file_version(path):
    """(version, language) of a versioned file for the File table, else (None, None)."""
    get = ctypes.windll.msi.MsiGetFileVersionW
    ver, lang = ctypes.create_unicode_buffer(64), ctypes.create_unicode_buffer(64)
    vlen, llen = ctypes.c_uint(64), ctypes.c_uint(64)
    if get(str(path), ver, ctypes.byref(vlen), lang, ctypes.byref(llen)) != 0:
        return None, None
    return ver.value or None, lang.value or None


# ---------------------------------------------------------------------------------------------
# Staging


def copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def need(path, what):
    if not path.is_file():
        raise BuildError('%s not found (%s)' % (path, what))
    return path


def stage_pspc(build_dir, stage):
    """Everything but the Python runtime."""
    bin_dir = build_dir / 'bin'
    for name in BINARIES:
        copy(need(bin_dir / name, 'build PSPC first'), stage / name)
    data = ROOT / 'data'
    for f in sorted(data.rglob('*')):
        if f.is_file() and '__pycache__' not in f.parts:
            copy(f, stage / 'data' / f.relative_to(data))
    for pkg in SERVER_PACKAGES:
        src = ROOT / 'server' / pkg
        need(src / '__main__.py', 'server package')
        for f in sorted(src.glob('*.py')):
            if not f.name.startswith('test'):
                copy(f, stage / 'server' / pkg / f.name)
    copy(need(ROOT / 'server' / 'README.md', 'server docs'), stage / 'server' / 'README.md')
    copy(need(ROOT / 'tools' / 'pspc_install_fw.py', 'firmware installer'), stage / 'tools' / 'pspc_install_fw.py')
    copy(need(ROOT / 'tools' / 'pspc_build.py', 'AOT builder'), stage / 'tools' / 'pspc_build.py')
    for f in sorted((ROOT / 'docs').glob('*.md')):
        copy(f, stage / 'docs' / f.name)
    copy(need(ROOT / 'README.md', 'docs'), stage / 'README.md')
    copy(need(ROOT / 'LICENSE', 'license'), stage / 'LICENSE.txt')
    copy(need(ROOT / 'THIRD_PARTY.md', 'docs'), stage / 'THIRD_PARTY.md')
    (stage / 'Games').mkdir(parents=True, exist_ok=True)
    # PSPC's own (empty) plugins folder: pspc and the launcher look for the nearest plugins\seplugins
    # from their folder up, and must not adopt an unrelated one above the install.
    (stage / 'plugins' / 'seplugins').mkdir(parents=True, exist_ok=True)


def stage_aot(build_dir, stage, toolchain):
    """Build the portable SDK with its own compiler; MSVC C++ libraries cannot be mixed with it.

    Only POD state and function pointers cross the x64 Windows ABI to the MSVC runtime.
    The out-of-line VFPU helpers and generated functions share the bundled compiler's ABI.
    """
    pspc_build.SEM_HEADER = build_dir / 'generated' / 'pspc_sem_version.h'
    try:
        version = pspc_build.sem_version()  # refuse a runtime built from different semantics
    except (OSError, RuntimeError) as e:
        raise BuildError(str(e))
    sdk = stage / 'aot'
    cc_name = 'x86_64-w64-mingw32-clang++.exe'
    need(toolchain / 'bin' / cc_name, 'run python -I tools/fetch_aot.py first')
    need(toolchain / 'LICENSE.TXT', 'LLVM license')
    need(toolchain / 'x86_64-w64-mingw32' / 'share' / 'mingw32' / 'COPYING', 'MinGW licenses')
    shutil.copytree(toolchain, sdk / 'toolchain')
    # Preserve every source hash input; the builder itself is installed under tools/.
    for p in (ROOT / 'src' / 'allegrex').iterdir():
        if p.is_file():
            copy(p, sdk / 'src' / 'allegrex' / p.name)
    copy(ROOT / 'src' / 'recomp' / 'codegen.cpp', sdk / 'src' / 'recomp' / 'codegen.cpp')
    copy(ROOT / 'src' / 'common' / 'common.h', sdk / 'src' / 'common' / 'common.h')
    copy(pspc_build.SEM_HEADER, sdk / 'generated' / 'pspc_sem_version.h')
    (sdk / 'lib').mkdir(parents=True)
    pspc_build.TOOLCHAIN = sdk / 'toolchain'
    env = pspc_build.bundled_env()
    cc = sdk / 'toolchain' / 'bin' / cc_name
    with tempfile.TemporaryDirectory(prefix='pspc-aot-sdk-') as tmp:
        objects = []
        for name in ('vfpu_exec', 'vfpu_math'):
            obj = pathlib.Path(tmp) / (name + '.o')
            cmd = [str(cc)] + pspc_build.MINGW_FLAGS + ['-I' + str(sdk / 'src'), '-c',
                   str(sdk / 'src' / 'allegrex' / (name + '.cpp')), '-o', str(obj)]
            r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if r.returncode:
                raise BuildError('AOT SDK compilation failed:\n' + r.stdout)
            objects.append(str(obj))
        r = subprocess.run([str(sdk / 'toolchain' / 'bin' / 'llvm-ar.exe'), 'rcs',
                            str(sdk / 'lib' / 'pspc_aot.a')] + objects,
                           env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            raise BuildError('AOT SDK archive failed:\n' + r.stdout)
    print('  AOT SDK: semantics %s, bundled LLVM/MinGW' % version)


def dll_dependencies(binaries, dll_dir):
    """The DLLs of dll_dir that binaries import, directly or through each other."""
    available = {p.name.lower(): p for p in dll_dir.glob('*.dll')}
    found, todo = {}, list(binaries)
    while todo:
        for name in PE(todo.pop()).imports():
            p = available.get(name.lower())
            if p and name.lower() not in found:
                found[name.lower()] = p
                todo.append(p)
    return sorted(found.values())


def stdlib_files(lib):
    for dirpath, dirnames, filenames in os.walk(lib):
        dirnames[:] = sorted(d for d in dirnames if d.lower() not in LIB_EXCLUDE_DIRS)
        for f in sorted(filenames):
            if f.endswith('.py') and f not in LIB_EXCLUDE_FILES:
                yield pathlib.Path(dirpath) / f


def build_stdlib_zip(lib, out, compile_pyc):
    """python3XY.zip: the standard library as sourceless .pyc (as source if compile_pyc is off,
    i.e. the runtime is another Python version than the one running this script)."""
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='pspc-stdlib-'))
    count = 0
    try:
        with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for src in stdlib_files(lib):
                rel = src.relative_to(lib).as_posix()
                name, data = rel, None
                if compile_pyc:
                    cfile = tmp / 'module.pyc'
                    try:
                        py_compile.compile(str(src), cfile=str(cfile), dfile=rel, doraise=True, optimize=0,
                                           invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
                        name, data = rel[:-3] + '.pyc', cfile.read_bytes()
                    except py_compile.PyCompileError:
                        pass  # keep the source
                info = zipfile.ZipInfo(name, date_time=(2000, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, data if data is not None else src.read_bytes())
                count += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if count < 100:
        raise BuildError('%s: only %d standard library modules - not a Python installation?' % (lib, count))
    return count


def stage_python(pyhome, dest):
    """The private runtime (see the top of this file). Returns its tag, e.g. 'python312'."""
    dlls = [p for p in pyhome.glob('python3*.dll') if re.fullmatch(r'python3\d+\.dll', p.name, re.I)]
    if len(dlls) != 1 or not (pyhome / 'Lib' / 'os.py').is_file() or not (pyhome / 'DLLs').is_dir():
        raise BuildError('%s is not a python.org Python installation (python3XY.dll, Lib, DLLs)' % pyhome)
    tag = dlls[0].stem.lower()
    if PE(need(pyhome / 'python.exe', 'Python')).machine != 0x8664:
        raise BuildError('%s is not a 64-bit Python' % pyhome)
    for name in ('python.exe', 'pythonw.exe', 'python3.dll', dlls[0].name, 'vcruntime140.dll', 'vcruntime140_1.dll',
                 'LICENSE.txt'):
        copy(need(pyhome / name, 'Python runtime file'), dest / name)
    dll_dir = pyhome / 'DLLs'
    pyds = []
    for name in PY_EXTENSIONS + PY_OPTIONAL_EXTENSIONS:
        p = dll_dir / (name + '.pyd')
        if p.is_file():
            pyds.append(p)
        elif name in PY_EXTENSIONS:
            raise BuildError('%s not found: the runtime needs it' % p)
    for p in pyds + dll_dependencies(pyds, dll_dir):
        copy(p, dest / p.name)
    same = tag == 'python%d%d' % sys.version_info[:2]
    if not same:
        print('warning: %s is not this Python (%d.%d): the standard library goes in as source' %
              (pyhome, *sys.version_info[:2]), file=sys.stderr)
    modules = build_stdlib_zip(pyhome / 'Lib', dest / (tag + '.zip'), same)
    # One entry per line, relative to python\: the stdlib, the extension modules, the servers, tools.
    (dest / (tag + '._pth')).write_text('%s.zip\n.\n..\\server\n..\\tools\n' % tag, encoding='ascii')
    return tag, same, modules


def precompile(stage):
    """Checked-hash .pyc for the servers and tools: Python validates them against the source (so
    an edited .py still works) and never writes __pycache__ itself."""
    count = 0
    for folder in [stage / 'server' / pkg for pkg in SERVER_PACKAGES] + [stage / 'tools']:
        for src in sorted(folder.glob('*.py')):
            py_compile.compile(str(src), cfile=importlib.util.cache_from_source(str(src)),
                               dfile=src.relative_to(stage).as_posix(), doraise=True,
                               invalidation_mode=py_compile.PycInvalidationMode.CHECKED_HASH)
            count += 1
    return count


def check_stage(stage):
    files = {p.name for p in stage.iterdir() if p.is_file()}
    dirs = {p.name for p in stage.iterdir() if p.is_dir()}
    if files != TOP_FILES or dirs != TOP_DIRS:
        raise BuildError('unexpected stage contents: files %s, folders %s' % (sorted(files ^ TOP_FILES),
                                                                             sorted(dirs ^ TOP_DIRS)))
    for p in stage.rglob('*'):
        sdk_lib = p.suffix.lower() == '.lib' and p.is_relative_to(stage / 'aot' / 'toolchain')
        if p.name.lower() in FORBIDDEN_NAMES or (p.suffix.lower() in FORBIDDEN_SUFFIXES and not sdk_lib):
            raise BuildError('%s must never be packaged' % p)


def stage_listing(stage):
    return {p.relative_to(stage).as_posix(): p.stat().st_size for p in stage.rglob('*') if p.is_file()}


def verify_runtime(stage, pyhome):
    """Runs the staged runtime the way the launcher will; raises BuildError if anything fails."""
    py = stage / 'python' / 'python.exe'
    sysroot = os.environ.get('SystemRoot', r'C:\Windows')
    env = {k: v for k, v in os.environ.items()
           if k.upper() in ('SYSTEMROOT', 'WINDIR', 'SYSTEMDRIVE', 'TEMP', 'TMP', 'USERPROFILE', 'USERNAME',
                            'LOCALAPPDATA', 'APPDATA', 'HOMEDRIVE', 'HOMEPATH', 'COMSPEC', 'PATHEXT',
                            'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE')}
    env['PATH'] = os.pathsep.join([os.path.join(sysroot, 'System32'), sysroot])
    env['PYTHONHOME'] = str(pyhome)  # must be ignored: the ._pth file makes the runtime isolated
    env['PYTHONPATH'] = str(pyhome / 'Lib')
    before = stage_listing(stage)
    work = tempfile.mkdtemp(prefix='pspc-runtime-check-')
    try:
        def run(args, what):
            try:
                r = subprocess.run([str(py)] + args, cwd=work, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=180, encoding='utf-8', errors='replace')
            except (OSError, subprocess.TimeoutExpired) as e:
                raise BuildError('%s: %s' % (what, e))
            if r.returncode != 0:
                raise BuildError('%s failed (exit code %d):\n%s' % (what, r.returncode, r.stdout.strip()))
            return r.stdout

        script = 'EXTENSIONS = %r\n%s' % (PY_EXTENSIONS, RUNTIME_CHECK)
        print('  ' + run(['-c', script], 'runtime check').strip())
        for pkg in SERVER_PACKAGES:
            run(['-m', pkg, '--help'], 'python -m %s --help' % pkg)
            print('  python -m %s --help: ok' % pkg)
        run([str(stage / 'tools' / 'pspc_install_fw.py'), '--help'], 'python tools\\pspc_install_fw.py --help')
        print('  python tools\\pspc_install_fw.py --help: ok')
        test_release_aot.verify(stage, pathlib.Path(work), env)
        print('  AOT: compiled, loaded and executed with only the installed tools')
    finally:
        shutil.rmtree(work, ignore_errors=True)
    after = stage_listing(stage)
    if after != before:
        changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
        raise BuildError('running the runtime changed the stage (it would write into the install): %s' % changed)


# ---------------------------------------------------------------------------------------------
# The MSI


SHORT_NAME = re.compile(r"^[A-Z0-9_\-!#$%&'(){}@^`~]{1,8}(\.[A-Z0-9_\-!#$%&'(){}@^`~]{1,3})?$")


def filename_column(name, used):
    """The Filename column value: the name itself if it is a valid 8.3 name, else 'SHORT~1.EXT|name'
    (short names unique within the folder)."""
    if SHORT_NAME.match(name.upper()) and name.upper() not in used:
        used.add(name.upper())
        return name
    base, ext = os.path.splitext(name)
    base = re.sub(r'[^A-Z0-9_]', '', base.upper()) or 'F'
    ext = re.sub(r'[^A-Z0-9_]', '', ext.upper())[:3]
    for i in range(1, 100000):
        suffix = '~%d' % i
        short = base[:8 - len(suffix)] + suffix + ('.' + ext if ext else '')
        if short not in used:
            used.add(short)
            return '%s|%s' % (short, name)
    raise BuildError('no short name for %s' % name)


def identifier(prefix, text, used):
    """A unique MSI identifier (letters, digits, _ and ., at most 72 characters)."""
    base = (prefix + re.sub(r'[^A-Za-z0-9_.]', '_', text))[:68]
    ident, n = base, 1
    while ident.lower() in used:
        ident = '%s_%d' % (base, n)
        n += 1
    used.add(ident.lower())
    return ident


def component_guid(component):
    # Stable per component, so repeated builds don't leave orphaned component registrations.
    return '{%s}' % str(uuid.uuid5(uuid.UUID(UPGRADE_CODE), component)).upper()


def white_bitmap(path):
    """A small white BMP: stretched behind the dialogs' text (the classic white wizard page)."""
    w = h = 8
    row = b'\xff' * (w * 3)
    pixels = row * h
    path.write_bytes(struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54) +
                     struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(pixels), 2835, 2835, 0, 0) + pixels)


def add_sequences(db, extra_ui, extra_exec):
    """msilib's standard sequences with PSPC's changes: FindRelatedProducts before LaunchConditions
    (so the downgrade check sees it), RemoveExistingProducts right after InstallValidate (the old
    version is removed before the new one is installed), no CCP searches."""
    def adjust(rows, moves, add):
        out = [(a, c, moves.get(a, s)) for a, c, s in rows if a not in ('CCPSearch', 'RMCCPSearch')]
        return out + add

    msilib.add_data(db, 'InstallUISequence',
                    adjust(sequence.InstallUISequence, {'FindRelatedProducts': 25}, extra_ui))
    msilib.add_data(db, 'InstallExecuteSequence',
                    adjust(sequence.InstallExecuteSequence,
                           {'FindRelatedProducts': 25, 'RemoveExistingProducts': 1401}, extra_exec))
    msilib.add_data(db, 'AdminUISequence', adjust(sequence.AdminUISequence, {}, [('ProgressDlg', None, 1280)]))
    msilib.add_data(db, 'AdminExecuteSequence', sequence.AdminExecuteSequence)
    msilib.add_data(db, 'AdvtExecuteSequence', sequence.AdvtExecuteSequence)


def add_ui(db, white_bmp):
    """The wizard: standard Windows Installer look (system font, white pages, buttons at the bottom)."""
    Dialog = msilib.Dialog
    W, H = 370, 270
    TITLE = '[ProductName] Setup'
    MODAL, MODELESS = 3, 1                # visible | modal; visible
    TEXT = 0x30003                        # visible | enabled | transparent | no & mnemonics
    HIDDEN = 0x30002                      # the same, shown by a ControlCondition
    PLAIN = 0x20003                       # visible | enabled | no & mnemonics (on the grey area)
    BUTTON = 3                            # visible | enabled
    INSTALL = 'NOT Installed AND ACTION <> "ADMIN"'
    REPAIR = 'Installed AND REMOVE <> "ALL" AND ACTION <> "ADMIN"'
    REMOVE = 'Installed AND REMOVE = "ALL" AND ACTION <> "ADMIN"'
    ADMIN = 'ACTION = "ADMIN"'

    msilib.add_data(db, 'Binary', [('WhiteBmp', msilib.Binary(str(white_bmp)))])
    msilib.add_data(db, 'TextStyle', [
        ('DlgFont8', 'Tahoma', 8, None, 0),
        ('BoldFont', 'Tahoma', 9, None, 1),
        ('TitleFont', 'Tahoma', 12, None, 1),
    ])
    msilib.add_data(db, 'CheckBox', [('LAUNCHPSPC', '1')])

    def page(name, first, default, cancel, attr=MODAL, background='page'):
        d = Dialog(db, name, 50, 50, W, H, attr, TITLE, first, default, cancel)
        if background == 'page':
            d.bitmap('Background', 0, 0, W, 234, 'WhiteBmp')
        else:  # banner
            d.bitmap('Banner', 0, 0, W, 44, 'WhiteBmp')
            d.line('BannerLine', 0, 44, W, 0)
        d.line('BottomLine', 0, 234, W, 0)
        return d

    def variant(d, name, x, y, w, h, text, condition):
        d.control(name, 'Text', x, y, w, h, HIDDEN, None, text, None, None).condition('Show', condition)

    # Welcome (first install or upgrade)
    d = page('WelcomeDlg', 'Install', 'Install', 'Cancel')
    d.text('Title', 20, 20, 330, 30, TEXT, r'{\TitleFont}Welcome to PSPC [ProductVersion]')
    d.text('Body', 20, 60, 330, 60, TEXT,
           'PSPC plays PSP games on Windows. It installs for your user account in [INSTALLDIR] '
           '(no administrator rights needed). PSPC is free software under the GNU GPL.')
    variant(d, 'Upgrade', 20, 125, 330, 40,
            'The PSPC that is already installed will be replaced by this version. Your games, saves, settings '
            'and firmware are kept.', 'PSPC_UPGRADE')
    d.text('Hint', 20, 205, 330, 15, TEXT, 'Click Install to continue, or Cancel to exit.')
    d.pushbutton('Install', 236, 243, 56, 17, BUTTON, '&Install', 'Cancel').event('EndDialog', 'Return')
    d.pushbutton('Cancel', 304, 243, 56, 17, BUTTON, 'Cancel', 'Install').event('SpawnDialog', 'CancelDlg')

    # Already installed: Repair or Remove. (Banner page: buttons on a white bitmap would be hidden
    # under it - Windows Installer creates the tab-order controls first, so they end up below.)
    d = page('MaintenanceDlg', 'Repair', 'Repair', 'Cancel', MODAL, 'banner')
    d.text('Title', 15, 8, 340, 15, TEXT, r'{\BoldFont}PSPC [ProductVersion] is installed')
    d.text('Description', 25, 25, 330, 15, TEXT, 'What do you want to do?')
    c = d.pushbutton('Repair', 25, 65, 70, 17, BUTTON, '&Repair', 'Remove')
    c.event('ReinstallMode', 'amus', '1', 1)  # every file, the registry values, the shortcuts
    c.event('Reinstall', 'ALL', '1', 2)  # (setting [REINSTALL] here would not reach the server)
    c.event('EndDialog', 'Return', '1', 3)
    d.text('RepairText', 105, 68, 245, 30, PLAIN, 'Puts back missing or damaged program files and shortcuts.')
    c = d.pushbutton('Remove', 25, 110, 70, 17, BUTTON, 'Re&move', 'Cancel')
    c.event('[REMOVE]', 'ALL', '1', 1)
    c.event('EndDialog', 'Return', '1', 2)
    d.text('RemoveText', 105, 113, 245, 50, PLAIN,
           'Uninstalls PSPC. What PSPC created in [INSTALLDIR] stays there: your games, saves, settings, '
           'firmware and server data.')
    d.pushbutton('Cancel', 304, 243, 56, 17, BUTTON, 'Cancel', 'Repair').event('SpawnDialog', 'CancelDlg')

    # Progress (modeless: stays up while the installation runs)
    d = page('ProgressDlg', 'Cancel', 'Cancel', 'Cancel', MODELESS, 'banner')
    variant(d, 'TitleInstall', 15, 8, 340, 15, r'{\BoldFont}Installing PSPC', INSTALL)
    variant(d, 'TitleRepair', 15, 8, 340, 15, r'{\BoldFont}Repairing PSPC', REPAIR)
    variant(d, 'TitleRemove', 15, 8, 340, 15, r'{\BoldFont}Removing PSPC', REMOVE)
    variant(d, 'TitleAdmin', 15, 8, 340, 15, r'{\BoldFont}Creating an administrative image of PSPC', ADMIN)
    d.text('Description', 25, 25, 330, 15, TEXT, 'Please wait, this takes a moment.')
    d.text('ActionText', 25, 70, 320, 15, PLAIN, '').mapping('ActionText', 'Text')
    d.control('ProgressBar', 'ProgressBar', 25, 90, 320, 12, 0x10001, None, 'Progress done', None,
              None).mapping('SetProgress', 'Progress')
    d.pushbutton('Cancel', 304, 243, 56, 17, BUTTON, 'Cancel', None).event('SpawnDialog', 'CancelDlg')

    # Done
    d = page('ExitDialog', 'Finish', 'Finish', 'Finish')
    variant(d, 'TitleInstall', 20, 20, 330, 30, r'{\TitleFont}PSPC is installed', INSTALL)
    variant(d, 'TitleRepair', 20, 20, 330, 30, r'{\TitleFont}PSPC is repaired', REPAIR)
    variant(d, 'TitleRemove', 20, 20, 330, 30, r'{\TitleFont}PSPC is removed', REMOVE)
    variant(d, 'TitleAdmin', 20, 20, 330, 30, r'{\TitleFont}Administrative image created', ADMIN)
    variant(d, 'BodyInstall', 20, 60, 330, 80,
            'Start PSPC with its shortcut on the desktop or in the Start menu. Put your games (ISO or CSO '
            'images, or EBOOT.PBP folders) in the Games folder: [INSTALLDIR]Games', INSTALL)
    variant(d, 'BodyRepair', 20, 60, 330, 80, 'The program files and shortcuts of PSPC are back in place.', REPAIR)
    variant(d, 'BodyRemove', 20, 60, 330, 80,
            'PSPC was uninstalled. What PSPC created in [INSTALLDIR] (games, saves, settings, firmware, server '
            'data) is still there: delete that folder if you no longer need it.', REMOVE)
    variant(d, 'BodyAdmin', 20, 60, 330, 80, 'The files of PSPC were extracted.', ADMIN)
    d.text('Hint', 20, 205, 330, 15, TEXT, 'Click Finish to close Setup.')
    c = d.checkbox('Launch', 20, 245, 200, 12, 2, 'LAUNCHPSPC', '&Start PSPC now', 'Finish')
    c.condition('Show', INSTALL)
    c = d.pushbutton('Finish', 304, 243, 56, 17, BUTTON, '&Finish', 'Launch')
    c.event('DoAction', 'LaunchPSPC', 'LAUNCHPSPC = "1" AND ' + INSTALL, 1)
    c.event('EndDialog', 'Return', '1', 2)

    # Cancelled, failed
    for name, title, body in (
            ('UserExit', 'Setup was cancelled',
             'PSPC Setup was cancelled before it finished, so nothing was changed. Run it again when you are '
             'ready.'),
            ('FatalError', 'Setup failed',
             'PSPC Setup stopped because of an error, so nothing was changed. If it fails again, run it with a '
             'log (msiexec /i <package>.msi /l*v setup.log) and report the problem at ' + URL + '/issues.')):
        d = page(name, 'Finish', 'Finish', 'Finish')
        d.text('Title', 20, 20, 330, 30, TEXT, r'{\TitleFont}' + title)
        d.text('Body', 20, 60, 330, 80, TEXT, body)
        d.pushbutton('Finish', 304, 243, 56, 17, BUTTON, '&Finish', None).event('EndDialog', 'Exit')

    # "Are you sure?"
    d = Dialog(db, 'CancelDlg', 50, 50, 260, 85, MODAL, TITLE, 'No', 'No', 'No')
    d.text('Text', 15, 15, 230, 30, PLAIN, 'Are you sure you want to cancel PSPC Setup?')
    d.pushbutton('Yes', 72, 57, 56, 17, BUTTON, '&Yes', 'No').event('EndDialog', 'Exit')
    d.pushbutton('No', 132, 57, 56, 17, BUTTON, '&No', 'Yes').event('EndDialog', 'Return')

    # Errors and warnings during the installation (the ErrorDialog property names it). The
    # installer shows the buttons the message needs: A/C/I/N/O/R/Y.
    d = Dialog(db, 'ErrorDlg', 50, 10, 330, 101, 0x10007, TITLE, 'ErrorText', None, None)  # error|minimize|modal|vis.
    d.text('ErrorText', 50, 9, 280, 48, PLAIN, '')
    for name, x, label, event in (('A', 0, 'Abort', 'ErrorAbort'), ('C', 42, 'Cancel', 'ErrorCancel'),
                                  ('I', 81, 'Ignore', 'ErrorIgnore'), ('N', 120, 'No', 'ErrorNo'),
                                  ('O', 159, 'OK', 'ErrorOk'), ('R', 198, 'Retry', 'ErrorRetry'),
                                  ('Y', 240, 'Yes', 'ErrorYes')):
        d.pushbutton(name, x, 72, 81, 21, BUTTON, label, None).event('EndDialog', event)

    # Programs (pspc, the launcher, a server) hold files that must be replaced or removed.
    d = page('FilesInUse', 'Retry', 'Retry', 'Retry', 19, 'banner')  # keep modeless | modal | visible
    d.text('Title', 15, 8, 340, 15, TEXT, r'{\BoldFont}Files in use')
    d.text('Description', 25, 25, 330, 15, TEXT, 'Some files that need to be changed are in use.')
    d.text('Text', 20, 55, 330, 30, PLAIN,
           'Close these programs (PSPC, its game servers), then click Retry. Ignore continues anyway; Windows '
           'then finishes the job when it restarts.')
    d.control('List', 'ListBox', 20, 87, 330, 140, 7, 'FileInUseProcess', None, None, None)
    d.pushbutton('Exit', 180, 243, 56, 17, BUTTON, 'E&xit', 'Ignore').event('EndDialog', 'Exit')
    d.pushbutton('Ignore', 236, 243, 56, 17, BUTTON, '&Ignore', 'Retry').event('EndDialog', 'Ignore')
    d.pushbutton('Retry', 304, 243, 56, 17, BUTTON, '&Retry', 'Exit').event('EndDialog', 'Retry')


def query(db, sql):
    view = db.OpenView(sql)
    view.Execute(None)
    rows = []
    while True:
        try:
            r = view.Fetch()
        except msilib.MSIError:
            break
        if r is None:
            break
        rows.append(tuple(r.GetString(i) or None for i in range(1, r.GetFieldCount() + 1)))
    view.Close()
    return rows


def check_ui(db):
    """Reads the UI tables back: every reference between dialogs, controls, events and actions resolves."""
    dialogs = {r[0]: r for r in query(db, 'SELECT `Dialog`, `Control_First`, `Control_Default`, `Control_Cancel` '
                                          'FROM `Dialog`')}
    controls = {(r[0], r[1]): r for r in query(db, 'SELECT `Dialog_`, `Control`, `Control_Next` FROM `Control`')}
    actions = {r[0] for r in query(db, 'SELECT `Action` FROM `CustomAction`')}
    problems = []
    for name, first, default, cancel in dialogs.values():
        for c in (first, default, cancel):
            if c and (name, c) not in controls:
                problems.append('%s: no control %s' % (name, c))
    for (dlg, ctl, nxt) in controls.values():
        if dlg not in dialogs:
            problems.append('control %s on missing dialog %s' % (ctl, dlg))
        if nxt and (dlg, nxt) not in controls:
            problems.append('%s.%s: Control_Next %s missing' % (dlg, ctl, nxt))
    for dlg, ctl, event, arg in query(db, 'SELECT `Dialog_`, `Control_`, `Event`, `Argument` FROM `ControlEvent`'):
        if (dlg, ctl) not in controls:
            problems.append('event %s on missing control %s.%s' % (event, dlg, ctl))
        if event in ('NewDialog', 'SpawnDialog', 'SpawnWaitDialog') and arg not in dialogs:
            problems.append('%s.%s: %s %s - no such dialog' % (dlg, ctl, event, arg))
        if event == 'DoAction' and arg not in actions:
            problems.append('%s.%s: DoAction %s - no such custom action' % (dlg, ctl, arg))
    for dlg, ctl, _action, _cond in query(db, 'SELECT * FROM `ControlCondition`'):
        if (dlg, ctl) not in controls:
            problems.append('condition on missing control %s.%s' % (dlg, ctl))
    for table in ('InstallUISequence', 'AdminUISequence'):
        for action, in query(db, 'SELECT `Action` FROM `%s`' % table):
            if action.endswith(('Dlg', 'Dialog')) and action not in dialogs:
                problems.append('%s: dialog %s missing' % (table, action))
    for prop in ('ErrorDialog',):
        value = query(db, "SELECT `Value` FROM `Property` WHERE `Property` = '%s'" % prop)
        if not value or value[0][0] not in dialogs:
            problems.append('%s names no dialog' % prop)
    for name in ('ExitDialog', 'UserExit', 'FatalError', 'FilesInUse', 'ErrorDlg', 'CancelDlg'):
        if name not in dialogs:
            problems.append('dialog %s missing' % name)
    if problems:
        raise BuildError('UI tables are inconsistent:\n  ' + '\n  '.join(problems))
    return len(dialogs), len(controls)


def build_msi(stage, out, version, icon, temp):
    product_code = msilib.gen_uuid()
    tmp_msi = out.with_name(out.stem + '.tmp.msi')
    db = msilib.init_database(str(tmp_msi), schema, PRODUCT, product_code, version, MANUFACTURER)
    si = db.GetSummaryInformation(20)
    si.SetProperty(msilib.PID_TITLE, 'Installation Database')
    si.SetProperty(msilib.PID_SUBJECT, '%s %s' % (PRODUCT, version))
    si.SetProperty(msilib.PID_AUTHOR, MANUFACTURER)
    si.SetProperty(msilib.PID_KEYWORDS, 'Installer, PSPC, PSP')
    si.SetProperty(msilib.PID_COMMENTS, 'PSPC plays PSP games on Windows. ' + URL)
    si.SetProperty(msilib.PID_TEMPLATE, 'x64;1033')  # 64-bit package
    si.SetProperty(msilib.PID_PAGECOUNT, 200)  # Windows Installer 2.0 schema (the minimum for x64)
    si.SetProperty(msilib.PID_WORDCOUNT, 2 | 8)  # compressed (embedded cabinet), no elevation needed
    si.SetProperty(msilib.PID_APPNAME, 'PSPC tools/make_installer.py (Python msilib)')
    si.Persist()
    del si  # its handle keeps the file open

    # Folders: TARGETDIR -> LocalAppDataFolder -> Programs -> INSTALLDIR (= PSPC) -> the stage's folders
    dirs = [d for d in sorted(stage.rglob('*')) if d.is_dir()]
    used_ids, dir_ids, components = set(), {stage: 'INSTALLDIR'}, {}
    used_ids.update(('installdir', 'targetdir', 'localappdatafolder', 'programsdir', 'programmenufolder',
                     'desktopfolder'))
    rows = [('TARGETDIR', None, 'SourceDir'), ('LocalAppDataFolder', 'TARGETDIR', '.'),
            ('ProgramsDir', 'LocalAppDataFolder', 'Programs'), ('INSTALLDIR', 'ProgramsDir', 'PSPC'),
            ('ProgramMenuFolder', 'TARGETDIR', '.'), ('DesktopFolder', 'TARGETDIR', '.')]
    short_names = {stage: set()}
    for d in dirs:
        dir_ids[d] = identifier('D_', d.relative_to(stage).as_posix(), used_ids)
        short_names[d] = set()
        rows.append((dir_ids[d], dir_ids[d.parent], filename_column(d.name, short_names[d.parent])))
    msilib.add_data(db, 'Directory', rows)

    # One component per folder; per-user components get an HKCU value as key path.
    comp_rows, reg_rows, remove_rows, create_rows = [], [], [], []
    for d in [stage] + dirs:
        did = dir_ids[d]
        comp = identifier('C_', did[2:] if did.startswith('D_') else did, used_ids)
        components[d] = comp
        reg = identifier('R_', comp, used_ids)
        comp_rows.append((comp, component_guid(comp), did, 4 | 256, None, reg))  # registry key path | 64-bit
        reg_rows.append((reg, 1, r'Software\PSPC\Components', comp, '#1', comp))
        remove_rows.append((identifier('RF_', did, used_ids), comp, None, did, 2))  # remove when empty
        if not any(d.iterdir()):
            create_rows.append((did, comp))  # e.g. Games: an empty folder
    remove_rows.append(('RF_ProgramsDir', components[stage], None, 'ProgramsDir', 2))
    reg_rows += [('R_InstallDir', 1, r'Software\PSPC', 'InstallDir', '[INSTALLDIR]', components[stage]),
                 ('R_Version', 1, r'Software\PSPC', 'Version', '[ProductVersion]', components[stage]),
                 ('R_C_Shortcuts', 1, r'Software\PSPC\Components', 'C_Shortcuts', '#1', 'C_Shortcuts')]
    comp_rows.append(('C_Shortcuts', component_guid('C_Shortcuts'), 'INSTALLDIR', 4 | 256, None, 'R_C_Shortcuts'))

    # Files, all in one cabinet (the File table's Sequence is the order in the cabinet)
    cab = msilib.CAB('pspc')
    file_rows, file_keys = [], {}
    used_keys = set()
    for path in sorted((p for p in stage.rglob('*') if p.is_file()), key=lambda p: p.relative_to(stage).as_posix()):
        rel = path.relative_to(stage).as_posix()
        key = identifier('F_', rel, used_keys)
        seq, _ = cab.append(str(path), path.name, key)
        ver, lang = file_version(path)
        file_rows.append((key, components[path.parent], filename_column(path.name, short_names[path.parent]),
                          path.stat().st_size, ver, lang if ver else None, 512, seq))  # vital
        file_keys[rel] = key
    launcher = file_keys[LAUNCHER]

    msilib.add_data(db, 'Component', comp_rows)
    msilib.add_data(db, 'Registry', reg_rows)
    msilib.add_data(db, 'RemoveFile', remove_rows)
    msilib.add_data(db, 'CreateFolder', create_rows)
    msilib.add_data(db, 'File', file_rows)
    msilib.add_data(db, 'Feature', [('PSPC', None, 'PSPC', 'PSPC, its game servers and their Python runtime.', 1, 1,
                                     'INSTALLDIR', 16)])  # 16: can't be made absent
    msilib.add_data(db, 'FeatureComponents', [('PSPC', r[0]) for r in comp_rows])

    icon_name = None
    if icon:
        icon_name = 'pspc.ico'
        msilib.add_data(db, 'Icon', [(icon_name, msilib.Binary(str(icon)))])
    msilib.add_data(db, 'Shortcut', [
        (sid, folder, 'PSPC', 'C_Shortcuts', '[#%s]' % launcher, None, 'Play PSP games', None, icon_name,
         0 if icon_name else None, None, 'INSTALLDIR')
        for sid, folder in (('S_StartMenu', 'ProgramMenuFolder'), ('S_Desktop', 'DesktopFolder'))])

    props = [('UpgradeCode', UPGRADE_CODE),
             ('ARPHELPLINK', URL), ('ARPURLINFOABOUT', URL), ('ARPNOMODIFY', '1'),
             ('ARPCOMMENTS', 'Plays PSP games on Windows'),
             ('DefaultUIFont', 'DlgFont8'), ('ErrorDialog', 'ErrorDlg'),
             ('LAUNCHPSPC', '1'),
             ('SecureCustomProperties', 'PSPC_UPGRADE;PSPC_NEWER;PSPC_PREVDIR')]
    if icon_name:
        props.append(('ARPPRODUCTICON', icon_name))
    msilib.add_data(db, 'Property', props)

    # Upgrades: older or equal versions are removed (equal: a rebuild with a new ProductCode must
    # replace, not join, the installed one); a newer installed version blocks this package.
    msilib.add_data(db, 'Upgrade', [
        (UPGRADE_CODE, None, version, None, 0x200 | 1, None, 'PSPC_UPGRADE'),  # max inclusive | migrate features
        (UPGRADE_CODE, version, None, None, 2, None, 'PSPC_NEWER'),  # only detect, min exclusive
    ])
    msilib.add_data(db, 'LaunchCondition', [
        ('Installed OR NOT PSPC_NEWER', 'A newer version of PSPC is already installed. To install this older version, '
                                        'uninstall PSPC first (Settings > Apps > Installed apps).')])
    # Install into the folder of the installed PSPC (upgrades, repair, uninstall) unless INSTALLDIR
    # is given: HKCU\Software\PSPC\InstallDir.
    msilib.add_data(db, 'AppSearch', [('PSPC_PREVDIR', 'PrevInstallDir')])
    msilib.add_data(db, 'RegLocator', [('PrevInstallDir', 1, r'Software\PSPC', 'InstallDir', 2)])  # HKCU, raw value
    msilib.add_data(db, 'CustomAction', [
        ('SetInstallDirFromRegistry', 51, 'INSTALLDIR', '[PSPC_PREVDIR]'),  # set property
        ('SetARPINSTALLLOCATION', 51, 'ARPINSTALLLOCATION', '[INSTALLDIR]'),
        ('LaunchPSPC', 210, launcher, None),  # exe from an installed file, async, no wait
    ])
    prevdir = ('SetInstallDirFromRegistry', 'PSPC_PREVDIR AND NOT INSTALLDIR', 410)
    add_sequences(db,
                  [prevdir, ('WelcomeDlg', 'NOT Installed', 1230),
                   ('MaintenanceDlg', 'Installed AND NOT RESUME AND NOT Preselected', 1250),
                   ('ProgressDlg', None, 1280)],
                  [prevdir, ('SetARPINSTALLLOCATION', None, 1010)])
    msilib.add_tables(db, msitext)  # ActionText, UIText
    bmp = temp / 'white.bmp'
    white_bitmap(bmp)
    add_ui(db, bmp)
    db.Commit()
    ui = check_ui(db)

    cab.commit(db)  # compresses the files into the cabinet stream; adds the Media row
    db.Commit()
    db.Close()
    os.replace(tmp_msi, out)
    return product_code, len(file_rows), len(comp_rows), ui


# ---------------------------------------------------------------------------------------------


def size_mb(n):
    return '%.1f MB' % (n / 1048576)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--build-dir', default=str(ROOT / 'build' / 'RelWithDebInfo'),
                    help='PSPC build tree; the binaries come from its bin folder (default: %(default)s)')
    ap.add_argument('--python', default=sys.base_prefix,
                    help='Python 3.12 installation the private runtime is made from (default: %(default)s)')
    ap.add_argument('--aot-toolchain', default=str(ROOT / 'third_party' / 'llvm-mingw'),
                    help='portable LLVM/MinGW from tools/fetch_aot.py (included in every installer)')
    ap.add_argument('--version', help='ProductVersion x.y.z (default: from CMakeLists.txt)')
    ap.add_argument('--out-dir', default=str(ROOT / 'build' / 'installer'),
                    help='where the MSI and the stage go (default: %(default)s)')
    ap.add_argument('--icon', help='.ico (or an .exe to take the icon of) for the shortcuts and Add/Remove '
                                   'Programs (default: the icon of pspc-launcher.exe; none if it has none)')
    ap.add_argument('--no-verify', action='store_true', help="skip staged Python and AOT execution checks")
    args = ap.parse_args(argv)

    if struct.calcsize('P') != 8:
        print('error: run this with 64-bit Python', file=sys.stderr)
        return 1
    try:
        version = msi_version(args.version or project_version())
        build_dir = pathlib.Path(args.build_dir).resolve()
        pyhome = pathlib.Path(args.python).resolve()
        out_dir = pathlib.Path(args.out_dir).resolve()
        stage = out_dir / 'stage'
        temp = out_dir / 'msi-temp'
        for d in (stage, temp):
            if d.exists():
                shutil.rmtree(d)
            d.mkdir(parents=True)
        out = out_dir / ('PSPC-%s-x64.msi' % version)
        print('PSPC %s: staging %s' % (version, stage))

        stage_pspc(build_dir, stage)
        stage_aot(build_dir, stage, pathlib.Path(args.aot_toolchain).resolve())
        tag, compiled, modules = stage_python(pyhome, stage / 'python')
        print('  runtime: %s from %s, %d standard library modules%s' %
              (tag, pyhome, modules, ' (compiled)' if compiled else ' (source)'))
        if compiled:
            print('  precompiled %d server/tool modules' % precompile(stage))
        check_stage(stage)
        if not args.no_verify:
            print('checking the private Python runtime')
            verify_runtime(stage, pyhome)

        icon = None
        source = pathlib.Path(args.icon) if args.icon else stage / LAUNCHER
        try:
            data = source.read_bytes() if source.suffix.lower() == '.ico' else extract_icon(source)
            if data:
                icon = temp / 'pspc.ico'
                icon.write_bytes(data)
            else:
                print('warning: %s has no icon resource: the shortcuts and Add/Remove Programs get a default icon'
                      % source.name, file=sys.stderr)
        except (OSError, ValueError, struct.error, IndexError) as e:
            print('warning: could not read the icon of %s (%s): no icon' % (source, e), file=sys.stderr)

        print('writing %s' % out)
        product_code, nfiles, ncomponents, (ndialogs, ncontrols) = build_msi(stage, out, version, icon, temp)
        shutil.rmtree(temp, ignore_errors=True)
    except (BuildError, OSError, RuntimeError, msilib.MSIError) as e:
        print('error: %s' % e, file=sys.stderr)
        return 1

    listing = stage_listing(stage)
    groups = {}
    for rel, size in listing.items():
        top = rel.split('/')[0] if '/' in rel else '.'
        n, total = groups.get(top, (0, 0))
        groups[top] = (n + 1, total + size)
    print('done: %s (%s), ProductCode %s' % (out, size_mb(out.stat().st_size), product_code))
    print('  %d files in %d components, %d dialogs (%d controls), icon: %s' %
          (nfiles, ncomponents, ndialogs, ncontrols, 'yes' if icon else 'no'))
    for top in sorted(groups):
        n, total = groups[top]
        print('  %-8s %4d files  %s' % (top if top != '.' else '(top)', n, size_mb(total)))
    print('  installed size %s' % size_mb(sum(listing.values())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
