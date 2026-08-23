#!/bin/bash
# Build and run all Kawa tests, comparing against expected output.
set -e

make

pass=0
fail=0
for src in tests/*.kawa; do
    name=$(basename "$src" .kawa)
    expected="tests/$name.kawa.out"
    if [ ! -f "$expected" ]; then
        echo "SKIP $name (no .out file)"
        continue
    fi
    ./kawac "$src" > /dev/null 2>&1
    if lli output.bc > "$TMPDIR/kawa_actual.txt" 2>&1 && diff -q "$expected" "$TMPDIR/kawa_actual.txt" > /dev/null 2>&1; then
        echo "PASS $name"
        pass=$((pass + 1))
    else
        echo "FAIL $name"
        diff "$expected" "$TMPDIR/kawa_actual.txt" | head -10 || true
        fail=$((fail + 1))
    fi
done

rm -f output.bc output.ll "$TMPDIR/kawa_actual.txt"
echo ""
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
