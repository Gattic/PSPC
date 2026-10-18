#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build/extract the real CPack archive; --isolate also mount-hides the source/build repo.

Requires CPack and readelf. The explicit isolation gate requires working bubblewrap;
it fails, rather than silently skipping, when unavailable. All outputs are disposable.
"""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BINARIES = ('pspc', 'pspc-launcher', 'pspc-recomp', 'pspc-iso', 'pspc-decrypt')


def run(args, cwd, env=None, expected=0, timeout=60):
    result = subprocess.run(list(map(str, args)), cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    print(result.stdout, end='', flush=True)
    assert result.returncode == expected, (args, result.returncode, result.stdout)
    return result.stdout


def exercise(prefix, compiler, hidden_root):
    work = prefix.parent
    if hidden_root:
        assert not (hidden_root / 'CMakeLists.txt').exists(), 'source tree is still visible'
        assert not (hidden_root / 'build').exists(), 'build/dependency tree is still visible'
    env = dict(os.environ, HOME=str(work / 'home'), PATH='/usr/bin:/bin',
               PYTHONHOME='/nonexistent/hostile-python', PYTHONPATH='/nonexistent/hostile-python',
               CXX=str(compiler), PYTHONDONTWRITEBYTECODE='1')
    for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_AUDIT'):
        env.pop(key, None)
    for kind in ('CONFIG', 'DATA', 'CACHE', 'STATE'):
        env['XDG_' + kind + '_HOME'] = str(work / 'home' / kind.lower())
    data, cache = work / 'home/data/pspc', work / 'home/cache/pspc'
    for name in BINARIES:
        dynamic = run(['readelf', '-d', prefix / name], work, env)
        assert 'RPATH' not in dynamic and 'RUNPATH' not in dynamic, name
        assert all('/' not in line.split('[', 1)[1] for line in dynamic.splitlines() if '(NEEDED)' in line)
        resolved = run(['ldd', prefix / name], work, env)
        assert 'not found' not in resolved, resolved
    run([prefix / 'pspc', '--version'], work, env)
    run([prefix / 'pspc', '--np', 'local', '--id', 'PackagePlayer'], work, env)
    config = work / 'home/config/pspc/pspc.ini'
    with config.open('a') as stream:
        stream.write('\ngames_dir=Games\nnetwork=0\nplugins=0\nmute=1\naot=1\nrenderer=software\n')
    games = data / 'Games'
    games.mkdir(parents=True)
    game = games / 'synthetic λ.elf'
    shutil.copy2(work / 'input.elf', game)
    assert str(game) in run([prefix / 'pspc-launcher', '--list'], work, env)
    run([sys.executable, '-I', prefix / 'tools/pspc_build.py', game, '--pspc-dir', prefix,
         '--cache', cache / 'aot', '--work', cache / 'work', '--jobs', '2'], work, env)
    saved = data / 'ms0/PSP/SAVEDATA/keep.bin'
    saved.parent.mkdir(parents=True)
    saved.write_bytes(b'package-save-sentinel')
    output = run([prefix / 'pspc', '--headless', '--timeout', '1', game], work, env, expected=124)
    assert '[AOT] ELF: 1 native functions' in output, output
    assert '0 interpreted instructions, 0 bad memory accesses' in output, output
    assert str(prefix / 'data/nids.csv') in output, output
    assert saved.read_bytes() == b'package-save-sentinel'
    run([sys.executable, '-I', prefix / 'tools/pspc_install_fw.py', '--help'], work, env)
    # Import and execute all installed server packages, never a source-tree package or PYTHONPATH.
    server = "import runpy,sys; sys.path.insert(0,sys.argv[1]); sys.argv=sys.argv[2:]; runpy.run_module(sys.argv[0],run_name='__main__')"
    for package in ('pspc_network', 'pspc_medius', 'pspc_gamespy'):
        run([sys.executable, '-I', '-B', '-c', server, prefix / 'server', package, '--help'], work, env)
    database = data / 'servers/package.sqlite'
    database.parent.mkdir(parents=True)
    accounts = run([sys.executable, '-I', '-B', '-c', server, prefix / 'server', 'pspc_network',
                    '--db', database, '--json', 'accounts'], work, env)
    assert accounts.strip() == '[]' and database.is_file()
    print('Installed runtime, GTK CLI, native execution, resources, settings/save and server database passed.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build-dir', type=Path)
    mode.add_argument('--prefix', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--isolate', action='store_true')
    parser.add_argument('--hidden-root', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.prefix:
        exercise(args.prefix, args.compiler, args.hidden_root)
        return
    if args.isolate and not shutil.which('bwrap'):
        parser.error('--isolate requires bubblewrap with user namespaces enabled')
    fixture = runpy.run_path(str(Path(__file__).with_name('install_fixture.py')))
    with tempfile.TemporaryDirectory(prefix='pspc runtime package ') as temporary:
        work = Path(temporary)
        run(['cpack', '--config', args.build_dir.resolve() / 'CPackConfig.cmake', '-B', work / 'archive'],
            work, timeout=120)
        archives = list((work / 'archive').glob('*.tar.gz'))
        assert len(archives) == 1, archives
        archive = archives[0]
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        assert Path(str(archive) + '.sha256').read_text().split()[0] == digest
        unpack = work / 'unpack'
        unpack.mkdir()
        run(['cmake', '-E', 'tar', 'xzf', archive], unpack)
        roots = list(unpack.iterdir())
        assert len(roots) == 1
        prefix = work / 'relocated λ'
        roots[0].rename(prefix)
        assert {p.name for p in prefix.iterdir()} == set(BINARIES) | {
            'aot', 'tools', 'server', 'data', 'docs', 'licenses', 'source', 'README.md', 'PLAN.md', 'LICENSE', 'THIRD_PARTY.md'}
        required = ['data/nids.csv', 'data/fw_boot_modules.csv', 'data/pspc.png',
                    'tools/pspc_desktop.py', 'tools/pspc_install_fw.py',
                    'tools/pspc_build.py', 'aot/lib/libpspc_sem.a', 'aot/lib/libpspc_common.a',
                    'licenses/pspc-decrypt/COPYING', 'licenses/at3_standalone/LICENSE',
                    'source/CMakeLists.txt', 'source/cmake/LinuxPackage.cmake',
                    'source/.github/workflows/linux-package.yml', 'source/tools/ci_linux.sh',
                    'source/src/frontend/main.cpp', 'source/tools/pspc-decrypt/libkirk/kirk_engine.c']
        assert all((prefix / p).is_file() for p in required)
        for path in prefix.rglob('*'):
            assert not path.is_symlink(), path
            assert path.name not in {'private', 'reference', 'Games', 'ms0', 'firmware', 'flash0', 'flash1', 'pspc.ini',
                                     'build', 'cache', 'servers', '.git', '.pi', '__pycache__'}, path
            assert path.suffix not in {'.elf', '.prx', '.pbp', '.iso', '.cso', '.sqlite', '.pyc', '.log'}, path
        # The source snapshot must contain the actual compiled inputs, including pre-existing dirty edits.
        for source in (ROOT / 'src').rglob('*'):
            if source.is_file():
                assert (prefix / 'source' / source.relative_to(ROOT)).read_bytes() == source.read_bytes(), source
        for source in [ROOT / 'CMakeLists.txt', ROOT / 'cmake/LinuxPackage.cmake',
                       ROOT / '.github/workflows/linux-package.yml', ROOT / 'tools/ci_linux.sh',
                       ROOT / 'tools/pspc_desktop.py', ROOT / 'tools/run_local_sdl.py',
                       *list((ROOT / 'cmake/local-sdl').glob('*')),
                       ROOT / 'tests/host/extract_wayland_geometry.py', ROOT / 'tests/host/test_wayland_scale.c',
                       ROOT / 'data/pspc.png',
                       ROOT / 'tools/pspc-decrypt/CMakeLists.txt', *list((ROOT / 'tools/pspc-decrypt/libkirk').glob('*'))]:
            if source.is_file():
                assert (prefix / 'source' / source.relative_to(ROOT)).read_bytes() == source.read_bytes(), source
        make_elf = runpy.run_path(str(ROOT / 'tests/aot/test_pipeline.py'))['make_elf']
        (work / 'input.elf').write_bytes(make_elf(struct.pack('<2I', 0x1000FFFF, 0)))
        probe = work / 'probe.py'
        shutil.copy2(__file__, probe)
        command = [sys.executable, '-I', probe, '--prefix', prefix, '--compiler', args.compiler.resolve()]
        if args.isolate:
            (work / 'tmp').mkdir()
            command += ['--hidden-root', ROOT]
            command = ['bwrap', '--die-with-parent', '--ro-bind', '/', '/', '--tmpfs', ROOT,
                       '--dev', '/dev', '--proc', '/proc', '--bind', work, work,
                       '--ro-bind', prefix, prefix, '--chdir', work, '--setenv', 'TMPDIR', work / 'tmp', '--'] + command
        before = fixture['snapshot'](prefix)
        fixture['readonly'](prefix)
        try:
            run(command, work, timeout=120)
            assert fixture['snapshot'](prefix) == before, 'installed payload was modified'
        finally:
            fixture['writable'](prefix)
        print('PASS: real archive checksum/contents/source snapshot; read-only relocation' +
              (' with source/build repository mount-hidden.' if args.isolate else '.'))
        print('Archive bytes:', archive.stat().st_size, '; SHA256:', digest)
    print('Disposable archive, staging, install and user data removed.')


if __name__ == '__main__':
    main()
