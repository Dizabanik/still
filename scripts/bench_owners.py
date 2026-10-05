#!/usr/bin/env python3
"""Equal-policy nested ownership, with independent checksums and byte accounting."""
import argparse
import json
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import tempfile

from bench_memory import ROOT, UNAVAILABLE, digest, invoke
from bench_process import run_measured

def verify(output,args,instrumented,workload='owner_tree'):
    size,trials,seed,shape=args
    values=[int(x) for x in output.split()]
    if workload=='owned_values':
        checksum=(34*size*(size-1)+4*size*seed+138*size+seed)*trials
        allocations=(8+shape)*trials
        peak,cloned=64*size+136*shape,32*size*trials
        checks=(12*size+2*int(size>0)+3*shape)*trials
    else:
        checksum=(17*size*(size-1)//2+size*seed)*trials
        allocations=2*((max(1,size) if shape==0 else size+1))*trials
        peak,cloned=80*size,40*size*trials
        checks=(6*size+1 if shape==0 else 7*size+4)*trials if size else 0
    expected=[checksum,checksum+trials*int(size>0),allocations,allocations,0,peak,cloned]
    assert len(values)==9 and values[:7]==expected,(args,values,expected)
    assert values[7]>0 and values[8]==(checks if instrumented else UNAVAILABLE),(args,values,checks)
    return values

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kawac',default=str(ROOT/'build-cmake/kawac'))
    p.add_argument('--workload',choices=['owner_tree','owned_values'],default='owner_tree')
    p.add_argument('--clang',default=str(Path(invoke(['llvm-config','--bindir'],ROOT).decode().strip())/'clang'))
    p.add_argument('--sanitizer-clang',default='/usr/bin/clang' if platform.system()=='Darwin' else 'clang')
    p.add_argument('--size',type=int,default=100000)
    p.add_argument('--trials',type=int)
    p.add_argument('--seed',type=int,default=12345)
    p.add_argument('--samples',type=int,default=7)
    p.add_argument('--warmups',type=int,default=2)
    p.add_argument('--verify-only',action='store_true')
    p.add_argument('--json',type=Path)
    a=p.parse_args()
    if a.trials is None: a.trials=64 if a.workload=='owned_values' else 8
    if a.json is None:
        a.json=ROOT/'build'/('owned-values-benchmark.json' if a.workload=='owned_values' else 'owners-benchmark.json')
    if not (0<=a.size<=10**6 and 1<=a.trials<=100 and 0<=a.seed<=65535 and a.samples>=3 and a.warmups>=1):
        p.error('invalid workload or insufficient sampling')
    compiler=Path(shutil.which(a.kawac) or a.kawac).resolve()
    sources={lang:ROOT/f'bench/managed/{a.workload}.{ext}' for lang,ext in [('kawa','kawa'),('c','c')]}
    check=lambda output,args,metrics:verify(output,args,metrics,a.workload)
    result={'workload':a.workload,
        'contract':'Same zero initialization, unsigned arithmetic, runtime, checked accesses, moves, clone and lexical cleanup.',
        'timing_scope':'Fresh whole process, including initialization, cloning, verification traversal, cleanup and output.',
        'rss_scope':'OS wait4 ru_maxrss for each fresh child; high-water resident bytes, not total allocations or cumulative child usage.',
        'timing_counters':False,'platform':platform.platform(),'machine':platform.machine(),
        'compiler_sha256':digest(compiler),'sources':{l:digest(s) for l,s in sources.items()},
        'runtime_sha256':digest(ROOT/'src/runtime/kawa_memory.c'),
        'runtime_header_sha256':digest(ROOT/'src/runtime/kawa_memory.h'),
        'clang_version':invoke([a.clang,'--version'],ROOT).decode().splitlines()[0],
        'sanitizer_clang_version':invoke([a.sanitizer_clang,'--version'],ROOT).decode().splitlines()[0],
        'sdkroot':os.environ.get('SDKROOT'),'samples':a.samples,'warmups':a.warmups,'cases':[]}
    with tempfile.TemporaryDirectory(prefix='kawa-owner-bench-') as d:
        work=Path(d); bins={}
        for lang in sources:
            for metrics in (False,True):
                exe=work/f'{lang}-{int(metrics)}'
                command=[compiler,'-O3',*(['--memory-metrics'] if metrics else []),sources[lang],'-o',exe] if lang=='kawa' else [
                    a.clang,'-O3','-march=native','-std=c11',*(['-DKAWA_MEMORY_METRICS'] if metrics else []),sources[lang],'-o',exe,'-pthread']
                invoke(command,work); bins[lang,metrics]=exe
        for size,trials,seed in [(0,1,0),(1,3,65535),(2,2,7),(17,1,31),(1025,2,42)]:
            for shape in (0,1):
                args=[size,trials,seed,shape]
                for (lang,metrics),exe in bins.items():
                    check(invoke([exe,*map(str,args)],work),args,metrics)
        # The same C algorithm also runs under sanitizers; never time it.
        sanitized=work/'c-sanitized'
        invoke([a.sanitizer_clang,'-O1','-g','-std=c11','-fsanitize=address,undefined',
                '-fno-omit-frame-pointer',sources['c'],'-o',sanitized,'-pthread'],work)
        for shape in (0,1):
            args=[1025,2,42,shape]
            check(invoke([sanitized,*map(str,args)],work),args,False)
        measurements={(lang,shape):[] for lang in sources for shape in (0,1)}
        instrumentation={}
        for lang,shape in measurements:
            args=[a.size,a.trials,a.seed,shape]
            values=check(invoke([bins[lang,True],*map(str,args)],work),args,True)
            instrumentation[lang,shape]={'allocations':values[2],'frees':values[3],
                'live_payload_bytes':values[4],'peak_payload_bytes':values[5],
                'cloned_bytes':values[6],'descriptor_bytes':values[7],'checks':values[8],
                'reference_bytes':32,
                **({'aggregate_bytes':136} if a.workload=='owned_values' else {'chain_node_bytes':40})}
        for shape in (0,1):
            assert instrumentation['kawa',shape]==instrumentation['c',shape],('different policies/layout/counters',shape,instrumentation)
        if not a.verify_only:
            rng=random.Random(729104)
            for round_index in range(a.warmups+a.samples):
                order=list(measurements); rng.shuffle(order)
                for lang,shape in order:
                    args=[a.size,a.trials,a.seed,shape]
                    output,measurement=run_measured([bins[lang,False],*map(str,args)],work)
                    check(output,args,False)
                    if round_index>=a.warmups: measurements[lang,shape].append(measurement)
        for (lang,shape),samples in measurements.items():
            exe=bins[lang,False]
            labels=('stack','heap') if a.workload=='owned_values' else ('chain','fanout')
            record={'language':lang,'shape':labels[shape],'args':[a.size,a.trials,a.seed,shape],
                'binary_sha256':digest(exe),'binary_file_bytes':exe.stat().st_size,
                'llvm_size_berkeley':invoke(['llvm-size','--format=berkeley',exe],work).decode().strip(),
                'instrumentation':instrumentation[lang,shape],'samples':samples}
            if samples:
                durations=[s['elapsed_ms'] for s in samples]
                median=statistics.median(durations); mad=statistics.median(abs(s-median) for s in durations)
                record.update(median_ms=median,mad_ms=mad,
                    warnings=[msg for ok,msg in [(median<50,'process duration below 50 ms'),(mad/median>.05,'MAD exceeds 5%')] if ok])
                print(f'{lang:4} {record["shape"]:6} {median:9.3f} ms MAD {mad:.3f}; RSS {max(s["peak_process_rss_bytes"] for s in samples)} bytes')
            result['cases'].append(record)
    a.json.parent.mkdir(parents=True,exist_ok=True)
    a.json.write_text(json.dumps(result,indent=2)+'\n')
    print(f'Verified 40 edge runs, 2 sanitized C runs, and 4 instrumented workloads. Results: {a.json}')

if __name__=='__main__': main()
