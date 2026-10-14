#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Linux launcher CLI must work without a display and preserve UTF-8 argv paths."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve())
env = dict(os.environ, GDK_BACKEND='x11')
env.pop('DISPLAY', None)
env.pop('WAYLAND_DISPLAY', None)


def run(*args):
    return subprocess.run([exe, *map(str, args)], env=env, capture_output=True,
                          text=True, encoding='utf-8', timeout=10)


with tempfile.TemporaryDirectory(prefix='pspc launcher ') as work:
    env['HOME'] = work
    for kind in ('CONFIG', 'DATA', 'CACHE', 'STATE'):
        env['XDG_' + kind + '_HOME'] = str(Path(work) / kind.lower())
    games = Path(work) / 'games λ'
    games.mkdir()
    game = games / '日本語 homebrew.ELF'
    game.write_bytes(b'\x7fELF')
    (games / 'broken.iso').write_bytes(b'not an image')
    nested = games / 'libraries'
    nested.mkdir()
    (nested / 'library.prx').write_bytes(b'\x7fELF')
    result = run(games, '--list')
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [f'\t{game.stem}\t{game}'], result.stdout
    assert run('--help').returncode == 0
    assert run('--unknown-option').returncode == 2
    missing = run(games / 'missing', '--list')
    assert missing.returncode == 1 and 'missing' in missing.stderr
    gui = run(games)
    assert gui.returncode == 1 and 'Cannot open the display' in gui.stderr, gui.stderr
print('launcher CLI: passed')
