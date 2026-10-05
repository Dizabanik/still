# Performance and benchmarks

Whisky aims to generate efficient native code while preserving its safety
contracts. Performance depends on the workload, target, and toolchain. This
repository does not establish a general ranking against C or Rust.

## Matched comparisons

`scripts/bench.py` rebuilds `.wky`, C, and Rust implementations from source.
Reviewed matched workloads have independent Python output oracles. The harness
validates edge cases and the full timed input before collecting samples, rotates
execution order, and records samples, median/MAD, flags, versions, and hashes.
Machine-readable results use `wky` as the language key and `still` as its compiler.

```sh
python3 scripts/bench.py --still build-cmake/still \
  --quick --verify-only --sanitize-c --json build-cmake/verified.json

# Collect timings separately on an idle host.
python3 scripts/bench.py --still build-cmake/still \
  --runs 9 --warmups 2 --json build-cmake/measurements.json
```

Byte-exact comparisons are the default. Explicit numeric contracts specify
finite output and bounded tolerances where floating-point comparisons require
them. Comparative ratios are limited to the reviewed matched suite.

The historical `b1`–`b16` and `bench2` cases have differential or smoke coverage.
They do not establish comparable speed results. In particular, the historical
channel workload compares different scheduling mechanisms across languages.
Old timing tables and ratios are not evidence for the current implementation.

## Safety and compiler tooling

The managed-memory harnesses exercise bounded references, stable guards,
subobjects, owners, and owning aggregates. They check independent output oracles,
allocation/check counters, and sanitized C references outside timed runs.
`scripts/bench_tools.py` covers checking, formatting, and native builds with
deterministic corpora and independent checksums.

```sh
bash scripts/ci/verify_benchmarks.sh build-cmake
```

This is CI's correctness gate. Hosted runner timings are not performance gates.
See [bench/README.md](../../bench/README.md) for exact workloads and measurement
limits, and [IMPLEMENTATION.md](../../IMPLEMENTATION.md) for the implemented
optimizations and remaining language work.
