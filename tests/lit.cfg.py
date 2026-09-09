# lit config for the kawac test suite.
#
# Two test kinds, both needing NO RUN: boilerplate in the source files:
#
#   1. Golden output:  tests/foo.kawa + tests/foo.kawa.out
#      kawac compiles foo.kawa, the binary runs, stdout must match the
#      .kawa.out twin byte-for-byte.
#
#   2. IR shape:       tests/ir/foo.kawa
#      The file's first comment block carries `CHECK:` / `CHECK-NOT:` lines
#      (FileCheck syntax) plus optional `OPT:` to pick the -O level (default
#      O0 so checks see unoptimized IR). kawac emits output.ll; FileCheck
#      verifies instruction-level shape -- sext vs zext, nuw/nsw flags,
#      call signatures -- things output comparison cannot see.

import os
import shlex
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from kawa_format import KawaTestFormat

config.name = "kawa"
config.suffixes = [".kawa"]
config.excludes = ["lib"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.environ.get("KAWA_TEST_WORK", config.test_source_root)

config.substitutions.append(("%lli", shlex.quote(os.environ.get("LLI", "lli"))))


def _find_kawac():
    got = os.environ.get("KAWAC")
    if got:
        return os.path.abspath(got)
    here = os.path.dirname(os.path.abspath(__file__))
    cand = os.path.abspath(os.path.join(here, "..", "build-cmake", "kawac"))
    if os.path.exists(cand):
        return cand
    return "kawac"


KAWAC = _find_kawac()



config.test_format = KawaTestFormat(KAWAC)
