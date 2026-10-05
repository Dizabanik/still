#!/usr/bin/env python3
"""Exercise the renamed CLI, runtime namespace, and compiler metadata."""
import argparse
import json
from pathlib import Path
import re
import tempfile

from test_support import environment, invoke


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler')
    compiler = str(Path(parser.parse_args().compiler).resolve())
    version = (Path(__file__).resolve().parents[1] / 'VERSION').read_text().strip()
    with tempfile.TemporaryDirectory(prefix='still-branding-') as directory:
        work = Path(directory).resolve()

        def run(*arguments, status=0, env=None):
            result = invoke([compiler, *map(str, arguments)], work, env=env)
            assert result.returncode == status, (arguments, result)
            return result

        result = run('--version')
        assert result.stdout == f'still {version}\n'.encode() and not result.stderr, result
        result = run('--help')
        assert b'Whisky compiler' in result.stdout and b'<source.wky>' in result.stdout, result
        result = run('--invalid-option', status=2)
        assert result.stderr.startswith(b'still: unknown option'), result

        project = work / 'project.with.dots'
        project.mkdir()
        library = project / 'library.wky'
        library.write_text('pub fn i64 wky_answer(){return 41;}\n')
        source = project / 'entry.test.wky'
        source.write_text('''import "library.wky";
fn i64 still_answer(){return 1;}
fn main(){
    let values:owner<i64>=own(2);
    values[0]=wky_answer(); values[1]=still_answer();
    let view=ref_of(values);
    stable(view){println("{view[0]+view[1]}");}
    return 0;
}
''')
        executable = source.with_suffix('')
        result = run('-O0', '-g', '--optimization-report=report.json', source)
        assert result.stdout == f"[Whisky] Built '{executable}'\n".encode() and not result.stderr, result
        result = invoke([str(executable)], work)
        assert result.returncode == 0 and result.stdout == b'42\n' and not result.stderr, result
        ir = (work / 'output.ll').read_text()
        assert f'producer: "still {version}"' in ir, ir[-5000:]
        assert '"wky.source"' in ir and '%struct.WkyDescriptor' in ir, ir[:5000]
        runtime = re.findall(r'^define ([^\n]*@__wky_[^\n]*)', ir, re.M)
        assert runtime and all(row.startswith('internal ') for row in runtime), runtime
        assert not re.search(r'[@%]"?(?:__)?kawa[_\.]', ir), ir[:5000]
        report = json.loads((work / 'report.json').read_bytes())
        functions = {f['name']: f for f in report['functions']}
        assert functions['wky_answer']['role'] == 'source', functions['wky_answer']
        assert functions['still_answer']['role'] == 'source', functions['still_answer']

        # Compiler controls must reach optimization, not just the CLI parser.
        run('-O3', '-c', '--optimization-report=disabled.json', source,
            env=environment({'STILL_NO_OPT': '1'}))
        disabled = json.loads((work / 'disabled.json').read_bytes())
        assert disabled['pipeline'] == 'disabled by STILL_NO_OPT', disabled

        # A shorter prefix must still reject every reserved backend declaration.
        for name in ('__wky_', '__wky_x', '__wky_mem_address',
                     'wky_main', 'wky_globals_init', 'wky_trap'):
            source.write_text(f'fn i32 {name}(){{return 0;}} fn main(){{return 0;}}\n')
            result = run('--check', '--diagnostic-format=json', source, status=1)
            diagnostic = json.loads(result.stderr.splitlines()[0])
            assert diagnostic['code'] == 'E0003' and 'reserved' in diagnostic['message'], diagnostic
        print('PASS Whisky/still integration: CLI, .wky imports/default output, runtime linkage, '
              'debug producer, source metadata, compiler controls and reserved symbols')


if __name__ == '__main__':
    main()
