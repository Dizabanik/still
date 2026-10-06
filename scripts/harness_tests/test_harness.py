import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_support import classify, environment, invoke, load_case, _check_execution, _check_variant
from bench import checked_output, schedule, summary, benchmark, build_commands, ROOT
from bench_oracles import oracle, semantics_oracle
from generate_property_cases import document


class HarnessTests(unittest.TestCase):
    def test_expected_failure_is_narrow(self):
        expected = {'phase':'output','pattern':'actual=1','reason':'alias bug'}
        self.assertEqual(classify('output','actual=1',expected,2)[0],'XFAIL')
        for phase in ['compiler_crash','timeout','compile','exit']:
            self.assertEqual(classify(phase,'actual=1',expected,2)[0],'FAIL')
        self.assertEqual(classify('output','actual=99',expected,2)[0],'FAIL')
        self.assertEqual(classify(None,'',expected,2)[0],'XPASS')

    def test_xfail_only_applies_to_listed_optimizations(self):
        expected = {'optimizations':[2,3],'phase':'output','reason':'alias bug'}
        self.assertEqual(classify(None,'',expected,0)[0],'PASS')
        self.assertEqual(classify('output','bad',expected,0)[0],'FAIL')

    def test_optimizer_environment_is_not_poisoned(self):
        with patch.dict(os.environ, {'STILL_NO_OPT':'1','STILL_NO_PRINTF':'1'}):
            env=environment()
            self.assertNotIn('STILL_NO_OPT',env)
            self.assertNotIn('STILL_NO_PRINTF',env)
            self.assertEqual(env['LC_ALL'],'C')

    def test_samples_are_balanced_and_repeatable(self):
        order=schedule(['wky','c','rust'],9,random.Random(42))
        self.assertEqual(order,schedule(['wky','c','rust'],9,random.Random(42)))
        self.assertTrue(all(sorted(r)==['c','rust','wky'] for r in order))
        self.assertGreater(len({tuple(r) for r in order}),1)

    def test_median_not_best_of(self):
        result=summary([0.01,0.10,0.11,0.12,2.0])
        self.assertEqual(result['median_seconds'],0.11)
        self.assertAlmostEqual(result['mad_seconds'],0.01)

    def test_incorrect_output_never_qualifies_for_timing(self):
        for result in [subprocess.CompletedProcess([],0,b'wrong',b''),
                       subprocess.CompletedProcess([],1,b'expected',b''),
                       subprocess.CompletedProcess([],0,b'expected',b'warning')]:
            with self.assertRaises(RuntimeError): checked_output(result,b'expected')
        self.assertEqual(checked_output(subprocess.CompletedProcess([],0,b'expected',b''),b'expected'),b'expected')

    def test_bad_benchmark_output_stops_before_sampling(self):
        entry=json.loads((ROOT/'bench/manifest.json').read_text())['benchmarks'][0]
        def fake_build(entry,compilers,folder):
            for lang in compilers: (folder/lang).write_bytes(b'fake executable')
            return {lang:['build',lang] for lang in compilers}
        def fake_invoke(command,*args,**kwargs):
            output=b'' if command[0]=='build' else b'incorrect\n'
            return subprocess.CompletedProcess(command,0,output,b'')
        with tempfile.TemporaryDirectory() as temp, patch('bench.build_commands',side_effect=fake_build), patch('bench.invoke',side_effect=fake_invoke) as runner:
            row=benchmark(entry,dict(wky='still',c='clang',rust='rustc'),
                          SimpleNamespace(compile_timeout=1,timeout=1),Path(temp),random.Random(1))
            self.assertEqual(row['status'],'failed')
            self.assertNotIn('timings',row)
            self.assertNotIn('sample_order',row)
            self.assertNotIn('ratios',row)
            self.assertEqual(runner.call_count,4)  # three builds, first incorrect verification

    def test_missing_source_is_a_reported_failure(self):
        entry={'name':'missing','suite':'matched','contract':'missing fixture','sources':{'c':'does-not-exist.c'}}
        with tempfile.TemporaryDirectory() as temp:
            row=benchmark(entry,{},SimpleNamespace(),Path(temp),random.Random(1))
            self.assertEqual(row['status'],'failed')
            self.assertIn('error',row)

    def test_builds_use_sources_and_explicit_safety_flags(self):
        entry=json.loads((ROOT/'bench/manifest.json').read_text())['benchmarks'][0]
        commands=build_commands(entry,dict(wky='still',c='clang',rust='rustc'),Path('/tmp/work'))
        self.assertIn('--bounds-check=safe',commands['wky'])
        self.assertIn('-O3',commands['wky'])
        self.assertIn('-O3',commands['c'])
        for lang,extension in [('wky','.wky'),('c','.c'),('rust','.rs')]:
            self.assertTrue(any(arg.endswith(extension) for arg in commands[lang]))

    def test_byte_exact_output(self):
        with self.assertRaises(RuntimeError):
            checked_output(subprocess.CompletedProcess([],0,b'line\r\n',b''),b'line\n')

    def test_float_comparison_is_explicit_and_bounded(self):
        policy={'kind':'numeric','pattern':r'value=(\S+)\n','abs_tolerance':0.000002,'rel_tolerance':0}
        result=lambda value: subprocess.CompletedProcess([],0,value,b'')
        checked_output(result(b'value=1.000001\n'),b'value=1.000000\n',policy)
        for actual in [b'value=1.001\n',b'value=nan\n',b'value=inf\n',b'wrong=1.0\n']:
            with self.assertRaises(RuntimeError): checked_output(result(actual),b'value=1.000000\n',policy)
        with self.assertRaises(RuntimeError): checked_output(result(b'value=nan\n'),b'value=nan\n',policy)

    def test_rejection_requires_the_intended_diagnostic(self):
        spec={'kind':'reject','flags':[],'diagnostic':r'error\[E0004\]'}
        with tempfile.TemporaryDirectory() as temp:
            work=Path(temp)
            for code,message,phase in [(0,b'', 'acceptance'),
                                       (1,b'error[E0001] parser error','diagnostic'),
                                       (-11,b'', 'compiler_crash'),
                                       (1,b'FATAL: bad compiler','compiler_crash'),
                                       (1,b'error[E0004] arity mismatch',None)]:
                with patch('test_support.invoke',return_value=subprocess.CompletedProcess([],code,b'',message)):
                    self.assertEqual(_check_variant(work/'case.wky','compiler',spec,0,work)[0],phase)

    def test_trap_requires_both_signature_and_exit_kind(self):
        spec={'kind':'trap','diagnostic':'Whisky: trap: bounds'}
        with tempfile.TemporaryDirectory() as temp:
            work=Path(temp)
            for code,message,phase in [(-11,b'Whisky: trap: bounds','trap'),
                                       (-6,b'assertion failed','trap'),
                                       (0,b'Whisky: trap: bounds','trap'),
                                       (-6,b'Whisky: trap: bounds',None)]:
                with patch('test_support.invoke',return_value=subprocess.CompletedProcess([],code,b'',message)):
                    self.assertEqual(_check_execution(work/'case.wky',work/'program',spec,work)[0],phase)
            spec['exit_codes']=[-11]
            with patch('test_support.invoke',return_value=subprocess.CompletedProcess([],-11,b'',b'Whisky: trap: bounds')):
                self.assertEqual(_check_execution(work/'case.wky',work/'program',spec,work)[0],'trap')

    def test_runtime_exit_and_stderr_are_observed(self):
        spec={'kind':'run','stdout':'ok\n'}
        with tempfile.TemporaryDirectory() as temp:
            work=Path(temp)
            for code,out,err,phase in [(7,b'ok\n',b'','exit'),(0,b'ok\n',b'bad','stderr'),
                                       (0,b'wrong\n',b'','output'),(0,b'ok\n',b'',None)]:
                with patch('test_support.invoke',return_value=subprocess.CompletedProcess([],code,out,err)):
                    self.assertEqual(_check_execution(work/'case.wky',work/'program',spec,work)[0],phase)

    def test_property_vectors_are_current(self):
        self.assertEqual(json.loads((ROOT/'tests/contracts/runtime_integer_stream.wky.json').read_text()),document())

    def test_timeout(self):
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaises(subprocess.TimeoutExpired):
                invoke([sys.executable,'-c','import time; time.sleep(10)'],work,timeout=.05)

    def test_oracle_vectors(self):
        self.assertEqual(oracle('runtime_mix',[1,1]),b'31\n')
        self.assertEqual(oracle('runtime_mix',[2,1]),b'960\n')
        self.assertEqual(oracle('dynamic_gather',[1,1,1]),b'3 1103527590\n')
        self.assertEqual(oracle('overlap_views',[2,1,2]),b'14 9\n')
        self.assertEqual(oracle('indexed_graph',[1,1,1]),b'1 0\n')
        self.assertEqual(oracle('matrix4',[0,1]),b'0 1 4\n')
        self.assertEqual(oracle('ring_pipeline',[65,1]),str(sum((i+1)*27+13 for i in range(65))).encode()+b'\n')

    def test_semantics_oracles_against_enumerated_specification(self):
        for rounds in (0,1,2,16,97,98,257):
            for seed in (0,1,31,65535):
                for period in (1,3,17,256):
                    data=list(range(seed,seed+rounds))
                    failures=[x for x in data if x%period==0]
                    result=sum(x if x%period==0 else x*17+14 for x in data)
                    owned=sum(x*4+(3 if x%period else 0) for x in data)
                    queued=sum(x*(i%97+7) for i,x in enumerate(data))
                    for name,checksum,counted in (
                            ('typed_result',result,len(failures)),
                            ('owned_enum',owned,rounds-len(failures)),
                            ('owned_channel',queued,rounds)):
                        actual=semantics_oracle(name,rounds,seed,period,True)
                        self.assertEqual(actual[:2],[checksum,counted])
                        self.assertEqual(actual[2],actual[3])
                        self.assertEqual(actual[4],0)

    def test_manifest_coverage(self):
        manifest=json.loads((ROOT/'bench/manifest.json').read_text())['benchmarks']
        self.assertEqual(len(manifest),len({e['name'] for e in manifest}))
        registered=set()
        for entry in manifest:
            self.assertEqual(set(entry['sources']),{'wky','c','rust'})
            self.assertTrue(entry['contract'])
            for path in entry['sources'].values():
                self.assertTrue((ROOT/'bench'/path).is_file(),path)
                registered.add(path)
            if entry['suite']=='matched':
                self.assertGreaterEqual(len(entry['cases']),8)
                self.assertIn(0,[case[0] for case in entry['cases']])
                for case in entry['cases']: oracle(entry['oracle'],case)
        for path in (ROOT/'bench').glob('*.wky'):
            if path.with_suffix('.c').exists() and path.with_suffix('.rs').exists():
                self.assertIn(path.name,registered)

    def test_future_contracts_are_explicitly_pending_and_linked(self):
        future=json.loads((ROOT/'tests/future-contracts.json').read_text())['features']
        benches=json.loads((ROOT/'bench/future-contracts.json').read_text())['benchmarks']
        names={b['name'] for b in benches}
        ids=[]
        for feature in future:
            self.assertEqual(feature['status'],'planned')
            self.assertIn(feature['benchmark'],names)
            for test in feature['tests']:
                ids.append(test['id'])
                for field in ['arrange','action','expect']: self.assertTrue(test[field])
        self.assertEqual(len(ids),len(set(ids)))
        self.assertGreaterEqual(len(ids),50)

    def test_no_orphan_compiler_tests(self):
        for path in (ROOT/'tests').rglob('*.wky'):
            if 'lib' not in path.relative_to(ROOT/'tests').parts:
                self.assertIsNotNone(load_case(path),str(path))


if __name__=='__main__': unittest.main()
