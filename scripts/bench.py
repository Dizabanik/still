#!/usr/bin/env python3
# Benchmark harness: builds each bench/*.kawa with the given kawac, times it
# best-of-N against its C twin, and reports parity. Exit 1 when any Kawa build
# regresses more than --tolerance beyond its C twin (CI-usable).
#
# Usage: scripts/bench.py [--kawac PATH] [--runs N] [--tolerance PCT]

import argparse
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def best_of(cmd, runs, cwd):
    best = float("inf")
    for _ in range(runs):
        t0 = time.perf_counter()
        subprocess.run(cmd, cwd=cwd, stdout=subprocess.DEVNULL, check=True)
        dt = time.perf_counter() - t0
        best = min(best, dt)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kawac", default=os.path.join(ROOT, "build-cmake", "kawac"))
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--tolerance", type=float, default=10.0,
                    help="fail if kawa is slower than C by this percent")
    args = ap.parse_args()

    if not os.path.exists(args.kawac):
        sys.exit(f"kawac not found at {args.kawac}")

    work = tempfile.mkdtemp(prefix="kawa-bench-")
    bench_dir = os.path.join(ROOT, "bench")
    rows = []
    failures = 0

    for src in sorted(os.listdir(bench_dir)):
        if not src.endswith(".kawa"):
            continue
        name = src[: -len(".kawa")]
        # The C twins are prebuilt executables named <prefix>_c (b1_c, b2_c).
        prefix = name.split("_")[0]
        c_twin = os.path.join(bench_dir, prefix + "_c")
        if not os.access(c_twin, os.X_OK):
            c_twin = None

        exe = os.path.join(work, name)
        r = subprocess.run(
            [args.kawac, os.path.join(bench_dir, src), "-o", exe],
            capture_output=True, text=True)
        if r.returncode != 0:
            print(f"FAIL {name}: kawac error\n{r.stderr}")
            failures += 1
            continue

        kt = best_of([exe], args.runs, work)
        if c_twin:
            ct = best_of([os.path.abspath(c_twin)], args.runs, bench_dir)
            pct = (kt - ct) / ct * 100.0
            flag = "ok" if pct <= args.tolerance else "REGRESSION"
            if pct > args.tolerance:
                failures += 1
            rows.append((name, kt, ct, pct, flag))
        else:
            rows.append((name, kt, None, None, "no C twin"))

    print(f"{'benchmark':<16} {'kawa':>8} {'C':>8} {'delta':>8}  status")
    for name, kt, ct, pct, flag in rows:
        if ct is not None:
            print(f"{name:<16} {kt:>7.3f}s {ct:>7.3f}s {pct:>+7.1f}%  {flag}")
        else:
            print(f"{name:<16} {kt:>7.3f}s {'--':>8} {'--':>8}  {flag}")

    import shutil
    shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
