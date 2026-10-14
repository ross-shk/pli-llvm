#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Linux/macOS wrapper for the PL/I benchmark harness (cf. run_benchmarks.cmd).
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export CC="${CC:-clang}"
export PLI="${PLI:-$DIR/../build/cmake/pli}"
if [ ! -x "$PLI" ] && [ -x "$DIR/../build/pli" ]; then
  PLI="$DIR/../build/pli"
  export PLI
fi
exec python3 "$DIR/run_benchmarks.py" "$@"
