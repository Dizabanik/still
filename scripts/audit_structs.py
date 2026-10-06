#!/usr/bin/env python3
"""Audit struct/impl readiness; failures remain visible, independently of CTest.

This is a readiness checklist, including capabilities and rejection contracts
that the compiler may not implement yet. It never treats known defects as PASS.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

from test_support import invoke

ROOT = Path(__file__).resolve().parents[1]


def audit(compiler, case, opt):
    with tempfile.TemporaryDirectory(prefix='still-struct-audit-') as directory:
        work = Path(directory)
        source = work / 'main.wky'
        source.write_text(case['source'])
        for name, body in case.get('libraries', {}).items():
            library = work / name
            library.parent.mkdir(parents=True, exist_ok=True)
            library.write_text(body)
        command = [compiler, f'-O{opt}', '--bounds-check=safe', str(source)]
        command += ['-c'] if case['kind'] == 'reject' else ['-o', str(work / 'program')]
        record = {'id': case['id'], 'optimization': opt, 'status': 'FAIL'}
        try:
            compiled = invoke(command, work, 20)
            record.update(compile_exit=compiled.returncode,
                          diagnostic=(compiled.stdout + compiled.stderr).decode(errors='replace'))
            if compiled.returncode < 0:
                record['reason'] = 'compiler crashed'
            elif case['kind'] == 'reject':
                if compiled.returncode == 1 and re.search(case['diagnostic'], record['diagnostic']):
                    record['status'] = 'PASS'
                else:
                    record['reason'] = ('accepted invalid source' if compiled.returncode == 0
                                        else 'wrong rejection diagnostic')
            elif compiled.returncode != 0 or compiled.stderr:
                record['reason'] = 'valid source did not compile cleanly'
            else:
                executed = invoke([str(work / 'program')], work, 10)
                record.update(run_exit=executed.returncode,
                              stdout=executed.stdout.decode(errors='replace'),
                              stderr=executed.stderr.decode(errors='replace'))
                if (executed.returncode == 0 and executed.stdout == case['stdout'].encode()
                        and not executed.stderr):
                    record['status'] = 'PASS'
                else:
                    record['reason'] = 'execution differed from independent oracle'
        except subprocess.TimeoutExpired:
            record['reason'] = 'compiler or program timed out'
        return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--still', default=str(ROOT / 'build-cmake/still'))
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    compiler = shutil.which(args.still)
    if not compiler:
        parser.error('compiler not found: ' + args.still)
    compiler = str(Path(compiler).resolve())
    manifest = ROOT / 'tests/struct-readiness.json'
    cases = json.loads(manifest.read_text())['cases']
    assert cases and len({case['id'] for case in cases}) == len(cases)
    with ThreadPoolExecutor(max_workers=4) as pool:
        jobs = [pool.submit(audit, compiler, case, opt) for case in cases for opt in (0, 2, 3)]
        records = [job.result() for job in jobs]
    counts = dict(Counter(record['status'] for record in records))
    for case in cases:
        results = [record for record in records if record['id'] == case['id']]
        print(case['id'] + ': ' + ', '.join(f"O{r['optimization']} {r['status']}" for r in results))
    print(counts)
    report = {'platform': platform.platform(),
              'compiler_sha256': hashlib.sha256(Path(compiler).read_bytes()).hexdigest(),
              'checklist_sha256': hashlib.sha256(manifest.read_bytes()).hexdigest(),
              'counts': counts, 'results': records}
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + '\n')
    return int(bool(counts.get('FAIL')))


if __name__ == '__main__':
    raise SystemExit(main())
