#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Small deterministic correctness gates; no timings or speed thresholds.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
build_dir=${1:-build-ci}
build_dir=$(cd "$build_dir" && pwd)
still="$build_dir/still"
# The development host's Homebrew ASan runtime stalls before main. These
# untimed C comparison checks use the working Apple sanitizer on macOS.
cc=clang
if [[ $(uname -s) == Darwin ]]; then cc=/usr/bin/clang; fi

python3 scripts/bench.py --still "$still" --cc "$cc" --quick --verify-only --sanitize-c \
  --json "$build_dir/benchmark-verification.json"
# Historical programs still need to compile and agree with their C/Rust twins.
# These fixed-input comparisons remain separate from independent-oracle gates.
python3 scripts/bench.py --still "$still" --cc "$cc" --suite historical --verify-only \
  --json "$build_dir/historical-verification.json"
for workload in reference_walk subobject_walk; do
  python3 scripts/bench_memory.py --still "$still" --workload "$workload" \
    --rounds 100000 --size 1024 --verify-only --sanitize-c \
    --json "$build_dir/$workload-verification.json"
done
for workload in owner_tree owned_values; do
  python3 scripts/bench_owners.py --still "$still" --workload "$workload" \
    --size 2048 --trials 8 --verify-only \
    --json "$build_dir/$workload-verification.json"
done
python3 scripts/bench_tools.py --still "$still" --verify-only \
  --json "$build_dir/tooling-verification.json"
