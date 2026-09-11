#!/usr/bin/env python3
"""Fresh-build, correctness-gated process benchmarks. No cross-language pass/fail races."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import math
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from test_support import environment, invoke
from bench_oracles import oracle

ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def summary(samples):
    median = statistics.median(samples)
    return {"samples_seconds": samples, "median_seconds": median,
            "min_seconds": min(samples), "max_seconds": max(samples),
            "mad_seconds": statistics.median(abs(x-median) for x in samples)}


def schedule(languages, runs, rng):
    # Shuffle each complete round: every implementation gets exactly one sample.
    rounds = []
    for _ in range(runs):
        order = list(languages)
        rng.shuffle(order)
        rounds.append(order)
    return rounds


def checked_output(result, expected=None, comparison=None):
    if result.returncode:
        raise RuntimeError(f"process exit {result.returncode}: {result.stderr.decode(errors='replace')}")
    if result.stderr:
        raise RuntimeError(f"unexpected stderr: {result.stderr.decode(errors='replace')}")
    if expected is None and comparison and comparison["kind"] == "numeric":
        expected = result.stdout  # Validate the first historical reference's format/finiteness too.
    if expected is not None:
        if comparison and comparison["kind"] == "numeric":
            want = re.fullmatch(comparison["pattern"], expected.decode("ascii"))
            got = re.fullmatch(comparison["pattern"], result.stdout.decode("ascii"))
            if want and got:
                pairs = [(float(a), float(b)) for a,b in zip(want.groups(), got.groups())]
                if pairs and all(math.isfinite(a) and math.isfinite(b) and
                                 math.isclose(a,b,rel_tol=comparison["rel_tolerance"],
                                              abs_tol=comparison["abs_tolerance"]) for a,b in pairs):
                    return result.stdout
            raise RuntimeError("numeric output is malformed, nonfinite, or outside the declared tolerance")
        if result.stdout != expected:
            raise RuntimeError(f"incorrect output: expected sha256={digest(expected)}, actual={digest(result.stdout)}; "
                               f"expected prefix={expected[:160]!r}, actual prefix={result.stdout[:160]!r}")
    return result.stdout


def build_commands(entry, compilers, directory, sanitize=False):
    commands = {}
    for lang, source in entry["sources"].items():
        src = str(ROOT / "bench" / source)
        exe = str(directory / lang)
        if lang == "kawa":
            flags = ["-O3", "--bounds-check=safe"]
        elif lang == "c":
            flags = ["-O3", "-march=native", "-std=c11", "-ffp-contract=off"]
            if sanitize:
                flags = ["-O1", "-g", "-std=c11", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        elif lang == "rust":
            flags = ["--edition=2021", "-C", "opt-level=3", "-C", "target-cpu=native",
                     "-C", "codegen-units=1", "-C", "panic=abort", "-C", "overflow-checks=no"]
        else:
            raise ValueError("unknown language: " + lang)
        commands[lang] = [compilers[lang], *flags, src, "-o", exe]
    return commands


def benchmark(entry, compilers, args, work, rng):
    folder = work / entry["name"]
    row = {"name": entry["name"], "suite": entry["suite"], "contract": entry["contract"],
           "comparison": entry.get("comparison", {"kind": "byte_exact"}),
           "status": "failed"}
    try:
        folder.mkdir()
        row["source_sha256"] = {k: digest((ROOT / "bench" / v).read_bytes()) for k,v in entry["sources"].items()}
        commands = build_commands(entry, compilers, folder)
        row["build_commands"] = commands
        row["build_seconds"] = {}
        row["build_diagnostics"] = {}
        for lang, command in commands.items():
            target = folder / (lang + "-build")
            target.mkdir()
            started = time.perf_counter()
            result = invoke(command, target, args.compile_timeout)
            row["build_seconds"][lang] = time.perf_counter() - started
            row["build_diagnostics"][lang] = (result.stdout + result.stderr).decode(errors="replace")
            if result.returncode:
                raise RuntimeError(lang + " build failed: " + (result.stdout + result.stderr).decode(errors="replace"))
        row["binary_bytes"] = {lang: (folder/lang).stat().st_size for lang in commands}
        cases = entry.get("cases", [[]])
        # New workloads have independent Python models; historical workloads only have differential checking.
        verified = []
        for case in cases:
            expected = oracle(entry["oracle"], case) if entry.get("oracle") else None
            hashes = {}
            for lang in commands:
                result = invoke([str(folder/lang), *map(str, case)], folder, args.timeout)
                output = checked_output(result, expected, entry.get("comparison"))
                hashes[lang] = digest(output)
                if expected is None:
                    expected = output
            verified.append({"args": case, "reference_stdout_sha256": digest(expected),
                             "stdout_sha256": hashes})
        row["verification"] = verified
        run_args = list(entry.get("timing_args", []))
        if args.quick and entry.get("quick_args"):
            run_args = list(entry["quick_args"])
        if args.iterations is not None and entry.get("oracle"):
            run_args[0] = args.iterations
        row["timing_args"] = run_args
        expected = oracle(entry["oracle"], run_args) if entry.get("oracle") else None
        # Verify the full timed input too; small-input agreement alone is insufficient.
        for lang in commands:
            output = checked_output(invoke([str(folder/lang), *map(str,run_args)], folder, args.timeout), expected, entry.get("comparison"))
            if expected is None:
                expected = output
        row["timed_stdout_sha256"] = digest(expected)
        if args.sanitize_c:
            sanitized = folder / "sanitized"
            sanitized.mkdir()
            command = build_commands(entry, compilers, sanitized, sanitize=True)["c"]
            row["c_sanitizer_command"] = command
            result = invoke(command, sanitized, args.compile_timeout)
            if result.returncode:
                raise RuntimeError("C sanitizer build failed: " + result.stderr.decode(errors="replace"))
            for case in cases + [run_args]:
                reference = oracle(entry["oracle"], case) if entry.get("oracle") else expected
                checked_output(invoke([str(sanitized/"c"), *map(str,case)], sanitized, args.timeout), reference, entry.get("comparison"))
            row["c_sanitizer"] = "passed (untimed)"
        else:
            row["c_sanitizer"] = "not run"
        if args.verify_only:
            row["status"] = "verified"
            return row
        for order in schedule(commands, args.warmups, rng):
            for lang in order:
                checked_output(invoke([str(folder/lang), *map(str,run_args)], folder, args.timeout), expected, entry.get("comparison"))
        samples = {lang: [] for lang in commands}
        row["sample_order"] = schedule(commands, args.runs, rng)
        for order in row["sample_order"]:
            for lang in order:
                start = time.perf_counter()
                result = invoke([str(folder/lang), *map(str,run_args)], folder, args.timeout)
                duration = time.perf_counter() - start
                checked_output(result, expected, entry.get("comparison"))  # Check outside the timer.
                samples[lang].append(duration)
        row["timings"] = {k: summary(v) for k,v in samples.items()}
        row["warnings"] = []
        for lang, values in row["timings"].items():
            if values["median_seconds"] < 0.05:
                row["warnings"].append(lang + ": under 50 ms; process startup/timer noise may dominate")
            if values["mad_seconds"] > values["median_seconds"] * 0.05:
                row["warnings"].append(lang + ": MAD exceeds 5% of median; rerun on an idle host")
        # Only reviewed matched-algorithm workloads receive comparative ratios.
        if entry["suite"] == "matched":
            row["ratios"] = {"kawa_over_"+k: row["timings"]["kawa"]["median_seconds"] / v["median_seconds"]
                             for k,v in row["timings"].items() if k != "kawa"}
        row["status"] = "measured"
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        row["error"] = str(exc)
        # Partial samples must never look like a successful speed comparison.
        row.pop("timings", None)
        row.pop("ratios", None)
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kawac", default=str(ROOT/"build-cmake/kawac"))
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--rustc", default="rustc")
    ap.add_argument("--suite", choices=["matched", "historical", "all"], default="matched")
    ap.add_argument("--filter", default="")
    ap.add_argument("--runs", type=int, default=9)
    ap.add_argument("--warmups", type=int, default=2)
    ap.add_argument("--seed", type=int, default=20260911)
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--compile-timeout", type=float, default=120)
    ap.add_argument("--json", type=Path, default=ROOT/"build/benchmark-results.json")
    ap.add_argument("--quick", action="store_true", help="smaller timed inputs; exploratory timing only")
    ap.add_argument("--iterations", type=int, help="override iterations for matched workloads (1..100000000)")
    ap.add_argument("--verify-only", action="store_true")
    ap.add_argument("--sanitize-c", action="store_true", help="run separate untimed ASan/UBSan C builds")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    if args.runs < 1 or args.warmups < 0 or min(args.timeout,args.compile_timeout) <= 0:
        ap.error("runs/timeouts must be positive; warmups cannot be negative")
    if args.iterations is not None and not 1 <= args.iterations <= 100_000_000:
        ap.error("iterations must be 1..100000000")
    entries = json.loads((ROOT/"bench/manifest.json").read_text())["benchmarks"]
    selected = [e for e in entries if (args.suite == "all" or e["suite"] == args.suite) and args.filter in e["name"]]
    if not selected:
        ap.error("no benchmarks matched")
    if args.list:
        for entry in selected:
            print(f"{entry['name']}: {entry['suite']} — {entry['contract']}")
        return 0
    compilers = {"kawa": args.kawac, "c": args.cc, "rust": args.rustc}
    for lang, compiler in compilers.items():
        found = shutil.which(compiler)
        if not found:
            ap.error("missing compiler: " + compiler)
        compilers[lang] = str(Path(found).absolute())
    report = {"schema_version": 1, "host": {"platform": platform.platform(), "machine": platform.machine(),
              "processor": platform.processor(), "cpu_count": os.cpu_count()},
              "method": {"clock": "perf_counter", "statistic": "median and median absolute deviation",
              "scope": "whole process: startup, initialization, kernel, formatting, stdout pipe, teardown",
              "warmups": args.warmups, "runs": args.runs, "seed": args.seed, "quick": args.quick,
              "iterations_override": args.iterations,
              "environment": {k:environment()[k] for k in ["LC_ALL","LANG","NO_COLOR"]},
              "note": "Matched means algorithm/data/layout/scheduling and valid-input semantics. Index checks are enabled; signed arithmetic is bounded by the input domain. CLI/runtime allocation costs can differ and are included. No temporal safety or aggregate language ranking is claimed."},
              "compilers": {}, "results": []}
    with tempfile.TemporaryDirectory(prefix="kawa-bench-") as folder:
        work = Path(folder)
        for lang, compiler in compilers.items():
            result = invoke([compiler, "--version"], work)
            if result.returncode:
                ap.error("could not query compiler version: " + compiler)
            report["compilers"][lang] = {"path": compiler, "version": result.stdout.decode(errors="replace").strip(),
                                         "binary_sha256": digest(Path(compiler).read_bytes())}
        rng = random.Random(args.seed)
        for entry in selected:
            print("Checking " + entry["name"], flush=True)
            row = benchmark(entry, compilers, args, work, rng)
            report["results"].append(row)
            print(row["status"].upper() + " " + entry["name"], flush=True)
            if "error" in row:
                print(row["error"], flush=True)
            for lang, values in row.get("timings", {}).items():
                print(f"  {lang}: median {values['median_seconds']:.6f}s; MAD {values['mad_seconds']:.6f}s")
            for warning in row.get("warnings", []):
                print("  warning: " + warning)
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps(report, indent=2) + "\n")
    print("Report: " + str(args.json.resolve()))
    return int(any(r["status"] == "failed" for r in report["results"]))


if __name__ == "__main__":
    sys.exit(main())
