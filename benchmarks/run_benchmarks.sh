#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Linux/macOS wrapper for the PL/I benchmark harness (cf. run_benchmarks.cmd).
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Force UTF-8 regardless of locale: the harness prints ✓/≈/✗/⚠ and the
# C/POSIX locale would otherwise crash it on write (cf. run_benchmarks.cmd).
export PYTHONIOENCODING=utf-8
export PYTHONUTF8=1
export CC="${CC:-clang}"
export PLI="${PLI:-$DIR/../build/cmake/pli}"
if [ ! -x "$PLI" ] && [ -x "$DIR/../build/pli" ]; then
  PLI="$DIR/../build/pli"
  export PLI
fi
exec python3 "$DIR/run_benchmarks.py" "$@"
