#!/usr/bin/env python3
"""Check mode must diagnose fully without ever running a linker or program."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('compiler')
a = p.parse_args()
compiler = str(Path(a.compiler).resolve())
with tempfile.TemporaryDirectory(prefix='kawa-check-') as directory:
    root = Path(directory)
    for name in ('clang', 'cc'):
        fake = root / name
        fake.write_text('#!/bin/sh\necho invoked > linker-was-run\nexit 97\n')
        fake.chmod(0o755)
    source = root / 'input.kawa'
    for body, status in [
        ('fn main() { let a: owner<i64> = own(2); a[1]=42; return 0; }', 0),
        ('fn main() { let a: owner<i64> = own(2); let b=a; return 0; }', 1),
        ('fn main() { return missing; }', 1),
        ('fn main( {', 1),
    ]:
        source.write_text(body)
        before = set(root.iterdir())
        result = subprocess.run([compiler, '--check', '-O3', str(source)], cwd=root,
                                env={**os.environ, 'PATH': str(root)}, capture_output=True, timeout=30)
        assert result.returncode == status, result
        assert set(root.iterdir()) == before, 'check mode wrote an artifact or ran the linker'
print('PASS check mode: valid, ownership, undefined-name and syntax cases; no output or linker')
