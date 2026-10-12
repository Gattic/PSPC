#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch the pinned, redistributable x64 AOT compiler for release packaging.

Run: python -I tools/fetch_aot.py
Only release builders need this download; the installer includes the resulting toolchain.
"""
import hashlib
import pathlib
import shutil
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
VERSION = '20260616'
ARCHIVE = 'llvm-mingw-%s-ucrt-x86_64.zip' % VERSION
URL = 'https://github.com/mstorsjo/llvm-mingw/releases/download/%s/%s' % (VERSION, ARCHIVE)
SHA256 = 'b9b68a4d276e16fa25802aaba458e4638f64b3884c290aaccdc2d87083b6ca35'
# clang's target wrapper invokes clang-22; the compiler and archiver share these DLLs.
BIN_FILES = {'clang.exe', 'clang++.exe', 'clang-22.exe', 'ld.lld.exe', 'llvm-ar.exe',
             'libclang-cpp.dll', 'libLLVM-22.dll', 'libc++.dll', 'libunwind.dll',
             'libwinpthread-1.dll', 'mingw32-common.cfg', 'x86_64-w64-windows-gnu.cfg',
             'x86_64-w64-mingw32-clang++.exe'}


def main():
    archive = ROOT / 'third_party' / '_downloads' / ARCHIVE
    dest = ROOT / 'third_party' / 'llvm-mingw'
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        print('Downloading ' + URL, flush=True)
        part = archive.with_suffix('.part')
        urllib.request.urlretrieve(URL, part)
        part.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('SHA-256 mismatch: remove %s and retry' % archive)
    # An allowlist keeps other targets, debuggers and their Python runtime out of PSPC.
    with zipfile.ZipFile(archive) as z:
        prefix = ARCHIVE[:-4] + '/'
        for info in z.infolist():
            if not info.filename.startswith(prefix) or info.is_dir():
                continue
            rel = pathlib.PurePosixPath(info.filename[len(prefix):])
            if rel.is_absolute() or '..' in rel.parts:
                raise RuntimeError('unsafe archive member: %s' % rel)
            selected = (rel.as_posix() == 'LICENSE.TXT' or
                        rel.parts[0] in ('include', 'x86_64-w64-mingw32') or
                        rel.parts[:2] == ('lib', 'clang') or
                        rel.parts[0] == 'bin' and rel.name in BIN_FILES)
            if selected:
                out = dest.joinpath(*rel.parts)
                out.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, out.open('wb') as dst:
                    shutil.copyfileobj(src, dst)
    (dest / 'PSPC_TOOLCHAIN.txt').write_text('%s\nSHA256 %s\n' % (URL, SHA256), encoding='utf-8')
    print('AOT toolchain: ' + str(dest))


if __name__ == '__main__':
    main()
