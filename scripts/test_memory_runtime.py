#!/usr/bin/env python3
"""Require a checked abort, never a segmentation fault, for each bad access."""
import argparse
import os
import signal
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('binary')
a = p.parse_args()
cases = {'valid': None, 'value': None,
         'value_drop_pinned': 'invalidation during stable',
         'value_store_pinned': 'invalidation during stable',
         'value_take_pinned': 'invalidation during stable',
         'value_duplicate': 'owner must be moved',
         'value_stale': 'stale reference',
         'nested': None, 'nested_pinned': 'invalidation during stable',
         'nested_replace_pinned': 'invalidation during stable',
         'nested_parent_pinned': 'invalidation during stable',
         'nested_resize_pinned': 'invalidation during stable',
         'nested_cycle': 'ownership cycle', 'nested_remove': 'remove requires an arena',
         'nested_stale': 'stale reference', 'nested_write_stale': 'stale reference',
         'bounds': 'index out of bounds',
         'view_bounds': 'index out of bounds',
         'view_outside': 'managed slot outside reference bounds',
         'view_stale': 'stale reference',
         'overflow': 'index out of bounds', 'negative': 'index out of bounds',
         'slice': 'slice out of bounds', 'pinned': 'invalidation during stable',
         'resize_pinned': 'invalidation during stable', 'resize_stale': 'stale reference',
         'stale': 'stale reference', 'reuse': 'stale reference',
         'arena_pinned': 'arena invalidation', 'region_pinned': 'arena invalidation'}
for name, message in cases.items():
    result = subprocess.run([a.binary, name], capture_output=True, timeout=20,
                            env={**os.environ, 'ASAN_OPTIONS':
                                 f'detect_leaks={int(sys.platform != "darwin")}:abort_on_error=1'})
    if message is None:
        assert result.returncode == 0 and result.stdout == b'memory runtime: verified\n' and not result.stderr, result
    else:
        assert result.returncode == -signal.SIGABRT, (name, result)
        assert result.stderr == f'Kawa memory trap: {message}'.encode() or (
            result.stderr.startswith(b'Kawa memory trap: ') and message.encode() in result.stderr
            and b'Sanitizer' not in result.stderr), (name, result)
    print('PASS', name)
