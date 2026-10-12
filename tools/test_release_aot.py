#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""End-to-end release AOT check, also run by make_installer.py before packaging.

Usage: python -I tools/test_release_aot.py <installed PSPC directory>
Creates an original, tiny MIPS ELF; needs neither Sony/game files nor the PSP SDK.
"""
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile


def fixture():
    # addiu v0, zero, 42; jr ra; nop. One executable segment, with a .text section.
    code = struct.pack('<3I', 0x2402002a, 0x03e00008, 0)
    ident = b'\x7fELF\x01\x01\x01' + bytes(9)
    head = ident + struct.pack('<HHIIIIIHHHHHH', 2, 8, 1, 0x08804000, 52, 0x110,
                               0, 52, 32, 1, 40, 2, 0)
    ph = struct.pack('<8I', 1, 0x100, 0x08804000, 0, len(code), len(code), 5, 4)
    text = struct.pack('<10I', 0, 1, 6, 0x08804000, 0x100, len(code), 0, 0, 4, 0)
    return (head + ph).ljust(0x100, b'\0') + code + bytes(4) + bytes(40) + text


def verify(stage, work, env):
    # Spaces and Unicode exercise the installed path/argument handling as well.
    work = work / 'AOT check é'
    work.mkdir()
    game = work / 'test game.elf'
    game.write_bytes(fixture())
    cache = work / 'cache'
    py = stage / 'python' / 'python.exe'
    build = [str(py), '-I', '-B', '-X', 'utf8', str(stage / 'tools' / 'pspc_build.py'),
             str(game), '--cache', str(cache), '--work', str(work / 'work'), '--jobs', '2']

    def run(cmd):
        try:
            r = subprocess.run(cmd, cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               encoding='utf-8', errors='replace', timeout=180)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise RuntimeError('release AOT check could not run %s: %s' % (cmd[0], e)) from e
        if r.returncode:
            raise RuntimeError('release AOT check failed (%d):\n%s' % (r.returncode, r.stdout))
        return r.stdout

    run(build)
    sha = hashlib.sha1(game.read_bytes()).hexdigest()
    dll = cache / (sha + '.dll')
    if not dll.is_file():
        raise RuntimeError('release AOT check produced no DLL')
    index = json.loads((cache / 'games.json').read_text(encoding='utf-8'))
    if not index['games'][str(game.resolve())]['modules'][0]['ok']:
        raise RuntimeError('release AOT check did not record a successful build')
    # Reuse, then invalidate the cache as an upgrade would.
    if 'up to date' not in run(build + ['--pspc-dir', str(stage)]):
        raise RuntimeError('release AOT cache was not reused')
    (cache / (sha + '.semver')).write_text('0x00000000\n')
    if 'up to date' in run(build):
        raise RuntimeError('release AOT cache was not rebuilt after a semantics change')
    output = run([str(stage / 'pspc.exe'), '--config', 'none', '--no-plugins', '--headless',
                  '--fast-time', '--timeout', '5', '--memstick', str(work / 'ms'),
                  '--flash0', str(work / 'flash'), '--flash1', str(work / 'flash1'),
                  '--aot-dir', str(cache), str(game)])
    if 'native functions' not in output or 'stale or mismatched' in output or 'LoadLibrary(' in output:
        raise RuntimeError('release runtime did not load the AOT DLL:\n' + output)

    # A PBP's DATA.PSP must reuse the same ELF cache, with a separate library index entry.
    pbp = work / 'EBOOT.PBP'
    elf = game.read_bytes()
    pbp.write_bytes(b'\0PBP' + struct.pack('<9I', 0x10000, *([40] * 7), 40 + len(elf)) + elf)
    if 'up to date' not in run([str(pbp) if a == str(game) else a for a in build]):
        raise RuntimeError('PBP did not reuse its embedded module')

    for options, message in ((['--pspc-dir', str(work / 'incomplete')], 'Repair or reinstall PSPC'),
                             (['--cmake'], 'bundled toolchain'),
                             (['--compiler', 'msvc'], 'bundled toolchain'),
                             (['--jobs', '0'], 'must be positive')):
        r = subprocess.run(build + options, cwd=work, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, encoding='utf-8', errors='replace', timeout=30)
        if not r.returncode or message not in r.stdout:
            raise RuntimeError('release AOT check did not reject %s:\n%s' % (options, r.stdout))


def main():
    stage = pathlib.Path(sys.argv[1]).resolve()
    system = pathlib.Path(os.environ.get('SystemRoot', r'C:\Windows'))
    env = {k: v for k, v in os.environ.items() if k.upper() not in
           ('PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'PYTHONHOME', 'PYTHONPATH')}
    env['PATH'] = os.pathsep.join(map(str, (system / 'System32', system)))
    env.update(INCLUDE='missing', LIB='missing', LIBPATH='missing', PYTHONHOME='missing', PYTHONPATH='missing')
    with tempfile.TemporaryDirectory(prefix='pspc-release-check-') as tmp:
        verify(stage, pathlib.Path(tmp), env)
    print('Release AOT: PASS (build, cache reuse, rebuild, native execution)')


if __name__ == '__main__':
    main()
