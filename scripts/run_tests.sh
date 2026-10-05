#!/bin/bash
# Standalone runner has exactly the same test semantics as lit.
set -eu
DIR="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$DIR/scripts/test.py" --still "$1" "${@:2}"
