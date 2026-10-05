#!/usr/bin/env python3
"""Equal-policy managed reference timings with separate instrumentation runs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import sys
import tempfile

from bench_process import run_measured

ROOT=Path(__file__).resolve().parents[1]
UNAVAILABLE=2**64-1

def invoke(command, work, extra_env=None):
    env=dict(os.environ)
    if extra_env: env.update(extra_env)
    for key in ('STILL_NO_OPT','STILL_NO_PRINTF','STILL_DUMP_BAD'):
        env.pop(key,None)
    result=subprocess.run([str(x) for x in command],cwd=work,env=env,capture_output=True,timeout=120)
    if result.returncode or result.stderr:
        raise RuntimeError(f'{command}\n{result.returncode}: {result.stdout.decode()} {result.stderr.decode()}')
    return result.stdout

def oracle(rounds,seed,size,scatter,workload):
    # Both index sequences are permutations over a power-of-two period. Build
    # that period independently, then sum full periods and the remainder.
    q,r=divmod(rounds,size)
    slope,bias=(36,seed*2+3) if workload=='subobject_walk' else (17,seed)
    prefix=sum((((i*(1664525 if scatter else 1)+seed)%size)*slope+bias) for i in range(r))
    return q*(slope*size*(size-1)//2+bias*size)+prefix

def verify(output,args,instrumented,workload):
    values=[int(x) for x in output.split()]
    n,seed,size,mode,scatter=args
    views=workload=='subobject_walk'
    assert len(values)==9+views and values[0]==oracle(n,seed,size,scatter,workload), (args,values)
    assert values[1:4]==[1,1,0] and values[8]>0,(args,values)
    if instrumented:
        assert values[5]-values[4]==(n*(4 if views else 1) if mode==0 else 0),(args,values)
        assert values[7]-values[6]==(2*n+1 if views and mode else mode),(args,values)
        if views: assert values[9]==2*n,(args,values)
    else:
        assert values[4:8]==[UNAVAILABLE]*4,(args,values)
        if views: assert values[9]==UNAVAILABLE,(args,values)
    return values

def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--still',default=str(ROOT/'build-cmake/still'))
    p.add_argument('--clang',default=str(Path(invoke(['llvm-config','--bindir'],ROOT).decode().strip())/'clang'))
    p.add_argument('--rounds',type=int,default=50000000)
    p.add_argument('--samples',type=int,default=7)
    p.add_argument('--warmups',type=int,default=2)
    p.add_argument('--size',type=int,default=65536)
    p.add_argument('--seed',type=int,default=12345)
    p.add_argument('--workload',choices=('reference_walk','subobject_walk'),default='reference_walk')
    p.add_argument('--verify-only',action='store_true')
    p.add_argument('--sanitize-c',action='store_true',help='separate untimed ASan/UBSan verification')
    p.add_argument('--sanitizer-clang',default='/usr/bin/clang' if sys.platform=='darwin' else None)
    p.add_argument('--json',type=Path)
    a=p.parse_args()
    if a.json is None: a.json=ROOT/f'build/{"subobject" if a.workload=="subobject_walk" else "memory"}-benchmark.json'
    sanitizer_clang=a.sanitizer_clang or a.clang
    if not (0<=a.rounds<=10**9 and 0<=a.seed<=65535 and 1<=a.size<=1048576 and
            a.size&(a.size-1)==0 and a.samples>=3 and a.warmups>=1):
        p.error('invalid workload or insufficient sampling')
    compiler=Path(shutil.which(a.still) or a.still).resolve()
    sources={lang:ROOT/f'bench/managed/{a.workload}.{ext}' for lang,ext in [('wky','wky'),('c','c')]}
    result={'contract':'Equal descriptor runtime and safety; process timing includes initialization and cleanup.',
            'workload':a.workload,'timing_counters':False,'platform':platform.platform(),'machine':platform.machine(),
            'compiler_sha256':digest(compiler),'sources':{l:digest(s) for l,s in sources.items()},
            'runtime_sha256':digest(ROOT/'src/runtime/wky_memory.c'),
            'runtime_header_sha256':digest(ROOT/'src/runtime/wky_memory.h'),
            'clang_version':invoke([a.clang,'--version'],ROOT).decode().splitlines()[0],
            'sdkroot':os.environ.get('SDKROOT'),'cases':[],'samples':a.samples,'warmups':a.warmups}
    if a.sanitize_c:
        result['sanitizer_clang_version']=invoke([sanitizer_clang,'--version'],ROOT).decode().splitlines()[0]
    sanitized_runs=0
    with tempfile.TemporaryDirectory(prefix='wky-memory-bench-') as d:
        work=Path(d); bins={}
        for lang in sources:
            for metrics in (False,True):
                exe=work/f'{lang}-{int(metrics)}'
                cmd=[compiler,'-O3',*(['--memory-metrics'] if metrics else []),sources[lang],'-o',exe] if lang=='wky' else [
                    a.clang,'-O3','-march=native','-std=c11',*(['-DWKY_MEMORY_METRICS'] if metrics else []),sources[lang],'-o',exe,'-pthread']
                invoke(cmd,work); bins[lang,metrics]=exe
        for n,seed,size in [(0,0,1),(1,1,1),(17,7,8),(65,65535,64),(1025,42,1024)]:
            for mode in (0,1):
                for scatter in (0,1):
                    args=[n,seed,size,mode,scatter]
                    for (lang,metrics),exe in bins.items():
                        verify(invoke([exe,*map(str,args)],work),args,metrics,a.workload)
        timings={(lang,mode,scatter):[] for lang in sources for mode in (0,1) for scatter in (0,1)}
        instrumentation={}
        for lang,mode,scatter in timings:
            args=[a.rounds,a.seed,a.size,mode,scatter]
            values=verify(invoke([bins[lang,True],*map(str,args)],work),args,True,a.workload)
            instrumentation[lang,mode,scatter]={'checks':values[5]-values[4],'pins':values[7]-values[6],
                'allocations':values[1],'frees':values[2],'live_payload_bytes':values[3],'descriptor_bytes':values[8],
                'reference_bytes':32,'payload_peak_bytes':a.size*(32 if a.workload=='subobject_walk' else 8)}
            if a.workload=='subobject_walk': instrumentation[lang,mode,scatter]['subobject_views']=values[9]
        for mode in (0,1):
            for scatter in (0,1):
                assert instrumentation['wky',mode,scatter]==instrumentation['c',mode,scatter],('different policies/layout/counters',mode,scatter)
        if a.sanitize_c:
            exe=work/'c-sanitized'
            invoke([sanitizer_clang,'-O1','-g','-std=c11','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer',sources['c'],'-o',exe,'-pthread'],work)
            for mode in (0,1):
                args=[1025,42,1024,mode,1]
                output=invoke([exe,*map(str,args)],work,
                    {'ASAN_OPTIONS':f'detect_leaks={int(sys.platform!="darwin")}:abort_on_error=1'})
                verify(output,args,False,a.workload)
                sanitized_runs+=1
        if not a.verify_only:
            rng=random.Random(729104)
            for round_index in range(a.warmups+a.samples):
                order=list(timings); rng.shuffle(order)
                for lang,mode,scatter in order:
                    args=[a.rounds,a.seed,a.size,mode,scatter]
                    output,measurement=run_measured([bins[lang,False],*map(str,args)],work)
                    verify(output,args,False,a.workload)
                    if round_index>=a.warmups: timings[lang,mode,scatter].append(measurement)
        for (lang,mode,scatter),samples in timings.items():
            record={'language':lang,'access':'stable' if mode else 'checked','pattern':'scatter' if scatter else 'sequential',
                'args':[a.rounds,a.seed,a.size,mode,scatter],'binary_sha256':digest(bins[lang,False]),
                'binary_file_bytes':bins[lang,False].stat().st_size,
                'llvm_size_berkeley':invoke(['llvm-size','--format=berkeley',bins[lang,False]],work).decode().strip(),
                'instrumentation':instrumentation[lang,mode,scatter],'samples':samples}
            if samples:
                durations=[s['elapsed_ms'] for s in samples]
                median=statistics.median(durations); mad=statistics.median(abs(v-median) for v in durations)
                record.update(median_ms=median,mad_ms=mad,ns_per_access_including_process=median*1e6/max(1,a.rounds),
                    warnings=[msg for ok,msg in [(median<50,'process duration below 50 ms'),(mad/median>.05,'MAD exceeds 5%')] if ok])
                print(f'{lang:4} {record["access"]:7} {record["pattern"]:10} {median:9.3f} ms MAD {mad:.3f}')
            result['cases'].append(record)
    result['sanitized_runs']=sanitized_runs
    a.json.parent.mkdir(parents=True,exist_ok=True)
    a.json.write_text(json.dumps(result,indent=2)+'\n')
    print(f'Verified 80 edge-case runs, 8 instrumented workloads and {sanitized_runs} sanitized C runs. Results: {a.json}')

if __name__=='__main__': main()
