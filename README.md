# Kawa

[![CI](https://github.com/Dizabanik/kawac/actions/workflows/tests.yml/badge.svg)](https://github.com/Dizabanik/kawac/actions/workflows/tests.yml)
[![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)

Kawa is an experimental systems language and native compiler written in C,
using LLVM 21. It combines direct control over data and pointers with managed
owners, bounded references, arenas, and explicit stability scopes.

The language is under active development. Managed memory and ownership have
executable regression coverage; the complete safety model is still being
implemented. Legacy raw pointers retain their existing semantics. See
[IMPLEMENTATION.md](IMPLEMENTATION.md) for implemented behavior and remaining work.

## Build

Requirements: LLVM/Clang **21**, CMake **3.28+**, a C11 compiler, Python **3.9+**,
and a POSIX host. CI covers Ubuntu 24.04 x86-64 and macOS 15 ARM64.
Rust is required only for the comparison benchmarks.

```sh
git clone --recurse-submodules https://github.com/Dizabanik/kawac.git
cd kawac
```

On macOS, install the toolchain and use Apple's compiler for the sanitized C tests:

```sh
brew install llvm@21 cmake ninja
export PATH="$(brew --prefix llvm@21)/bin:$PATH"
export CC=/usr/bin/clang
```

On Ubuntu, install LLVM 21 from the [official LLVM packages](https://apt.llvm.org/)
and put `/usr/lib/llvm-21/bin` first on `PATH`. Detailed instructions, binary
release requirements, and installation options are in [INSTALL.md](INSTALL.md).

```sh
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --parallel
ctest --test-dir build-cmake --output-on-failure --parallel 2
```

Already cloned without submodules? Run `git submodule update --init --recursive`.
CMake and Make generate all embedded runtimes with the selected LLVM toolchain;
no generated bitcode header needs to be committed.

## Try Kawa

[examples/hello.kawa](examples/hello.kawa):

```kawa
fn main() -> i32 {
    let numbers: owner<i64> = own(2);
    numbers[0] = 20;
    numbers[1] = 22;
    let view: ref<i64> = ref_of(numbers);
    stable(view) {
        println("{view[0] + view[1]}");
    }
    return 0;
}
```

```sh
./build-cmake/kawac examples/hello.kawa -o build-cmake/hello
./build-cmake/hello             # prints 42
./build-cmake/kawac --check examples/hello.kawa
./build-cmake/kawac --help
```

The compiler currently writes `output.bc`, `output.ll`, and `output.o` in the
working directory. Tests and benchmarks isolate these outputs in temporary
directories. The formatter writes formatted source to stdout; use
`--format-check` to check without rewriting a file.

## Tests and measurements

[tests/README.md](tests/README.md) explains output, rejection, trap, and IR
contracts. CTest runs strict language checks at O0/O2/O3, ASan/UBSan runtime
checks, compiler tooling, and the harness itself.

```sh
# All CI benchmark correctness gates; requires rustc on PATH.
bash scripts/ci/verify_benchmarks.sh build-cmake

# Actual measurements: run on an idle machine, separate from CI.
python3 scripts/bench.py --kawac build-cmake/kawac \
  --runs 9 --warmups 2 --json build-cmake/bench.json
```

Benchmarks rebuild every implementation, verify against independent oracles,
and record samples and toolchains. Hosted CI checks correctness and safety
counters, without speed thresholds. See [bench/README.md](bench/README.md)
for workload policies and measurement limits.

## Repository

| Path | Contents |
| --- | --- |
| `src/`, `include/` | Frontend, analysis, LLVM lowering, and compiler driver |
| `src/runtime/` | Embedded I/O and managed memory runtimes |
| `timbr/` | Pinned diagnostics library submodule |
| `tests/` | Executable contracts and future acceptance specifications |
| `bench/`, `scripts/` | Matched workloads, independent oracles, and tooling |
| `.github/` | Linux/macOS CI, verified releases, and contribution templates |

[CONTRIBUTING.md](CONTRIBUTING.md) covers changes and validation.
[.github/REPOSITORY_SETUP.md](.github/REPOSITORY_SETUP.md) documents the required
GitHub check and the tagged release process.

## License

Kawa is licensed under [Apache 2.0](LICENSE). Attribution is in [NOTICE](NOTICE).
The timbr dependency retains its [MIT license](third_party/timbr.LICENSE).
LLVM and Clang are external dependencies and are not bundled in release archives.
