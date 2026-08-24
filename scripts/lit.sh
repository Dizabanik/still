#!/bin/bash
# Resolve the lit test runner: repo-local vendored copy first, then PATH,
# then Homebrew's LLVM keg. Keeps ctest/lit wiring independent of where the
# user installed it.
DIR="$(cd "$(dirname "$0")/.." && pwd)"
if [ -x "$DIR/tools/bin/lit" ]; then
	export PYTHONPATH="$DIR/tools/lib/python3.11/site-packages${PYTHONPATH:+:$PYTHONPATH}"
	exec "$DIR/tools/bin/lit" "$@"
fi
for cand in "$(command -v lit)" /opt/homebrew/opt/llvm/bin/lit; do
	[ -n "$cand" ] && [ -x "$cand" ] && exec "$cand" "$@"
done
echo "lit not found: run scripts/setup_lit.sh" >&2
exit 1
