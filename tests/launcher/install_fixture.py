# SPDX-License-Identifier: GPL-2.0-or-later
"""Disposable staging of the actual Linux CMake installation."""
import hashlib
import subprocess


def stage(build, destination):
    subprocess.run(['cmake', '--install', str(build), '--prefix', str(destination)],
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def snapshot(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}


def readonly(root):
    for path in [root, *root.rglob('*')]:
        path.chmod(path.stat().st_mode & ~0o222)


def writable(root):
    for path in [root, *root.rglob('*')]:
        path.chmod(path.stat().st_mode | 0o200)
