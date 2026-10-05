#!/usr/bin/env python3
"""Run the same semantic contracts as lit without needing lit installed."""
import argparse
from collections import Counter
import json
from pathlib import Path
import shutil
import sys
from test_support import run_case

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--still", default=str(ROOT / "build-cmake/still"))
    ap.add_argument("--filter", default="")
    ap.add_argument("--json", type=Path)
    ap.add_argument("--strict", action="store_true", help="also fail for documented compiler gaps")
    args = ap.parse_args()
    compiler = shutil.which(args.still)
    if not compiler:
        ap.error("compiler not found: " + args.still)
    records = []
    for src in sorted((ROOT / "tests").rglob("*.wky")):
        if "lib" in src.relative_to(ROOT / "tests").parts or args.filter not in str(src):
            continue
        for result in run_case(src, str(Path(compiler).resolve())):
            result["test"] = str(src.relative_to(ROOT))
            records.append(result)
            print(f"{result['status']:11} {result['test']} O{result.get('optimization', '-')}" , flush=True)
            if result["status"] in {"FAIL", "XPASS"}:
                print(result["detail"], flush=True)
    counts = Counter(r["status"] for r in records)
    print(dict(counts))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"counts": dict(counts), "results": records}, indent=2) + "\n")
    return int(bool(not records or counts["FAIL"] or counts["XPASS"] or (args.strict and counts["XFAIL"])))


if __name__ == "__main__":
    sys.exit(main())
