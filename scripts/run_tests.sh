#!/bin/bash
# Fallback test runner used only while `lit` is unavailable. Runs the same
# golden-file contract as lit does: compile each tests/*.kawa with a .kawa.out
# twin, run it, diff. The binary is built out-of-tree so no stray executables
# land in tests/.
set -e
KAWAC="$1"
SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/kawa-tests.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

cd "$SRCDIR"
pass=0
fail=0
for src in tests/*.kawa; do
	name=$(basename "$src" .kawa)
	expected="tests/$name.kawa.out"
	[ -f "$expected" ] || continue
	if ! "$KAWAC" "$src" -o "$WORK/$name" > /dev/null 2>&1; then
		echo "FAIL $name (compile error)"
		fail=$((fail + 1))
		continue
	fi
	if "$WORK/$name" > "$WORK/$name.actual" 2>&1 &&
		diff -q "$expected" "$WORK/$name.actual" > /dev/null 2>&1; then
		echo "PASS $name"
		pass=$((pass + 1))
	else
		echo "FAIL $name"
		diff "$expected" "$WORK/$name.actual" | head -10 || true
		fail=$((fail + 1))
	fi
done

echo ""
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
