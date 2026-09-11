#!/bin/bash
# Build kawac and run semantic, FileCheck, and harness tests through CTest.
# Equivalent: cmake --build build-cmake &&
# ctest --test-dir build-cmake.
set -e
cd "$(dirname "$0")"

if [ ! -f build-cmake/CMakeCache.txt ]; then
	cmake -B build-cmake -DCMAKE_MAKE_PROGRAM=make > /dev/null
fi
cmake --build build-cmake > /dev/null

exec ctest --test-dir build-cmake --output-on-failure
