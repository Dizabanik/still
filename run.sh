#!/bin/bash
# Build kawac and run the full test suite through lit (golden-output tests +
# IR-shape FileCheck tests). Equivalent: cmake --build build-cmake &&
# ctest --test-dir build-cmake.
set -e
cd "$(dirname "$0")"

if [ ! -f build-cmake/CMakeCache.txt ]; then
	cmake -B build-cmake -DCMAKE_MAKE_PROGRAM=make > /dev/null
fi
cmake --build build-cmake > /dev/null

exec ./scripts/lit.sh -sv tests/
