#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run lifetime regressions with a tiny synthetic ELF and disposable guest storage."""
import argparse
import os
from pathlib import Path
import runpy
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--case', choices=('deferred', 'shutdown'), required=True)
    parser.add_argument('--valgrind')
    args = parser.parse_args()
    make_elf = runpy.run_path(str(Path(__file__).parents[1] / 'aot/test_pipeline.py'))['make_elf']
    with tempfile.TemporaryDirectory(prefix='pspc-lifetime-') as folder:
        work = Path(folder)
        elf = work / 'return.elf'
        elf.write_bytes(make_elf(struct.pack('<2I', 0x03E00008, 0)))  # jr ra; nop
        command = [str(args.probe.resolve()), args.case]
        if args.case == 'shutdown':
            command.append(str(elf))
        if args.valgrind:
            command = [args.valgrind, '--error-exitcode=99', '--exit-on-first-error=yes'] + command
        env = dict(os.environ, DEBUGINFOD_URLS='', HOME=str(work),
                   XDG_DATA_HOME=str(work / 'data'), XDG_CONFIG_HOME=str(work / 'config'),
                   XDG_CACHE_HOME=str(work / 'cache'), SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        subprocess.run(command, cwd=work, env=env, check=True, timeout=30)


if __name__ == '__main__':
    main()
