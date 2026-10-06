#!/usr/bin/env python3
"""Check exact formatting bytes while ASan/UBSan check the print runtime."""
import argparse
import os
import sys

from test_support import environment, invoke

parser = argparse.ArgumentParser()
parser.add_argument('binary')
args = parser.parse_args()
minimum = str(-(1 << 63))
expected = {
    'integers': ('\n'.join((minimum, minimum.zfill(22), minimum.rjust(22))) + '\n').encode(),
    'padding': b'p' * 65530 + b'\n',
    'floats': b'',
    'bounded': b'x' * 131079 + b'\na\0b\nxx\n',
}
for width in (0, 1, 65535, 65536, 65537, 131073):
    for value in (7, -7):
        text = str(value)
        for fill in (' ', '0'):
            expected['padding'] += ((text.zfill(width) if fill == '0' else text.rjust(width)) + '\n').encode()
for precision in (0, 2, 6, 63, 64, 100, 4096):
    for value in (1.25, -1.25, sys.float_info.max, -0.0, 0.125, 0.375):
        expected['floats'] += (format(value, f'.{precision}f') + '\n').encode()
env = environment({'ASAN_OPTIONS': f'detect_leaks={int(sys.platform != "darwin")}:abort_on_error=1',
                   'UBSAN_OPTIONS': 'halt_on_error=1'})
for case, output in expected.items():
    result = invoke([os.path.abspath(args.binary), case], os.getcwd(), env=env)
    assert result.returncode == 0 and not result.stderr, (case, result)
    assert result.stdout == output, (case, len(result.stdout), len(output))
    print('PASS', case)
