#!/usr/bin/env python3
"""Check reports against emitted IR, executable counters and source locations."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import tempfile

from test_support import invoke


def symbol(name):
    return re.sub(rb'\\([0-9a-fA-F]{2})', lambda m: bytes([int(m[1], 16)]),
                  name.encode()).decode()


def ir_facts(text):
    functions = {}
    tags = Counter()
    definitions = dict(re.findall(r'^!(\d+) = !\{i64 (\d+)\}$', text, re.M))
    current = None
    for line in text.splitlines():
        if line.startswith('define '):
            name = re.search(r'@(?:"([^"]+)"|([^ (]+))\(', line)
            current = symbol(next(x for x in name.groups() if x is not None))
            functions[current] = {'calls': Counter(), 'branches': 0}
        elif line == '}':
            current = None
        elif current is not None:
            functions[current]['branches'] += bool(re.search(r'\bbr i1\b', line))
            if re.search(r'\b(call|invoke)\b', line):
                callee = re.search(r'\b(?:call|invoke)\b[^@]*@(?:"([^"]+)"|([^ (]+))\(', line)
                name = symbol(next(x for x in callee.groups() if x is not None)) if callee else '<indirect>'
                functions[current]['calls'][name] += 1
            tag = re.search(r'!wky\.site !(\d+)', line)
            if tag:
                tags[int(definitions[tag[1]])] += 1
    return functions, tags


def main():
    p = argparse.ArgumentParser()
    p.add_argument('compiler')
    compiler = str(Path(p.parse_args().compiler).resolve())
    with tempfile.TemporaryDirectory(prefix='wky-report-') as directory:
        work = Path(directory).resolve()
        lib = work / 'lib"ż.wky'
        lib.write_text('''pub fn i64 constant_raw() {
    [4]i64 values={1,2,3,4}; return values[2];
}
pub fn i64 dynamic_raw(i64 index) {
    [4]i64 values={1,2,3,4}; return values[index];
}
pub fn i64 checked_read(ref<i64> values,i64 index) { return values[index]; }
pub fn i64 stable_read(ref<i64> values,i64 index) {
    stable(values) { return values[index]; }
}
''')
        source = work / 'main.wky'
        source.write_text('''import "lib\\"ż.wky";
fn main() {
    owner<i64> owned=own(4); let values=ref_of(owned); values[2]=37;
    let before_checks=mem_metric(7); let before_pins=mem_metric(8);
    let checked=checked_read(values,2); let stable_value=stable_read(values,2);
    let checks=mem_metric(7)-before_checks; let pins=mem_metric(8)-before_pins;
    println("{checked} {constant_raw()} {stable_value} {checks} {pins}");
    return 0;
}
''')
        for level in (0, 3):
            report = work / f'report-{level}.json'
            exe = work / 'program'
            result = invoke([compiler, f'-O{level}', '--memory-metrics',
                             f'--optimization-report={report}', str(source), '-o', str(exe)], work)
            assert result.returncode == 0 and not result.stderr, result
            executed = invoke([str(exe)], work)
            assert executed.returncode == 0 and executed.stdout == b'37 3 37 1 1\n' and not executed.stderr, executed
            data = json.loads(report.read_bytes())
            assert data['schema_version'] == 1 and data['optimization_level'] == level, data
            assert data['memory_metrics_enabled'] is True and data['runtime_counts'] is None
            assert 'not proof' in data['limitations']
            functions, tags = ir_facts((work / 'output.ll').read_text())
            reported = {f['name']: f['after'] for f in data['functions'] if f['after'] is not None}
            assert reported.keys() == functions.keys(), (reported.keys(), functions.keys())
            for name, facts in functions.items():
                assert reported[name]['conditional_branches'] == facts['branches'], name
                calls = Counter({c['callee']: c['count'] for c in reported[name]['calls']})
                assert calls == facts['calls'], (name, calls, facts['calls'])
            assert {s['id']: s['after']['tagged_instructions'] for s in data['sites']
                    if s['after']['tagged_instructions']} == dict(tags)
            omitted = [s for s in data['sites'] if s['function'] == 'constant_raw'
                       and s['detail'] == 'index out of bounds']
            assert len(omitted) == 1 and omitted[0]['status'] == 'omitted_during_lowering', omitted
            dynamic = [s for s in data['sites'] if s['function'] == 'dynamic_raw'
                       and s['detail'] == 'index out of bounds']
            assert len(dynamic) == 1 and dynamic[0]['after']['conditional_branches'] >= 1, dynamic
            assert dynamic[0]['location']['file'] == str(lib) and dynamic[0]['location']['line'] == 5, dynamic
            assert dynamic[0]['location']['column'] == lib.read_text().splitlines()[4].index('index]') + 1, dynamic
            assert dynamic[0]['after']['condition_ir']
            if level == 0:
                assert all(s['before']['tagged_instructions'] == s['after']['tagged_instructions']
                           for s in data['sites'])
            else:
                accesses = [s for s in data['sites'] if s['kind'] == 'lifetime_and_bounds']
                assert any(s['status'] == 'unobserved_in_final_ir' for s in accesses), accesses
                assert all('no runtime elimination' in s['reason'] for s in accesses
                           if s['status'] == 'unobserved_in_final_ir')
            # Extra metadata must not change generated machine code.
            object_with_report = (work / 'output.o').read_bytes()
            result = invoke([compiler, f'-O{level}', '--memory-metrics', '-c', str(source)], work)
            assert result.returncode == 0 and not result.stderr, result
            assert (work / 'output.o').read_bytes() == object_with_report
        print('PASS optimization report: exact final IR calls/branches/tags, counters, imports and unchanged native object')

        for target in (source, lib, work/'output.bc', work/'./output.ll', work/'output.o', work/'program'):
            original=target.read_bytes()
            result=invoke([compiler,f'--optimization-report={target}',str(source),'-o',str(work/'program')],work)
            assert result.returncode==1 and b'conflicts with input or build artifact' in result.stderr,result
            assert target.read_bytes()==original,target
        hardlink=work/'hardlink.json'
        os.link(lib,hardlink)
        result=invoke([compiler,f'--optimization-report={hardlink}','-c',str(source)],work)
        assert result.returncode==1 and hardlink.read_bytes()==lib.read_bytes(),result
        print('PASS optimization report: source/import/artifact collisions and hardlinks rejected before writing')
        for flags in (['--optimization-report='], ['--check', '--optimization-report'],
                      ['--format', '--optimization-report']):
            before = set(work.iterdir())
            result = invoke([compiler, *flags, str(source)], work)
            assert result.returncode == 2 and set(work.iterdir()) == before, result
        result = invoke([compiler, '--optimization-report=missing/report.json', '-c', str(source)], work)
        assert result.returncode == 1 and b'cannot write optimization report' in result.stderr, result
        result = invoke([compiler, '-O3', '--optimization-report', '-c', str(source)], work)
        assert result.returncode == 0 and (work / 'optimization.json').exists(), result
        # The result has no timestamps or filesystem iteration-order fields.
        first = (work / 'optimization.json').read_bytes()
        result = invoke([compiler, '-O3', '--optimization-report', '-c', str(source)], work)
        assert result.returncode == 0 and (work / 'optimization.json').read_bytes() == first
        print('PASS optimization report: CLI validation, write errors and deterministic output')


if __name__ == '__main__':
    main()
