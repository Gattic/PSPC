#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Linux AOT integration check: synthetic ELF -> CLI -> shared module -> native execution.

No PSP toolchain or firmware is needed. The optional runtime check links the real runtime.
CTest supplies the build directory, probe, and configured C++ compiler. Generated files live in one temporary workspace,
removed on exit; subprocess output is retained in the CTest log on failure.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import runpy
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
from unittest.mock import patch

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


def run(command, cwd, env, success=True):
    command = [str(arg) for arg in command]
    print('+', shlex.join(command), flush=True)
    result = subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors='replace', timeout=90)
    print(result.stdout, end='', flush=True)
    if success:
        result.check_returncode()
    else:
        require(result.returncode != 0, 'invalid installation/arguments were accepted')
    return result.stdout


def check_bundle_semantics(build_dir, work):
    # Check the installed SDK layout without needing to execute a Windows compiler.
    installed = work / 'relocated SDK λ'
    src = installed / 'aot/src'
    shutil.copytree(ROOT / 'src/allegrex', src / 'allegrex')
    (src / 'recomp').mkdir()
    shutil.copy2(ROOT / 'src/recomp/codegen.cpp', src / 'recomp/codegen.cpp')
    (installed / 'tools').mkdir()
    script = installed / 'tools/pspc_build.py'
    shutil.copy2(ROOT / 'tools/pspc_build.py', script)
    header = installed / 'aot/generated/pspc_sem_version.h'
    header.parent.mkdir()
    shutil.copy2(build_dir / 'generated/pspc_sem_version.h', header)
    spec = importlib.util.spec_from_file_location('installed_builder', script)
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    builder.SRC, builder.SEM_HEADER = src, header
    version = builder.sem_version()
    for path in (src / 'allegrex/sem.h', src / 'recomp/codegen.cpp', script):
        original = path.read_bytes()
        try:
            path.write_bytes(original + b'\n')
            try:
                builder.sem_version()
            except RuntimeError as error:
                require('rebuild the main tree first' in str(error), str(error))
            else:
                raise AssertionError('SDK source change did not invalidate semantics: ' + str(path))
        finally:
            path.write_bytes(original)
        require(builder.sem_version() == version, 'restored SDK no longer matches the build')
    print('Relocated SDK hash and semantics/emitter/builder invalidation checks passed', flush=True)


def write_inputs(work):
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
    return inputs, hashes


def check_pipeline(build_dir, probe, compiler, work):
    check_bundle_semantics(build_dir, work)
    env = dict(os.environ, CXX=str(compiler))
    inputs, hashes = write_inputs(work)
    # Relative paths (including spaces) must work in both build routes, including CMake's
    # archive-link boundary. Absolute paths here would hide that regression.
    command = [sys.executable, '-I', ROOT / 'tools/pspc_build.py', '--build-dir', build_dir,
               '--jobs', '2', '--cache', 'cache space', '--work', 'work space']
    cache = work / 'cache space'
    modules = [cache / (sha + '.so') for sha in hashes]
    stamps = [cache / (sha + '.semver') for sha in hashes]

    gap_report = work / 'gap report λ.txt'
    gap_report.write_text('stale report')
    run([build_dir / 'bin/pspc-recomp', inputs[1], '--info', '--dump-gaps', gap_report], work, env)
    require('stale report' not in gap_report.read_text(), 'gap report was not rewritten')

    run(command + inputs, work, env)
    require(not list((work / 'work space').iterdir()), 'per-invocation workspace was not cleaned up')
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

    # An open reader must retain the old module while new loads see the replacement.
    with modules[0].open('rb') as loaded:
        original = loaded.read()
        run(command + ['--cmake', '--force'] + inputs, work, env)
        require(os.fstat(loaded.fileno()).st_ino != modules[0].stat().st_ino,
                'rebuild overwrote the existing module inode')
        loaded.seek(0)
        require(loaded.read() == original, 'rebuild modified the old module contents')
    require(not list(cache.glob('.publish-*')), 'module publication staging was not cleaned up')
    require(all(stamp.read_text() == version for stamp in stamps), 'CMake build lost semantics stamps')
    run([probe] + modules, work, env)

    # Both guest_tests.py and large_image_test.py use this fresh-build helper.
    build_guest = runpy.run_path(str(ROOT / 'tools/guest_tests.py'))['build_native']
    guest_cache = work / 'guest cache \u03bb'
    with patch.dict(os.environ, CXX=str(compiler)):
        error = build_guest(work / inputs[0], guest_cache, build_dir)
    require(error is None, error)
    run([probe, guest_cache / modules[0].name, *modules[1:]], work, env)
    print('Guest test helper accepted and executed a freshly built native module', flush=True)
    print('AOT direct/CMake builds, guest round-up, native execution and cache checks passed', flush=True)


def check_install(build_dir, probe, compiler, work):
    env = dict(os.environ, CXX=str(compiler))
    # Exercise actual install rules, not a handcrafted copy of the dependency set.
    stage = work / 'staged SDK'
    run(['cmake', '--install', build_dir, '--component', 'AOT', '--prefix', stage], work, env)
    require((stage / 'tools/pspc_build.py').is_file(), 'AOT install did not deliver the builder')
    installed = work / 'relocated SDK λ'
    stage.rename(installed)
    script = installed / 'tools/pspc_build.py'
    inputs, hashes = write_inputs(work)
    command = [sys.executable, '-I', script, '--jobs', '2']
    cache = installed / 'cache/aot'
    modules = [cache / (sha + '.so') for sha in hashes]
    # Auto-detection must work with only the SDK: no pspc runtime or source checkout.
    require(not (installed / 'pspc').exists(), 'SDK unexpectedly includes the runtime')
    run(command + inputs, work, env)
    run([probe] + modules, work, env)
    require(not list((installed / 'cache/aot-work').iterdir()), 'installed workspace leaked')
    require(run(command + inputs, work, env).count('up to date') == len(inputs), 'installed warm cache failed')

    # A system-managed SDK is read-only; all build products can go outside it.
    paths = [installed, *installed.rglob('*')]
    modes = {path: path.stat().st_mode & 0o777 for path in paths}
    before = {str(p.relative_to(installed)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in paths if p.is_file()}
    try:
        for path, mode in modes.items():
            path.chmod(mode & ~0o222)
        external = work / 'external cache'
        run(command + ['--pspc-dir', installed, '--cache', external, '--work', work / 'external work',
                       '--cmake'] + inputs, work, env)
        run([probe] + [external / p.name for p in modules], work, env)
        after = {str(p.relative_to(installed)): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in installed.rglob('*') if p.is_file()}
        require(before == after, 'external build changed the read-only SDK')
    finally:
        for path, mode in modes.items():
            path.chmod(mode)

    # Never fall back to the developer's build when the installation is incomplete/stale.
    archive = installed / 'aot/lib/libpspc_sem.a'
    missing = archive.with_suffix('.missing')
    archive.rename(missing)
    try:
        output = run(command + inputs, work, env, success=False)
        require('AOT installation is incomplete' in output and archive.name in output, output)
    finally:
        missing.rename(archive)
    header = installed / 'aot/src/allegrex/sem.h'
    header.write_bytes(header.read_bytes() + b'\n')
    output = run(command + inputs, work, env, success=False)
    require('AOT sources or compiler policy changed' in output, output)
    output = run(command + ['--pspc-dir', installed, '--build-dir', build_dir] + inputs,
                 work, env, success=False)
    require('cannot be combined with --build-dir' in output, output)
    print('Relocated installed SDK: native execution, read-only external builds, cache and rejection checks passed',
          flush=True)


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
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--runtime', action='store_true', help='exercise the runtime-linked probe')
    mode.add_argument('--installed', action='store_true', help='exercise the CMake-installed Linux AOT SDK')
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    check = check_install if args.installed else check_runtime if args.runtime else check_pipeline
    # Installed-layout verification lives outside the source and build directories.
    with tempfile.TemporaryDirectory(prefix='aot pipeline ', dir=None if args.installed else build_dir / 'tests') as temporary:
        check(build_dir, args.probe.resolve(), args.compiler.absolute(), Path(temporary))


if __name__ == '__main__':
    main()
