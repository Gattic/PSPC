#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Linux AOT integration check: synthetic ELF -> CLI -> shared module -> native execution.

No PSP toolchain or firmware is needed. The optional runtime check links the real runtime.
CTest supplies the build directory, probe, and configured C++ compiler. Generated files live in one temporary workspace,
removed on exit; subprocess output is retained in the CTest log on failure.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def make_elf(code):
    entry = 0x08804000
    ident = b'\x7fELF' + bytes([1, 1, 1]) + bytes(9)
    header = ident + struct.pack('<HHIIIIIHHHHHH', 2, 8, 1, entry, 52, 0,
                                 0x10A23000, 52, 32, 1, 40, 0, 0)
    # A fixed MIPS ELF with one executable segment and no PSP module-info block.
    segment = struct.pack('<8I', 1, 0x100, entry, 0xFFFFFFFF, len(code), len(code), 5, 4)
    return (header + segment).ljust(0x100, b'\0') + code


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(command, cwd, env):
    command = [str(arg) for arg in command]
    print('+', shlex.join(command), flush=True)
    result = subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors='replace', timeout=90)
    print(result.stdout, end='', flush=True)
    result.check_returncode()
    return result.stdout


def check_pipeline(build_dir, probe, compiler, work):
    env = dict(os.environ, CXX=str(compiler))
    inputs = []
    hashes = []
    for name, addend in [('module one.elf', 7), ('module \u03b1.elf', 11)]:
        # lw t0,0(a0); addiu v0,t0,addend; sw v0,4(a0); syscall; jr ra; addiu v0,v0,1
        data = make_elf(struct.pack('<6I', 0x8C880000, 0x25020000 | addend, 0xAC820004,
                                   0x0000000C, 0x03E00008, 0x24420001))
        (work / name).write_bytes(data)
        inputs.append(name)
        hashes.append(hashlib.sha1(data).hexdigest())
    # CTC1 selects round-up; ADD.S computes 1.0f + 2^-24; MFC1 returns the bits.
    # Restore round-to-nearest after the result, as guest code commonly does.
    rounding = make_elf(struct.pack('<12I',
                                   0x24080002, 0x44C8F800, 0x3C083F80, 0x44880000,
                                   0x3C083380, 0x44880800, 0x46010080, 0x44021000,
                                   0x24080000, 0x44C8F800, 0x03E00008, 0))
    (work / 'rounding.elf').write_bytes(rounding)
    inputs.append('rounding.elf')
    hashes.append(hashlib.sha1(rounding).hexdigest())
    # Relative paths (including spaces) must work in both build routes, including CMake's
    # archive-link boundary. Absolute paths here would hide that regression.
    command = [sys.executable, '-I', ROOT / 'tools/pspc_build.py', '--build-dir', build_dir,
               '--jobs', '2', '--cache', 'cache space', '--work', 'work space']
    cache = work / 'cache space'
    modules = [cache / (sha + '.so') for sha in hashes]
    stamps = [cache / (sha + '.semver') for sha in hashes]

    run(command + inputs, work, env)
    require(all(module.is_file() for module in modules), 'missing SHA-1-named .so files')
    version = stamps[0].read_text()
    require(version.strip() and all(stamp.read_text() == version for stamp in stamps),
            'missing or inconsistent semantics stamps')
    index = json.loads((cache / 'games.json').read_text(encoding='utf-8'))
    require(index['version'] == 1, 'incorrect cache index version')
    for name, sha in zip(inputs, hashes):
        record = index['games'][str(work / name)]
        require(record['modules'] == [{'name': name, 'sha1': sha, 'ok': True}],
                'incorrect game/module cache index')
    run([probe] + modules, work, env)

    mtimes = [module.stat().st_mtime_ns for module in modules]
    output = run(command + inputs, work, env)
    require(output.count('up to date') == len(inputs), 'warm cache did not reuse all modules')
    require([module.stat().st_mtime_ns for module in modules] == mtimes, 'warm cache rewrote modules')

    stamps[0].write_text('stale\n')
    run(command + inputs, work, env)
    require(stamps[0].read_text() == version, 'stale semantics stamp was not replaced')
    require(modules[0].stat().st_mtime_ns != mtimes[0], 'stale module was not rebuilt')
    require([module.stat().st_mtime_ns for module in modules[1:]] == mtimes[1:],
            'valid cached module was rebuilt')

    run(command + ['--cmake', '--force'] + inputs, work, env)
    require(all(stamp.read_text() == version for stamp in stamps), 'CMake build lost semantics stamps')
    run([probe] + modules, work, env)
    print('AOT direct/CMake builds, guest round-up, native execution and cache checks passed', flush=True)


def check_runtime(build_dir, probe, compiler, work):
    env = dict(os.environ, CXX=str(compiler))
    hashes = []
    inputs = ['runtime module.elf', 'other module \u03b1.elf']
    for name, addend in zip(inputs, [7, 11]):
        # A native caller saves ra, calls the callee at entry+32, then adds one on return.
        # Patching the callee must also affect this direct call from compiled code.
        code = struct.pack('<13I',
                           0x27BDFFF0, 0xAFBF000C, 0x0E201008, 0,
                           0x8FBF000C, 0x27BD0010, 0x03E00008, 0x24420001,
                           0x8C880000, 0x25020000 | addend, 0xAC820004, 0x03E00008, 0)
        data = make_elf(code)
        (work / name).write_bytes(data)
        hashes.append(hashlib.sha1(data).hexdigest())
    run([sys.executable, '-I', ROOT / 'tools/pspc_build.py', '--build-dir', build_dir,
         '--jobs', '2', '--cache', 'cache space', '--work', 'work space'] + inputs, work, env)
    cache = work / 'cache space'
    modules = [cache / (sha + '.so') for sha in hashes]
    require(all(module.is_file() for module in modules), 'missing runtime fixture modules')
    for mode in ['native', 'disabled']:
        run([probe, inputs[0], cache, mode], work, env)
    missing = work / 'missing cache'
    missing.mkdir()
    run([probe, inputs[0], missing, 'missing'], work, env)
    mismatched = work / 'mismatched cache'
    mismatched.mkdir()
    # The file exists under the requested hash but describes a different ELF.
    (mismatched / modules[0].name).write_bytes(modules[1].read_bytes())
    output = run([probe, inputs[0], mismatched, 'mismatched'], work, env)
    require('stale or mismatched; ignoring' in output, 'descriptor mismatch was not diagnosed')
    print('Runtime AOT loading, native calls, interpreter fallback and unload checks passed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--runtime', action='store_true', help='exercise the runtime-linked probe')
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    check = check_runtime if args.runtime else check_pipeline
    with tempfile.TemporaryDirectory(prefix='aot pipeline ', dir=build_dir / 'tests') as temporary:
        check(build_dir, args.probe.resolve(), args.compiler.absolute(), Path(temporary))


if __name__ == '__main__':
    main()
