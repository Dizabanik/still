#!/bin/bash
# Smoke test: the freshly built kawac compiles a tiny program and the result
# runs. Invoked by ctest with the binary path as $1.
set -e
KAWAC="${1:?compiler path required}"
[[ "$KAWAC" = /* ]] || KAWAC="$PWD/$KAWAC"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/kawa-smoke.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/smoke.kawa" <<'EOF'
import stdc;
fn i32 main() {
    stdc.printf("smoke %d\n", 6 * 7);
    return 0;
}
EOF

cd "$WORK"
"$KAWAC" smoke.kawa -o smoke > /dev/null
out=$(./smoke)
if [ "$out" != "smoke 42" ]; then
	echo "SMOKE FAIL: got '$out'"
	exit 1
fi
echo "smoke ok: compiled + ran + printed correctly"
