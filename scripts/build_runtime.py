#!/usr/bin/env python3
"""Standalone I/O runtime generation; normal builds generate it automatically."""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=ROOT / 'build/generated/kawa_runtime_bc.h')
parser.add_argument('--clang')
parser.add_argument('--portable', action='store_true')
args = parser.parse_args()
clang = args.clang or str(Path(subprocess.check_output(
    ['llvm-config', '--bindir'], text=True).strip()) / 'clang')
subprocess.run([sys.executable, str(ROOT / 'scripts/embed_runtime.py'),
                '--clang', clang, '--source', str(ROOT / 'src/runtime/kawa_runtime.c'),
                '--output', str(args.output), '--symbol', 'kawa_runtime_bc',
                *([] if args.portable else ['--native'])], check=True)
