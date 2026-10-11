#!/bin/bash
# Builds PSPC's own guest test programs (tests/guest/*/) with the pspdev toolchain, into
# build/guest-tests/<name>/<name>.prx. Run it under Linux or WSL with pspdev ($PSPDEV, default
# ~/pspdev) and make installed. The sources are built in a copy.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/guest-tests"
export PSPDEV="${PSPDEV:-$HOME/pspdev}"
export PATH="$PATH:$PSPDEV/bin"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK:?}"' EXIT

for dir in "$ROOT"/tests/guest/*/; do
	name="$(basename "$dir")"
	cp -r "$dir" "$WORK/$name"
	make -C "$WORK/$name" >/dev/null
	mkdir -p "$OUT/$name"
	cp "$WORK/$name/$name.prx" "$OUT/$name/"
	echo "built $name"
done
