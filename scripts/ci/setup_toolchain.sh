#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# LLVM's major version is part of the compiler ABI and embedded bitcode format.
set -euo pipefail

run_root() {
  if [[ $(id -u) == 0 ]]; then "$@"; else sudo "$@"; fi
}

case "${RUNNER_OS:-}" in
  Linux)
    # The workflow pins Ubuntu 24.04 rather than silently changing apt suites.
    run_root apt-get update
    run_root apt-get install -y --no-install-recommends ca-certificates curl gnupg cmake ninja-build python3
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    curl --fail --silent --show-error --location --retry 3 \
      https://apt.llvm.org/llvm-snapshot.gpg.key --output "$work/llvm.key"
    gpg --batch --dearmor --output "$work/llvm.gpg" "$work/llvm.key"
    run_root install -m 644 "$work/llvm.gpg" /usr/share/keyrings/still-llvm.gpg
    printf '%s\n' 'deb [signed-by=/usr/share/keyrings/still-llvm.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main' > "$work/llvm.list"
    run_root install -m 644 "$work/llvm.list" /etc/apt/sources.list.d/still-llvm.list
    run_root apt-get update
    run_root apt-get install -y --no-install-recommends clang-21 llvm-21-dev llvm-21-tools libclang-rt-21-dev lld-21
    llvm_bin=/usr/lib/llvm-21/bin
    ;;
  macOS)
    brew install cmake ninja llvm@21
    llvm_bin="$(brew --prefix llvm@21)/bin"
    ;;
  *)
    printf 'Unsupported runner: %s\n' "${RUNNER_OS:-unset}" >&2
    exit 1
    ;;
esac

printf '%s\n' "$llvm_bin" >> "${GITHUB_PATH:?GITHUB_PATH must be set}"
export PATH="$llvm_bin:$PATH"
[[ $(llvm-config --version) == 21.* ]]
clang --version
llvm-config --version
FileCheck --version
rustc --version
python3 --version
cmake --version
