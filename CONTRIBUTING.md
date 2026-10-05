# Contributing to Whisky

Whisky is evolving. Read [README.md](README.md), the
[implementation ledger](IMPLEMENTATION.md), and the relevant executable
contracts before changing language behavior. Future contract manifests are
plans; they are not evidence that a feature already works.

Follow [CODE_STYLE.md](CODE_STYLE.md) for naming, formatting, memory handling,
and performance conventions. In particular, the language is Whisky (`wky`),
while its compiler is `still`; their identifier prefixes have different roles.

## Development

Follow [INSTALL.md](INSTALL.md) for the LLVM 21 toolchain, initialize the timbr
submodule, and use an out-of-tree CMake build.

```sh
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --parallel
ctest --test-dir build-cmake --output-on-failure --parallel 2
bash scripts/ci/verify_benchmarks.sh build-cmake
```

Use `cmake --build build-cmake --target still_debug` for the debug compiler.
Focused language checks can use `python3 scripts/test.py --still
build-cmake/still --strict --filter ownership/`.

## Changes and review

- Explain the concrete behavior before and after the change.
- Add regression coverage with an independently calculated oracle for compiler
  or runtime changes. A crash is never a successful memory-safety test.
- Keep `.wky.out` fixtures byte-exact. Do not regenerate expected output from
  the compiler being tested or weaken contracts to hide failures.
- For performance changes, provide workload inputs, toolchain versions,
  correctness checks, samples, median/MAD, and relevant allocation/check counters.
  Run timings on an idle host; hosted CI is a correctness gate.
- Follow the surrounding C style and `.clang-format`. Avoid unrelated formatting
  changes. Keep generated files, executables, environments, and timing reports
  under ignored build directories.
- Update the implementation ledger when a planned contract gains real coverage
  or a language restriction changes.

Pull requests run the same Linux/macOS verification as release builds. Reports
remain available as workflow artifacts, including on failure.

## Licensing

Contributions intentionally submitted for inclusion in Whisky are provided under
the Apache License, Version 2.0, as described in [LICENSE](LICENSE). Preserve
existing attribution and the separate MIT terms of the timbr dependency.
