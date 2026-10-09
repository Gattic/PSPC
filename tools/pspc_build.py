#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Ahead-of-time build: recompile PSP modules to native DLLs for pspc.

Usage: pspc_build.py [options] <module.prx|.elf|EBOOT.PBP> [...]

For each input the plain ELF is located (PBP: DATA.PSP; encrypted ~PSP files go through
pspc-decrypt), recompiled with pspc-recomp, compiled with clang-cl (or cl) into <sha1>.dll and
copied into the AOT cache that pspc.exe searches (default: <pspc dir>/cache/aot).
"""
import argparse
import concurrent.futures
import hashlib
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / 'build' / 'RelWithDebInfo' / 'bin'
LIB = ROOT / 'build' / 'RelWithDebInfo' / 'lib'
SEM_HEADER = ROOT / 'build' / 'RelWithDebInfo' / 'generated' / 'pspc_sem_version.h'
LINK_LIBS = ('pspc_sem.lib', 'pspc_common.lib')

_seen_lock = threading.Lock()
_seen = set()


def sem_version():
    """The semantics version the runtime will demand (see src/CMakeLists.txt)."""
    for line in SEM_HEADER.read_text().splitlines():
        if line.startswith('#define PSPC_SEM_VERSION'):
            return line.split()[2].rstrip('u').lower()
    raise RuntimeError('no PSPC_SEM_VERSION in %s' % SEM_HEADER)


def snapshot_libs(work, version):
    """Copies the libraries the DLLs link so a concurrent main build can't swap them mid-batch."""
    dest = work / ('lib-' + version)
    dest.mkdir(parents=True, exist_ok=True)
    for name in LINK_LIBS:
        src = LIB / name
        if not src.exists():
            raise RuntimeError('%s missing - build the main tree first' % src)
        shutil.copy2(src, dest / name)
    return dest


def vs_env():
    vswhere = pathlib.Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / \
        'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    out = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                          'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                         stdout=subprocess.PIPE, text=True).stdout.strip()
    vcvars = pathlib.Path(out) / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat'
    env_out = subprocess.run('call "%s" >nul && set' % vcvars, shell=True, stdout=subprocess.PIPE, text=True).stdout
    env = dict(os.environ)
    for line in env_out.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            env[k] = v
    links = pathlib.Path(os.environ.get('LOCALAPPDATA', '')) / 'Microsoft' / 'WinGet' / 'Links'
    env['PATH'] = env.get('PATH', '') + os.pathsep + str(links) + os.pathsep + r'C:\Program Files\LLVM\bin'
    return env


def plain_elf(path, work, decrypt_tool):
    data = path.read_bytes()
    if data[:4] == b'\0PBP':
        offs = struct.unpack_from('<8I', data, 8)
        end = offs[7] if offs[7] > offs[6] else len(data)
        data = data[offs[6]:end]
    if data[:4] == b'\x7fELF':
        return data
    if data[:4] == b'~PSP':
        tmp_in = work / ('in_' + hashlib.sha1(data).hexdigest())
        tmp_out = tmp_in.with_suffix('.elf')
        tmp_in.write_bytes(data)
        r = subprocess.run([str(decrypt_tool), 'prx', str(tmp_in), str(tmp_out)], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
        if r.returncode != 0:
            raise RuntimeError('pspc-decrypt failed (%d): %s' % (r.returncode, r.stderr.strip()))
        return tmp_out.read_bytes()
    raise RuntimeError('%s: not an ELF, PRX, PBP or ~PSP file' % path)


DISC_EXTS = {'.iso', '.cso', '.zso', '.dax'}


def expand_inputs(args, paths):
    """UMD images contribute every executable on the disc (EBOOT.BIN, BOOT.BIN, *.PRX)."""
    out = []
    for p in paths:
        if p.suffix.lower() not in DISC_EXTS:
            out.append(p)
            continue
        dest = pathlib.Path(args.work) / ('disc_' + hashlib.sha1(str(p.resolve()).encode()).hexdigest()[:12])
        dest.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([str(BIN / 'pspc-iso.exe'), 'modules', str(p), str(dest)], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
        if r.returncode not in (0, 3):
            raise RuntimeError('pspc-iso failed on %s: %s' % (p, r.stderr.strip()))
        for line in r.stdout.splitlines():
            if '\t' in line:
                out.append(pathlib.Path(line.split('\t', 1)[1]))
    return out


def build_one(args, env, path):
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    elf = plain_elf(path, work, pathlib.Path(args.decrypt))
    sha = hashlib.sha1(elf).hexdigest()
    with _seen_lock:
        if sha in _seen:
            return '%s: same module as another input (%s)' % (path.name, sha)
        _seen.add(sha)
    dest = pathlib.Path(args.cache) / (sha + '.dll')
    hints = pathlib.Path(args.cache) / (sha + '.hints')
    stamp = pathlib.Path(args.cache) / (sha + '.semver')
    stale = hints.exists() and dest.exists() and hints.stat().st_mtime > dest.stat().st_mtime
    if not stamp.exists() or stamp.read_text().strip() != args.sem_version:
        stale = True
    if dest.exists() and not args.force and not stale:
        return '%s: up to date (%s)' % (path.name, sha)
    elf_path = work / (sha + '.elf')
    elf_path.write_bytes(elf)
    src = work / sha
    if src.exists():
        shutil.rmtree(src)
    cmd = [str(BIN / 'pspc-recomp.exe'), str(elf_path), '-o', str(src)]
    if args.hints:
        cmd += ['--hints', args.hints]
    elif hints.exists():
        # Entry points the runtime saw the interpreter reach inside the previous build.
        cmd += ['--hints', str(hints)]
    if args.no_comments:
        cmd.append('--no-comments')
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError('pspc-recomp failed:\n' + r.stdout)
    summary = r.stdout.strip()
    bdir = src / 'build'
    compiler = 'clang-cl' if args.compiler == 'clang' else 'cl'
    cfg = ['cmake', '-S', str(src), '-B', str(bdir), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
           '-DCMAKE_CXX_COMPILER=' + compiler, '-DPSPC_SRC_DIR=' + str(ROOT / 'src').replace('\\', '/'),
           '-DPSPC_LIB_DIR=' + str(args.lib_dir).replace('\\', '/')]
    r = subprocess.run(cfg, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError('cmake configure failed:\n' + r.stdout[-4000:])
    r = subprocess.run(['cmake', '--build', str(bdir), '--parallel', str(args.jobs)], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError('compile failed:\n' + r.stdout[-6000:])
    dll = bdir / (sha + '.dll')
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(dll, dest)
    stamp.write_text(args.sem_version + '\n')
    if not args.keep_src:
        shutil.rmtree(src, ignore_errors=True)
        elf_path.unlink(missing_ok=True)
    return '%s -> %s\n%s' % (path.name, dest, summary)


def main():
    # @file reads one argument per line (long module lists).
    ap = argparse.ArgumentParser(fromfile_prefix_chars='@')
    ap.add_argument('inputs', nargs='+')
    ap.add_argument('--cache', default=str(BIN / 'cache' / 'aot'))
    ap.add_argument('--work', default=str(ROOT / 'build' / 'aot-work'))
    ap.add_argument('--decrypt', default=str(BIN / 'pspc-decrypt.exe'))
    ap.add_argument('--compiler', choices=['clang', 'msvc'], default='clang')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--modules-in-parallel', type=int, default=1)
    ap.add_argument('--hints', default=None)
    ap.add_argument('--no-comments', action='store_true')
    ap.add_argument('--keep-src', action='store_true')
    ap.add_argument('--force', action='store_true')
    args = ap.parse_args()
    env = vs_env()
    args.sem_version = sem_version()
    args.lib_dir = snapshot_libs(pathlib.Path(args.work), args.sem_version)
    paths = expand_inputs(args, [pathlib.Path(p) for p in args.inputs])
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.modules_in_parallel) as ex:
        futs = {ex.submit(build_one, args, env, p): p for p in paths}
        for f in concurrent.futures.as_completed(futs):
            try:
                print(f.result())
            except Exception as e:  # noqa: BLE001 - report and continue with other modules
                failures += 1
                print('FAILED %s: %s' % (futs[f], e))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
