#!/usr/bin/env python3
"""Typed propagation and owning enum/channel workloads with equal-policy C twins."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import sys
import tempfile
import time

from bench_memory import ROOT, digest, invoke
from bench_oracles import semantics_oracle
from bench_process import run_measured

WORKLOADS = ('typed_result', 'owned_enum', 'owned_channel')
EDGES = ((0, 0, 1), (1, 0, 1), (1, 7, 3), (17, 31, 3), (257, 65535, 17))


def verify(output, workload, args, metrics):
    values = [int(value) for value in output.split()]
    expected = semantics_oracle(workload, *args, metrics)
    assert len(values) == 9 and values[:8] == expected, (workload, args, metrics, values, expected)
    assert (values[8] > 0) == (values[2] > 0), (workload, args, values)
    return values


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--still', default=str(ROOT / 'build-cmake/still'))
    p.add_argument('--clang', default=str(Path(invoke(['llvm-config', '--bindir'], ROOT).decode().strip()) / 'clang'))
    p.add_argument('--sanitizer-clang', default='/usr/bin/clang' if sys.platform == 'darwin' else 'clang')
    p.add_argument('--workload', choices=WORKLOADS)
    p.add_argument('--rounds', type=int, default=2000000)
    p.add_argument('--result-rounds', type=int, default=100000000)
    p.add_argument('--seed', type=int, default=12345)
    p.add_argument('--parameter', type=int, default=17, help='failure period or requested channel capacity')
    p.add_argument('--samples', type=int, default=7)
    p.add_argument('--warmups', type=int, default=2)
    p.add_argument('--verify-only', action='store_true')
    p.add_argument('--json', type=Path, default=ROOT / 'build/semantics-benchmark.json')
    a = p.parse_args()
    if not (0 <= a.rounds <= 100000000 and 0 <= a.result_rounds <= 100000000
            and 0 <= a.seed <= 65535 and 1 <= a.parameter <= 256 and a.samples >= 3 and a.warmups >= 1):
        p.error('invalid workload domain or insufficient sampling')
    compiler = Path(shutil.which(a.still) or a.still).resolve()
    workloads = (a.workload,) if a.workload else WORKLOADS
    result = {
        'contract': 'Equivalent algorithms, ABI, zero initialization, unsigned arithmetic and cleanup. Owning workloads inline the same checked descriptor runtime in Whisky and C.',
        'timing_scope': 'Fresh whole process, including parsing, initialization, work, formatting and teardown; builds, oracles, sanitizers and instrumentation are untimed.',
        'timing_counters': False, 'platform': platform.platform(), 'machine': platform.machine(),
        'recorded_at_utc': datetime.now(timezone.utc).isoformat(), 'runner_sha256': digest(Path(__file__)),
        'compiler_sha256': digest(compiler), 'runtime_sha256': digest(ROOT / 'src/runtime/wky_memory.c'),
        'compiler_version': invoke([compiler, '--version'], ROOT).decode().strip(),
        'print_runtime_sha256': digest(ROOT / 'src/runtime/wky_runtime.c'),
        'runtime_header_sha256': digest(ROOT / 'src/runtime/wky_memory.h'),
        'common_header_sha256': digest(ROOT / 'bench/managed/semantics_common.h'),
        'oracle_sha256': digest(ROOT / 'scripts/bench_oracles.py'),
        'clang_version': invoke([a.clang, '--version'], ROOT).decode().splitlines()[0],
        'sanitizer_clang_version': invoke([a.sanitizer_clang, '--version'], ROOT).decode().splitlines()[0],
        'sdkroot': os.environ.get('SDKROOT'), 'warmups': a.warmups, 'samples': a.samples,
        'verification': {'edge_runs': 0, 'sanitized_runs': 0, 'instrumented_runs': 0}, 'builds': [], 'cases': []}
    with tempfile.TemporaryDirectory(prefix='wky-semantics-bench-') as directory:
        work = Path(directory)
        bins, inputs, records = {}, {}, {}
        for workload in workloads:
            rounds = a.result_rounds if workload == 'typed_result' else a.rounds
            inputs[workload] = [rounds, a.seed, a.parameter]
            for language in ('wky', 'c'):
                source = ROOT / f'bench/managed/{workload}.{language}'
                for optimization, metrics in ((0, False), (2, False), (3, False), (3, True), (1, False)):
                    sanitizer = optimization == 1
                    if sanitizer and language != 'c': continue
                    exe = work / f'{workload}-{language}-{optimization}-{int(metrics)}'
                    if language == 'wky':
                        command = [compiler, f'-O{optimization}', '--bounds-check=safe',
                                   *(['--memory-metrics'] if metrics else []), source, '-o', exe]
                    else:
                        command = [a.sanitizer_clang if sanitizer else a.clang, f'-O{optimization}',
                                   '-std=c11', *(['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
                                   if sanitizer else ['-march=native']),
                                   *(['-DWKY_MEMORY_METRICS'] if metrics else []), source, '-o', exe, '-pthread']
                    start = time.monotonic(); invoke(command, work)
                    result['builds'].append({'workload': workload, 'language': language, 'optimization': optimization,
                        'instrumented': metrics, 'sanitized': sanitizer, 'command': list(map(str, command)),
                        'elapsed_ms': (time.monotonic() - start) * 1000, 'source_sha256': digest(source),
                        'binary_sha256': digest(exe), 'binary_file_bytes': exe.stat().st_size})
                    for args in EDGES:
                        output = invoke([exe, *map(str, args)], work,
                            {'ASAN_OPTIONS': f'detect_leaks={int(sys.platform != "darwin")}:abort_on_error=1'} if sanitizer else None)
                        verify(output, workload, args, metrics)
                        result['verification']['sanitized_runs' if sanitizer else 'edge_runs'] += 1
                    if optimization == 3: bins[workload, language, metrics] = exe
                args = inputs[workload]
                metrics = verify(invoke([bins[workload, language, True], *map(str, args)], work), workload, args, True)
                result['verification']['instrumented_runs'] += 1
                exe = bins[workload, language, False]
                records[workload, language] = {
                    'workload': workload, 'language': language, 'args': args, 'source_sha256': digest(source),
                    'binary_sha256': digest(exe), 'binary_file_bytes': exe.stat().st_size,
                    'llvm_size_berkeley': invoke(['llvm-size', '--format=berkeley', exe], work).decode().strip(),
                    'instrumentation': dict(zip(('checksum', 'counted', 'allocations', 'frees', 'live_payload_bytes',
                                                'peak_payload_bytes', 'cloned_bytes', 'checks', 'descriptor_bytes'), metrics)),
                    'samples': []}
            assert records[workload, 'wky']['instrumentation'] == records[workload, 'c']['instrumentation'], ('different runtime policies', workload)
        if not a.verify_only:
            rng = random.Random(395717)
            for sample in range(a.warmups + a.samples):
                order = list(records); rng.shuffle(order)
                for position, (workload, language) in enumerate(order):
                    output, measurement = run_measured([bins[workload, language, False], *map(str, inputs[workload])], work)
                    verify(output, workload, inputs[workload], False)
                    if sample >= a.warmups:
                        records[workload, language]['samples'].append({**measurement, 'round': sample-a.warmups, 'order': position})
        for record in records.values():
            if record['samples']:
                durations = [sample['elapsed_ms'] for sample in record['samples']]
                median = statistics.median(durations)
                mad = statistics.median(abs(value - median) for value in durations)
                record.update(median_ms=median, mad_ms=mad, warnings=[message for condition, message in (
                    (median < 50, 'process duration below 50 ms'), (mad / median > .05, 'MAD exceeds 5%')) if condition])
                print(f'{record["workload"]:14} {record["language"]:3}: {median:.3f} ms, MAD {mad:.3f}')
            result['cases'].append(record)
    a.json.parent.mkdir(parents=True, exist_ok=True)
    a.json.write_text(json.dumps(result, indent=2)+'\n')
    print(f'Verified {result["verification"]}. Results: {a.json}')


if __name__ == '__main__': main()
