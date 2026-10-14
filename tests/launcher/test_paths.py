#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runtime/GTK CLI defaults in a read-only relocated layout and isolated XDG homes."""
import argparse
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
fixture = runpy.run_path(str(Path(__file__).with_name('install_fixture.py')))
make_elf = runpy.run_path(str(ROOT / 'tests/aot/test_pipeline.py'))['make_elf']


def run(exe, args, env, cwd, expected=0):
    result = subprocess.run([str(exe), *map(str, args)], cwd=cwd, env=env, capture_output=True,
                            text=True, timeout=15)
    print(result.stdout + result.stderr, end='', flush=True)
    assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
    return result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='pspc installed paths ') as temp:
        work = Path(temp)
        stage = work / 'stage'
        fixture['stage'](args.build_dir.resolve(), stage)
        install = work / 'relocated λ'
        stage.rename(install)
        # Obsolete executable-local settings must neither override nor be modified by XDG use.
        legacy = install / 'pspc.ini'
        legacy.write_text('np_online_id=LegacyPlayer\ngames_dir=wrong-old-path\n')
        before = fixture['snapshot'](install)
        fixture['readonly'](install)
        try:
            for mode in ('explicit', 'defaults', 'relative'):
                home = work / mode
                home.mkdir()
                env = dict(os.environ, HOME=str(home), GDK_BACKEND='x11')
                env.pop('DISPLAY', None)
                env.pop('WAYLAND_DISPLAY', None)
                roots = {}
                for key, fallback in [('CONFIG', '.config'), ('DATA', '.local/share'),
                                      ('CACHE', '.cache'), ('STATE', '.local/state')]:
                    var = 'XDG_' + key + '_HOME'
                    env.pop(var, None)
                    roots[key] = home / fallback / 'pspc'
                    if mode == 'explicit':
                        env[var] = str(home / (key.lower() + ' λ'))
                        roots[key] = Path(env[var]) / 'pspc'
                    elif mode == 'relative':
                        env[var] = 'relative-must-not-use-cwd'
                runtime, launcher = install / 'pspc', install / 'pspc-launcher'
                run(runtime, ['--np', 'local', '--id', 'IsolatedPlayer'], env, work)
                config = roots['CONFIG'] / 'pspc.ini'
                assert 'np_online_id=IsolatedPlayer' in config.read_text()
                assert 'IsolatedPlayer' in run(runtime, ['--np', 'status'], env, install)
                # Default Games is writable and independent of the install/cwd.
                run(launcher, ['--list'], env, work)
                assert (roots['DATA'] / 'Games').is_dir()
                games = roots['DATA'] / 'relative games λ'
                games.mkdir()
                game = games / 'synthetic λ.elf'
                game.write_bytes(make_elf(struct.pack('<2I', 0x1000FFFF, 0)))
                if mode == 'explicit':
                    # Synthetic decrypted firmware exercises destination/tool separation, not Sony PSAR/XMB.
                    firmware = home / 'decrypted firmware'
                    vsh = firmware / 'flash0/vsh/module/vshmain.prx'
                    vsh.parent.mkdir(parents=True)
                    elf = bytearray(game.read_bytes())
                    struct.pack_into('<I', elf, 52 + 12, 0x80)  # module-info file offset
                    elf[0x84:0x8D] = b'Synthetic'
                    vsh.write_bytes(elf)
                    installer = install / 'tools/pspc_install_fw.py'
                    command = ['-I', installer, '--fw', firmware, '--pspc-dir', install]
                    run(sys.executable, command + ['--no-aot'], env, work, expected=2)
                    env['CXX'] = str(args.compiler.absolute())
                    run(sys.executable, command + ['--data-dir', roots['DATA'], '--aot-core', '--jobs', '2',
                        '--cache', roots['CACHE'] / 'aot', '--work', roots['CACHE'] / 'firmware-work'], env, work)
                    assert (roots['DATA'] / 'flash0/vsh/module/vshmain.prx').read_bytes() == elf
                    assert (roots['DATA'] / 'flash1/registry').is_dir()
                    assert list((roots['CACHE'] / 'aot').glob('*.so'))
                with config.open('a') as f:
                    f.write('\ngames_dir=relative games λ\nplugins_dir=plugins\nnetwork=0\naot=0\nmute=1\n')
                assert str(game) in run(launcher, ['--list'], env, work)
                output = run(runtime, ['--headless', '--timeout', '1', game], env, work, expected=124)
                assert 'Settings from ' + str(config) in output
                assert (roots['DATA'] / 'ms0/PSP/SAVEDATA').is_dir()
                assert (roots['DATA'] / 'flash1').is_dir()
                # Existing data survives a second boot; explicit CLI roots still take precedence.
                saved = roots['DATA'] / 'ms0/PSP/SAVEDATA/keep.bin'
                saved.write_bytes(b'owned-save-sentinel')
                override = home / 'explicit memory stick'
                run(runtime, ['--headless', '--timeout', '1', '--memstick', override, game], env, install, expected=124)
                assert (override / 'PSP/SAVEDATA').is_dir()
                assert saved.read_bytes() == b'owned-save-sentinel'
            assert not (work / 'relative-must-not-use-cwd').exists()
            assert fixture['snapshot'](install) == before, 'runtime/GTK changed the read-only installation'
        finally:
            fixture['writable'](install)
    print('Read-only relocated runtime/GTK CLI: XDG roots, HOME defaults, relative overrides, save preservation passed')


if __name__ == '__main__':
    main()
