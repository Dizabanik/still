#!/usr/bin/env python3
# Benchmark harness: builds each bench/*.kawa with the given kawac, times it
# best-of-N against its C and Rust twins, and reports parity. Exit 1 when any
# Kawa build regresses more than --tolerance beyond its C twin (CI-usable).
#
# Twins are discovered by prefix: b6_chan.kawa pairs with prebuilt executables
# b6_c (C) and b6_rs (Rust). Missing twins are reported, not fatal.
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


def best_of(cmd, runs, cwd, samples=None):
    """Best-of-N wall time. When `samples` is a list, each run is appended to
    it so callers can interleave kawa/C timing and cancel thermal or load
    drift that would otherwise bias whichever binary happens to run later."""
    best = float("inf")
    for _ in range(runs):
        t0 = time.perf_counter()
        subprocess.run(cmd, cwd=cwd, stdout=subprocess.DEVNULL, check=True)
        dt = time.perf_counter() - t0
        if samples is not None:
            samples.append(dt)
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
        # Prebuilt language twins live next to the sources as <prefix>_c /
        # <prefix>_rs executables.
        prefix = name.split("_")[0]
        c_twin = os.path.join(bench_dir, prefix + "_c")
        if not os.access(c_twin, os.X_OK):
            c_twin = None
        rs_twin = os.path.join(bench_dir, prefix + "_rs")
        if not os.access(rs_twin, os.X_OK):
            rs_twin = None

        exe = os.path.join(work, name)
        r = subprocess.run(
            [args.kawac, os.path.join(bench_dir, src), "-o", exe],
            capture_output=True, text=True)
        if r.returncode != 0:
            print(f"FAIL {name}: kawac error\n{r.stderr}")
            failures += 1
            continue

        # Interleave kawa/C/Rust samples round-robin: back-to-back timing of
        # one binary then the other lets thermal drift or background load
        # bias whichever ran later, which showed up as phantom ~2% swings.
        ks, cs, rs_s = [], [], []
        kt = best_of([exe], args.runs, work, samples=ks)
        ct = rt = None
        pct = None
        flag_parts = []
        if c_twin:
            c_bin = os.path.abspath(c_twin)
            for i in range(args.runs):
                best_of([exe], 1, work, samples=ks)
                best_of([c_bin], 1, bench_dir, samples=cs)
            if rs_twin:
                r_bin = os.path.abspath(rs_twin)
                for _ in range(args.runs):
                    best_of([r_bin], 1, bench_dir, samples=rs_s)
            kt = min(ks)
            ct = min(cs)
            rt = min(rs_s) if rs_s else None
            pct = (kt - ct) / ct * 100.0
            if pct > args.tolerance:
                flag_parts.append("REGRESSION vs C")
                failures += 1

        if "REGRESSION vs C" in flag_parts:
            flag = "REGRESSION"
        elif pct is not None:
            flag = "ok"
        else:
            flag = "no C twin"
        rows.append((name, kt, ct, rt, pct, flag))

    hdr = f"{'benchmark':<16} {'kawa':>8} {'C':>8} {'rust':>8} {'vs C':>8}  status"
    print(hdr)
    print("-" * len(hdr))
    for name, kt, ct, rt, pct, flag in rows:
        cs = f"{ct:>7.3f}s" if ct is not None else f"{'--':>8}"
        rs = f"{rt:>7.3f}s" if rt is not None else f"{'--':>8}"
        ps = f"{pct:>+7.1f}%" if pct is not None else f"{'--':>8}"
        print(f"{name:<16} {kt:>7.3f}s {cs} {rs} {ps}  {flag}")

    import shutil
    shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
