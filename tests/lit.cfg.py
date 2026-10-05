# lit config for the still test suite.
#
# Two test kinds, both needing NO RUN: boilerplate in the source files:
#
#   1. Golden output:  tests/foo.wky + tests/foo.wky.out
#      still compiles foo.wky, the binary runs, stdout must match the
#      .wky.out twin byte-for-byte.
#
#   2. IR shape:       tests/ir/foo.wky
#      The file's first comment block carries `CHECK:` / `CHECK-NOT:` lines
#      (FileCheck syntax) plus optional `OPT:` to pick the -O level (default
#      O0 so checks see unoptimized IR). still emits output.ll; FileCheck
#      verifies instruction-level shape -- sext vs zext, nuw/nsw flags,
#      call signatures -- things output comparison cannot see.

import os
import shlex
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from wky_format import WkyTestFormat

config.name = "wky"
config.suffixes = [".wky"]
config.excludes = ["lib"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.environ.get("STILL_TEST_WORK", config.test_source_root)

config.substitutions.append(("%lli", shlex.quote(os.environ.get("LLI", "lli"))))


def _find_still():
    got = os.environ.get("STILL")
    if got:
        return os.path.abspath(got)
    here = os.path.dirname(os.path.abspath(__file__))
    cand = os.path.abspath(os.path.join(here, "..", "build-cmake", "still"))
    if os.path.exists(cand):
        return cand
    return "still"


STILL = _find_still()



config.test_format = WkyTestFormat(STILL)
