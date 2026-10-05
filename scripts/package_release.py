#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Package an exact source revision or an installed, CPU-portable compiler."""
import argparse
from contextlib import contextmanager
import gzip
import hashlib
import io
import json
from pathlib import Path
import platform
import re
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command, cwd=ROOT):
    return subprocess.check_output([str(x) for x in command], cwd=cwd)


def git(*args, cwd=ROOT):
    return run(['git', *args], cwd).decode().strip()


def version():
    value = (ROOT / 'VERSION').read_text().strip()
    if not re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?', value):
        raise ValueError('VERSION must contain a semantic version without build metadata')
    if '-' in value and any(p.isdigit() and len(p) > 1 and p[0] == '0'
                            for p in value.split('-', 1)[1].split('.')):
        raise ValueError('numeric prerelease identifiers cannot have leading zeroes')
    return value


def source_identity():
    if git('status', '--porcelain', '--untracked-files=no'):
        raise ValueError('release packaging requires a clean tracked working tree')
    if git('status', '--porcelain', '--untracked-files=no', cwd=ROOT / 'timbr'):
        raise ValueError('timbr has uncommitted changes')
    pinned = git('ls-tree', 'HEAD', 'timbr').split()[2]
    if git('rev-parse', 'HEAD', cwd=ROOT / 'timbr') != pinned:
        raise ValueError('timbr must match the revision pinned in the parent repository')
    return {'commit': git('rev-parse', 'HEAD'), 'timbr_commit': pinned,
            'source_date_epoch': int(git('show', '-s', '--format=%ct', 'HEAD'))}


def normalize(info, epoch):
    info.uid = info.gid = 0
    info.uname = info.gname = ''
    info.mtime = epoch
    info.pax_headers = {}
    return info


@contextmanager
def archive(path, epoch):
    # No host paths, owner names, or gzip creation times in release metadata.
    with path.open('wb') as raw:
        with gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode='w', format=tarfile.PAX_FORMAT) as output:
                yield output


def checksum(path):
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    path.with_name(path.name + '.sha256').write_text(f'{digest}  {path.name}\n')
    print(f'{path.name}: {digest}')


def package_source(output_dir, ver, identity):
    prefix = f'still-{ver}-source'
    path = output_dir / f'{prefix}.tar.gz'
    with archive(path, identity['source_date_epoch']) as output:
        for repo, base in [(ROOT, prefix + '/'), (ROOT / 'timbr', prefix + '/timbr/')]:
            data = run(['git', 'archive', '--format=tar', f'--prefix={base}', 'HEAD'], repo)
            with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as source:
                for info in source:
                    if repo == ROOT and info.name.rstrip('/') == prefix + '/timbr':
                        continue
                    payload = source.extractfile(info) if info.isfile() else None
                    output.addfile(normalize(info, identity['source_date_epoch']), payload)
        data = (json.dumps({'version': ver, **identity}, indent=2, sort_keys=True) + '\n').encode()
        info = tarfile.TarInfo(prefix + '/SOURCE.json')
        info.size = len(data)
        info.mode = 0o644
        output.addfile(normalize(info, identity['source_date_epoch']), io.BytesIO(data))
    checksum(path)


def package_binary(output_dir, build_dir, ver, identity):
    cache = {}
    for line in (build_dir / 'CMakeCache.txt').read_text().splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line and ':' in line:
            key, value = line.split('=', 1)
            cache[key.split(':', 1)[0]] = value
    if cache.get('STILL_NATIVE_CPU') != 'OFF':
        raise ValueError('release binaries require -DSTILL_NATIVE_CPU=OFF')
    if cache.get('CMAKE_BUILD_TYPE') != 'Release':
        raise ValueError('release binaries require -DCMAKE_BUILD_TYPE=Release')
    if Path(cache.get('CMAKE_HOME_DIRECTORY', '')).resolve() != ROOT:
        raise ValueError('build directory belongs to a different source checkout')
    system = {'Linux': 'linux', 'Darwin': 'macos'}.get(platform.system())
    arch = {'x86_64': 'x86_64', 'arm64': 'arm64', 'aarch64': 'arm64'}.get(platform.machine())
    if not system or not arch:
        raise ValueError('unsupported release platform')
    llvm = run([cache['LLVM_CONFIG_EXECUTABLE'], '--version']).decode().strip()
    if not llvm.startswith('21.'):
        raise ValueError('release compiler must use LLVM 21')
    prefix = f'still-{ver}-{system}-{arch}'
    path = output_dir / f'{prefix}.tar.gz'
    with tempfile.TemporaryDirectory(prefix='still-package-') as directory:
        work = Path(directory)
        stage = work / prefix
        subprocess.run(['cmake', '--install', str(build_dir), '--prefix', str(stage)], check=True)
        (stage / 'BUILD.json').write_text(json.dumps({
            'version': ver, **identity, 'platform': system, 'architecture': arch,
            'build_host': platform.platform(), 'llvm_version': llvm,
            'native_cpu': False, 'build_type': 'Release',
            'c_compiler': run([cache['CMAKE_C_COMPILER'], '--version']).decode().splitlines()[0],
            'runtime_dependencies': ['LLVM 21 shared library', 'Clang 21 on PATH',
                                     'host C library and development headers'],
        }, indent=2, sort_keys=True) + '\n')
        with archive(path, identity['source_date_epoch']) as output:
            output.add(stage, arcname=prefix,
                       filter=lambda info: normalize(info, identity['source_date_epoch']))
        # Execute the actual unpacked archive, not the build-tree executable.
        unpacked = work / 'unpacked'
        with tarfile.open(path, 'r:gz') as packaged:
            for info in packaged:
                if Path(info.name).is_absolute() or '..' in Path(info.name).parts or not (info.isfile() or info.isdir()):
                    raise ValueError('unexpected unsafe archive entry')
            packaged.extractall(unpacked)
        compiler = unpacked / prefix / 'bin/still'
        if run([compiler, '--version'], work) != f'still {ver}\n'.encode():
            raise ValueError('installed compiler version does not match VERSION')
        subprocess.run([str(compiler), '-O3', str(ROOT / 'examples/hello.wky'),
                        '-o', str(work / 'hello')], cwd=work, check=True)
        if run([work / 'hello'], work) != b'42\n':
            raise ValueError('packaged compiler failed its managed-memory/printing smoke test')
    checksum(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', action='store_true')
    parser.add_argument('--binary', action='store_true')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-ci')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    args = parser.parse_args()
    if not (args.source or args.binary):
        parser.error('select --source, --binary, or both')
    try:
        ver = version()
        identity = source_identity()
        if git('show', 'HEAD:VERSION') != ver:
            raise ValueError('VERSION must match the packaged source revision')
        args.output.mkdir(parents=True, exist_ok=True)
        if args.source:
            package_source(args.output.resolve(), ver, identity)
        if args.binary:
            package_binary(args.output.resolve(), args.build_dir.resolve(), ver, identity)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'packaging failed: {error}\n')


if __name__ == '__main__':
    main()
