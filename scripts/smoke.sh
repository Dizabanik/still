#!/bin/bash
# Smoke test: the freshly built still compiles a tiny program and the result
# runs. Invoked by ctest with the binary path as $1.
set -e
STILL="${1:?compiler path required}"
[[ "$STILL" = /* ]] || STILL="$PWD/$STILL"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/wky-smoke.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/smoke.wky" <<'EOF'
import stdc;
unsafe fn i32 main() {
    stdc.printf("smoke %d\n", 6 * 7);
    return 0;
}
EOF

cd "$WORK"
"$STILL" smoke.wky -o smoke > /dev/null
out=$(./smoke)
if [ "$out" != "smoke 42" ]; then
	echo "SMOKE FAIL: got '$out'"
	exit 1
fi
echo "smoke ok: compiled + ran + printed correctly"
