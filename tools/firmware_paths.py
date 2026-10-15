# SPDX-License-Identifier: GPL-2.0-or-later
"""Resolve the selected firmware once, before launching parallel guest tests.

The runtime owns platform paths, profile selection and migration. Passing its resolved
roots explicitly lets tests ignore other user settings without using a different OS.
"""
import json
from pathlib import Path
import subprocess


def resolve_firmware(pspc, flash0=None):
    command = [str(pspc), '--firmware-paths']
    if flash0 is not None:
        command += ['--flash0', str(flash0)]
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=30)
    if result.returncode:
        raise RuntimeError('Cannot resolve firmware paths: ' + (result.stderr or result.stdout).strip())
    paths = json.loads(result.stdout)
    return tuple(str(Path(paths[key]).resolve()) for key in ('flash0', 'flash1'))
