#!/usr/bin/env python3
"""Exercise compiler tools against source, executable and byte-level oracles."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import tempfile

from test_support import environment, invoke


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    args = parser.parse_args()
    compiler = str(Path(args.compiler).resolve())
    repository = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='kawa-tools-') as directory:
        root = Path(directory).resolve()

        def run(*arguments, status=0, cwd=root):
            result = invoke([compiler, *map(str, arguments)], cwd)
            assert result.returncode == status, (arguments, result)
            return result

        source = root / 'input.kawa'
        raw = r'''import stdc;
// import "missing-comment.kawa"; untouched comment ż
#[nocapture] fn i32 read(i32* p){return *p;}
fn main(){let text="import \"missing-string.kawa\"; ż\x00Z";
i32 x=41; stdc.printf("%d %d %d\n",read(&x),text.len,(i32)'\x41');
if(x==41){println("yes");}else{println("no");}return 0;}
'''.encode()
        source.write_bytes(raw)
        before = set(root.iterdir())
        formatted = run('--format', source).stdout
        assert set(root.iterdir()) == before, 'formatting emitted build artifacts'
        for spelling in ('// import "missing-comment.kawa"; untouched comment ż'.encode(),
                         r'"import \"missing-string.kawa\"; ż\x00Z"'.encode(), br"'\x41'"):
            assert spelling in formatted, (spelling, formatted)
        target = root / 'formatted.kawa'
        target.write_bytes(formatted)
        assert run('--format', target).stdout == formatted, 'formatter is not idempotent'
        run('--format-check', source, status=1)
        run('--format-check', target)
        for opt in (0, 3):
            outputs = []
            for path in (source, target):
                exe = root / 'program'
                compiled=run(f'-O{opt}', path, '-o', exe)
                assert not compiled.stderr,compiled
                executed = invoke([str(exe)], root)
                assert executed.returncode == 0 and not executed.stderr, executed
                outputs.append(executed.stdout)
            # Length is a byte count, including NUL and both bytes of the UTF-8 character.
            assert outputs == [b'41 34 65\nyes\n'] * 2, outputs
        for case in ('fn main( {', 'fn main(){ let s="unterminated; }',
                     'fn main(){ let s="escape' + '\\', "fn main(){ let c='",
                     "fn main(){ let c='" + '\\', '#[unfinished'):
            bad = root / 'syntax.kawa'
            bad.write_text(case)
            before = set(root.iterdir())
            result = run('--format', '--diagnostic-format=json', bad, status=1)
            assert not result.stdout and set(root.iterdir()) == before, result
            errors = [json.loads(line) for line in result.stderr.splitlines()]
            assert errors and errors[0]['code'] == 'E0001', errors
        print('PASS formatter: idempotence, literals/comments, independent executable oracle and syntax errors')

        # Representative language constructs, including generics, enums, managed
        # access, macros and coroutine syntax. This is syntax validation only.
        for relative in ('tests/test_struct_defaults.kawa', 'tests/contracts/named_evaluation.kawa',
                         'tests/managed/alias_move_clone.kawa', 'tests/test_generics.kawa',
                         'tests/ir/fp_permissions.kawa', 'tests/numeric/comptime_width.kawa'):
            path = repository / relative
            if not path.exists():
                continue
            canonical = run('--format', path).stdout
            target.write_bytes(canonical)
            assert run('--format', target).stdout == canonical, relative
        print('PASS formatter: representative syntax round trips')

        source.write_text('fn main(){ println("ż"); let a:owner<i64>=own(1); release(a); println("{a[0]}"); return 0; }')
        result = run('--check', '--diagnostic-format=json', source, status=1)
        diagnostic = json.loads(result.stderr.splitlines()[0])
        location = diagnostic['location']
        expected_column = source.read_bytes().index(b'a[0]') + 1
        assert diagnostic['code'] == 'E0010', diagnostic
        assert location['file'] == str(source) and location['line'] == 1, location
        assert location['column'] == expected_column and location['length'] == 1, location
        assert location['column_unit'] == 'byte', location
        assert location['source_line'] == source.read_text(), location

        library = root / 'lib.kawa'
        library.write_text('pub fn i32 value(){\n    return missing;\n}\n')
        source.write_text('// import "ignored.kawa";\nimport "lib.kawa";\nfn main(){return value();}\n')
        diagnostic = json.loads(run('--check', '--diagnostic-format=json', source, status=1).stderr.splitlines()[0])
        assert diagnostic['code'] == 'E0002', diagnostic
        assert diagnostic['location']['file'] == str(library), diagnostic
        assert diagnostic['location']['line'] == 2, diagnostic
        library.write_text('pub fn i32 value(){return 42;}\n')
        source.write_text('import "lib.kawa";\n\nfn main(){\n    return missing;\n}\n')
        diagnostic = json.loads(run('--check', '--diagnostic-format=json', source, status=1).stderr.splitlines()[0])
        assert diagnostic['location']['file'] == str(source) and diagnostic['location']['line'] == 4, diagnostic
        source.write_bytes(b'// non-UTF8 comment: \xff\nfn main(){return missing;}\n')
        records = [json.loads(line) for line in run('--check', '--diagnostic-format=json', source, status=1).stderr.splitlines()]
        assert records[0]['code'] == 'E0002', records
        source.write_bytes(b'fn main(){println("\xff");return missing;}\n')
        records = [json.loads(line) for line in run('--check', '--diagnostic-format=json', source, status=1).stderr.splitlines()]
        assert records[0]['code'] == 'E0002', records
        quoted = root / 'quote"and\\slash.kawa'
        quoted.write_text('pub fn i32 quoted(){return missing;}\n')
        source.write_text('import "quote\\"and\\\\slash.kawa"; fn main(){return quoted();}\n')
        diagnostic = json.loads(run('--check', '--diagnostic-format=json', source, status=1).stderr.splitlines()[0])
        assert diagnostic['location']['file'] == str(quoted), diagnostic
        quoted.write_text('pub fn i32 quoted(){return 42;}\n')
        source.write_text('import "quote\\"and\\\\slash.kawa"; fn main(){return missing;}\n')
        diagnostic = json.loads(run('--check', '--diagnostic-format=json', source, status=1).stderr.splitlines()[0])
        assert diagnostic['location']['line'] == 1, diagnostic
        assert diagnostic['location']['column'] == source.read_bytes().index(b'missing')+1, diagnostic
        source.write_text('fn main(){return 0; println("unreachable");}\n')
        records = [json.loads(line) for line in run('--check', '--diagnostic-format=json', source).stderr.splitlines()]
        assert any(d['code'] == 'W0011' and d['level'] == 'warning' for d in records), records
        for code in ('E0001', 'E0010', 'W0011', 'W0012'):
            explained = json.loads(run('--diagnostic-format=json', '--explain', code).stdout)
            assert explained['code'] == code and explained['explanation'], explained
        for code in ('E-001', 'E0000', 'E9999', 'W0001', 'E0011'):
            run('--explain', code, status=2)
        print('PASS diagnostics: UTF-8 byte spans, imports, warnings, JSON and explain codes')

        (root / 'sub').mkdir()
        library.write_text('import "sub/../input.kawa";\npub fn i32 value(){return 42;}\n')
        (root / 'left.kawa').write_text('import "./lib.kawa";\n')
        (root / 'right.kawa').write_text('import "sub/../lib.kawa";\n')
        source.write_text('import "left.kawa";\nimport "right.kawa";\nfn main(){println("{value()}");return 0;}\n')
        run('--check', source)
        # The executable name is literal argv data, even with shell metacharacters.
        exe = root / "program';touch INJECTED;#"
        compiled=run(source, '-o', exe)
        assert not compiled.stderr,compiled
        assert not (root / 'INJECTED').exists()
        executed = invoke([str(exe)], root)
        assert executed.returncode == 0 and executed.stdout == b'42\n' and not executed.stderr, executed
        print('PASS driver: canonical cyclic/diamond imports and literal linker arguments')

        library.write_text('pub fn i32 divide(i32 divisor){\n    return 42/divisor;\n}\n')
        source.write_text('import "lib.kawa";\nfn main(){return divide(0);}\n')
        for opt in (0, 3):
            run(f'-O{opt}', '-g', source, '-o', root / 'trap')
            trapped = invoke([str(root / 'trap')], root)
            assert trapped.returncode == -6, trapped
            assert f'invalid integer division at {library}:2'.encode() in trapped.stderr, trapped
        print('PASS source locations: imported runtime traps and debug builds')

        # Pin the source path, target, flags and compiler; vary only output cwd.
        source.write_text('fn main(){let a:owner<i64>=own(4);a[2]=7;println("{a[2]}");return 0;}\n')
        for debug in (False, True):
            builds = []
            for number in (1, 2):
                work = root / f'build-{debug}-{number}'
                work.mkdir()
                result = run('-O3', '-c', '--emit-hash', *(['-g'] if debug else []), source, cwd=work)
                artifacts = {name: (work / name).read_bytes() for name in ('output.bc', 'output.ll', 'output.o')}
                digest = hashlib.sha256(artifacts['output.bc']).hexdigest()
                assert digest.encode() in result.stdout, (digest, result.stdout)
                builds.append(artifacts)
            assert builds[0] == builds[1], f'artifacts depend on output cwd (debug={debug})'
        print('PASS reproducibility: bitcode, IR, native object and independent SHA-256 oracle')
        if platform.system()=='Darwin':
            deployment=environment({'MACOSX_DEPLOYMENT_TARGET':'13.0'})
            result=invoke([compiler,'-O3',str(source),'-o',str(root/'deployment')],root,env=deployment)
            assert result.returncode==0 and not result.stderr,result
            assert 'apple-macosx13.0.0' in (root/'output.ll').read_text()
            for version in ('27x','27.0.bad','27.0.1.2','-1','27.',''):
                result=invoke([compiler,'--check',str(source)],root,
                    env=environment({'MACOSX_DEPLOYMENT_TARGET':version}))
                assert result.returncode==1 and b'invalid macOS deployment version' in result.stderr,result
            print('PASS native target: matching linker deployment and validated override')


if __name__ == '__main__':
    main()
