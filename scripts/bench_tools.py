#!/usr/bin/env python3
"""Compiler check/format/build latency on a deterministic, verified corpus."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import random
import shutil
import statistics
import tempfile

from bench_memory import ROOT, digest, invoke
from bench_process import run_measured

CORPUS_VERSION=1
SEED=42

def corpus(functions,depth=0):
    parts=['// compiler_tools corpus version 1\n',
        'fn i64 input() { str text=std.process.arg_at(1); i64 value=0; '
        'for (i64 i=0;i<text.len;i+=1) { value=value*10+(i64)text[i]-48; } return value; }\n']
    if depth:
        parts.append('fn T level_0(T x) { return x; }\n')
        for i in range(1,depth+1):
            parts.append(f'fn T level_{i}(T x) {{ return level_{i-1}(x)+1; }}\n')
    kinds=['i64','i32','u64','u32'] if depth else ['i64']
    checksum=0
    for i in range(functions):
        kind=kinds[i%len(kinds)]
        expr=f'level_{depth}(x)*{i+1}+{i}' if depth else f'(x*17+{i})%100003'
        parts.append(f'pub fn {kind} kernel_{i}({kind} x) {{ return {expr}; }}\n')
        checksum+=((SEED+i+depth)*(i+1)+i) if depth else ((SEED+i)*17+i)%100003
    parts.append('fn main() { i64 seed=input(); i64 total=0;\n')
    for i in range(functions):
        kind=kinds[i%len(kinds)]
        parts.append(f'total+=(i64)kernel_{i}(({kind})(seed+{i}));\n')
    parts.append('println("{total}"); return 0; }\n')
    return ''.join(parts),f'{checksum}\n'.encode()

def artifacts(work):
    return {name:digest(work/name) for name in ('output.bc','output.ll','output.o')}
def verify_build_output(output,expected_hash,exe):
    expected=f"hash: {expected_hash}\n[Whisky] Built '{exe}'\n".encode()
    assert output==expected,('incorrect build/hash output',output,expected)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--still',default=str(ROOT/'build-cmake/still'))
    p.add_argument('--samples',type=int,default=7)
    p.add_argument('--warmups',type=int,default=2)
    p.add_argument('--verify-only',action='store_true')
    p.add_argument('--json',type=Path,default=ROOT/'build/compiler-tools-benchmark.json')
    a=p.parse_args()
    if a.samples<3 or a.warmups<1: p.error('need at least three samples and one warmup')
    compiler=Path(shutil.which(a.still) or a.still).resolve()
    report={'corpus_version':CORPUS_VERSION,'compiler_sha256':digest(compiler),
        'machine':platform.machine(),'platform':platform.platform(),
        'llvm_version':invoke(['llvm-config','--version'],ROOT).decode().strip(),
        'cache_policy':'Warm filesystem after validation; a fresh compiler process for each measurement; OS caches are not flushed.',
        'compiler_cache_hit_rate':None,'optimization':'O3 check/build',
        'timing_scope':'Whole compiler process; build includes object emission and linker.',
        'rss_scope':'Per-child OS wait4 high-water resident bytes; may include waited-for descendants, not summed concurrent process memory.',
        'reproducibility_scope':['output.bc','output.ll','output.o'],
        'samples':a.samples,'warmups':a.warmups,'corpus':[],'cases':[]}
    with tempfile.TemporaryDirectory(prefix='still-tool-bench-') as directory:
        root=Path(directory); configurations={}; fixed={}
        for name,count,depth in [('small',12,0),('large',256,0),('generic_depth_16',64,16)]:
            text,expected=corpus(count,depth)
            source=root/f'{name}.wky'; source.write_text(text)
            work=root/name; work.mkdir()
            before=set(work.iterdir())
            assert not invoke([compiler,'--check','-O3',source],work)
            assert set(work.iterdir())==before,'check mode created artifacts'
            formatted=invoke([compiler,'--format',source],work)
            assert set(work.iterdir())==before,'formatter created artifacts'
            canonical=root/f'{name}-formatted.wky'; canonical.write_bytes(formatted)
            assert invoke([compiler,'--format',canonical],work)==formatted,'formatter is not idempotent'
            exe=work/'program'
            for candidate in (canonical,source):
                output=invoke([compiler,'-O3','--emit-hash',candidate,'-o',exe],work)
                verify_build_output(output,digest(work/'output.bc'),exe)
                assert invoke([exe,str(SEED)],work)==expected,'independent execution checksum mismatch'
            fixed[name]=artifacts(work)
            verify_build_output(invoke([compiler,'-O3','--emit-hash',source,'-o',exe],work),fixed[name]['output.bc'],exe)
            assert artifacts(work)==fixed[name],'declared artifacts changed between repeated builds'
            report['corpus'].append({'name':name,'functions':count,'generic_depth':depth,
                'source_bytes':len(text.encode()),'source_sha256':hashlib.sha256(text.encode()).hexdigest(),
                'formatter_sha256':hashlib.sha256(formatted).hexdigest(),
                'expected_stdout':expected.decode(),'artifact_sha256':fixed[name],
                'object_file_bytes':(work/'output.o').stat().st_size,'binary_file_bytes':exe.stat().st_size,
                'llvm_size_berkeley':invoke(['llvm-size','--format=berkeley',exe],work).decode().strip()})
            configurations[name]=(source,work,exe,formatted,expected)
        measurements={(name,phase):[] for name in configurations for phase in ('check','format','build')}
        if not a.verify_only:
            rng=random.Random(729104)
            for round_index in range(a.warmups+a.samples):
                order=list(measurements); rng.shuffle(order)
                for name,phase in order:
                    source,work,exe,formatted,expected=configurations[name]
                    command=[compiler,'--format',source] if phase=='format' else (
                        [compiler,'--check','-O3',source] if phase=='check' else
                        [compiler,'-O3','--emit-hash',source,'-o',exe])
                    before={f.name:digest(f) for f in work.iterdir() if f.is_file()}
                    output,measurement=run_measured(command,work)
                    if phase=='format':
                        assert output==formatted
                    elif phase=='build':
                        verify_build_output(output,fixed[name]['output.bc'],exe)
                        assert artifacts(work)==fixed[name]
                        assert invoke([exe,str(SEED)],work)==expected
                    else:
                        assert not output
                    if phase!='build':
                        assert before=={f.name:digest(f) for f in work.iterdir() if f.is_file()},'non-build phase changed an artifact'
                    if round_index>=a.warmups: measurements[name,phase].append(measurement)
        for (name,phase),samples in measurements.items():
            record={'corpus':name,'phase':phase,'samples':samples}
            if samples:
                durations=[s['elapsed_ms'] for s in samples]; median=statistics.median(durations)
                mad=statistics.median(abs(v-median) for v in durations)
                record.update(median_ms=median,mad_ms=mad,
                    warnings=[msg for ok,msg in [(median<50,'process duration below 50 ms'),(mad/median>.05,'MAD exceeds 5%')] if ok])
                print(f'{name:16} {phase:6} {median:9.3f} ms MAD {mad:.3f}')
            report['cases'].append(record)
    a.json.parent.mkdir(parents=True,exist_ok=True)
    a.json.write_text(json.dumps(report,indent=2)+'\n')
    print(f'Verified check, formatter fixed points, independent execution checksums, and repeated declared artifacts for three corpora. Results: {a.json}')

if __name__=='__main__': main()
