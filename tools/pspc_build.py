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

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / 'build' / 'RelWithDebInfo' / 'bin'
LIB = ROOT / 'build' / 'RelWithDebInfo' / 'lib'


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


def build_one(args, env, path):
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    elf = plain_elf(path, work, pathlib.Path(args.decrypt))
    sha = hashlib.sha1(elf).hexdigest()
    dest = pathlib.Path(args.cache) / (sha + '.dll')
    if dest.exists() and not args.force:
        return '%s: up to date (%s)' % (path.name, sha)
    elf_path = work / (sha + '.elf')
    elf_path.write_bytes(elf)
    src = work / sha
    if src.exists():
        shutil.rmtree(src)
    cmd = [str(BIN / 'pspc-recomp.exe'), str(elf_path), '-o', str(src)]
    if args.hints:
        cmd += ['--hints', args.hints]
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
           '-DPSPC_LIB_DIR=' + str(LIB).replace('\\', '/')]
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
    if not args.keep_src:
        shutil.rmtree(src, ignore_errors=True)
        elf_path.unlink(missing_ok=True)
    return '%s -> %s\n%s' % (path.name, dest, summary)


def main():
    ap = argparse.ArgumentParser()
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
    paths = [pathlib.Path(p) for p in args.inputs]
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
