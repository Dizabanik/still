#!/bin/bash
# Vendor lit into tools/ via pip --prefix (works even when ~/.local is not
# writable and site-packages is managed by a conda env). Idempotent: skips
# when tools/bin/lit already runs.
set -e
DIR="$(cd "$(dirname "$0")/.." && pwd)"
if "$DIR/scripts/lit.sh" --version >/dev/null 2>&1; then
	echo "lit already available: $("$DIR/scripts/lit.sh" --version)"
	exit 0
fi
echo "Installing lit into tools/ ..."
python3 -m pip install --quiet \
	--trusted-host pypi.org --trusted-host files.pythonhosted.org \
	--prefix "$DIR/tmp/litenv" lit
mkdir -p "$DIR/tools"
cp -R "$DIR/tmp/litenv/." "$DIR/tools/"
rm -rf "$DIR/tmp/litenv"
"$DIR/scripts/lit.sh" --version
