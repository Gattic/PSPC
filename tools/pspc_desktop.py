#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Register/remove this installed PSPC launcher in the current user's application menu."""
import argparse
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parent.parent
DESKTOP_ID = 'io.github.Gattic.PSPC.desktop'
MARKER = '# Managed by PSPC desktop integration\n'


def applications_dir():
    value = Path(os.environ.get('XDG_DATA_HOME', ''))
    if not value.is_absolute():
        home = Path(os.environ.get('HOME', ''))
        if not home.is_absolute():
            raise ValueError('Set an absolute HOME or XDG_DATA_HOME for desktop registration')
        value = home / '.local/share'
    return value / 'applications'


def desktop_string(value):
    return value.replace('\\', '\\\\').replace('\n', '\\n').replace('\r', '\\r').replace('\t', '\\t')


def entry():
    launcher = str(ROOT / 'pspc-launcher')
    # Exec has its own quoting layer, after Desktop Entry string escapes. No shell is used.
    if '=' in launcher or any(ord(c) < 32 for c in launcher):
        raise ValueError('Desktop registration requires an install path without = or control characters')
    quoted = ''.join('\\' + c if c in '\\"`$' else c for c in launcher).replace('%', '%%')
    # GIO validates the executable before expanding %%. Keep literal-percent paths in
    # an argument; env replaces itself with the launcher, without shell interpretation.
    command = desktop_string(f'/usr/bin/env -- "{quoted}"')
    return (MARKER + '[Desktop Entry]\nType=Application\nName=PSPC\n'
            'Comment=PlayStation Portable emulator\n'
            f'Exec={command}\n'
            f'Icon={desktop_string(str(ROOT / "data/pspc.png"))}\n'
            'Terminal=false\nCategories=Game;Emulator;\nStartupNotify=false\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('install', 'remove'))
    args = parser.parse_args()
    try:
        target = applications_dir() / DESKTOP_ID
        content = entry()
        if target.is_symlink():
            raise ValueError(f'Refusing to replace/remove a desktop-file symlink: {target}')
        existing = target.read_text(encoding='utf-8') if target.exists() else None
        if args.action == 'remove':
            if existing is not None:
                if existing != content:
                    raise ValueError('Desktop entry belongs to another installation or was edited; left unchanged')
                target.unlink()
        else:
            if not os.access(ROOT / 'pspc-launcher', os.X_OK) or not (ROOT / 'data/pspc.png').is_file():
                raise ValueError('Run this helper from a complete installed PSPC package')
            if existing is not None and not existing.startswith(MARKER):
                raise ValueError(f'Refusing to overwrite an unmanaged desktop entry: {target}')
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=target.parent,
                                                 prefix='.' + DESKTOP_ID, delete=False) as stream:
                    temporary = Path(stream.name)
                    stream.write(content)
                temporary.chmod(0o644)
                temporary.replace(target)
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
        print(f'{args.action}: {target}')
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
