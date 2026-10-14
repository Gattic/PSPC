#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Fixture-free Linux runtime/package gates shared by CI and local clean-checkout validation.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
config=${1:-LinuxCI}
build="$PWD/build/$config"
evidence="$build/ci-evidence"
mkdir -p "$evidence"

JOBS=${JOBS:-2} PSPC_CMAKE_ARGS="${PSPC_CMAKE_ARGS:-} -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPSPC_BUILD_RUNTIME=ON -DPSPC_BUILD_TESTS=ON" \
  ./build.sh "$config" 2>&1 | tee "$evidence/build.log"
# The decrypt suite requires user-supplied M33 binaries. Do not download them, weaken its
# assertions or claim they passed. Unit-level reference/GPU omissions remain visible in -V.
ctest --test-dir "$build" --no-tests=error -V -E '^pspc_decrypt_tests$' \
  2>&1 | tee "$evidence/ctest.log"
python3 -I tests/launcher/test_package.py --build-dir "$build" \
  --compiler "$(command -v "${CXX:-c++}")" --isolate \
  2>&1 | tee "$evidence/isolation.log"
dbus-run-session -- xvfb-run -a python3 -I tests/launcher/test_desktop.py \
  --build-dir "$build" --gui 2>&1 | tee "$evidence/desktop-gui.log"
cpack --config "$build/CPackConfig.cmake" -B "$build/package" \
  2>&1 | tee "$evidence/package.log"
printf '%s\n' 'PASS: fixture-free CTest, mount-hidden package and real desktop activation.' \
  'Not covered: M33/Sony/reference fixtures, retail gameplay or required OpenGL hardware parity.' \
  | tee "$evidence/scope.txt"
