# Performance and benchmarks

Whisky compiles to native machine code through LLVM. Performance varies with workload, target platform, and optimization flags. This repository establishes no broad speed ranking against C or Rust.

## Matched comparisons

The benchmark driver in `scripts/bench.py` builds Whisky, C, and Rust implementations from source on each invocation. Matched workloads compare equivalent algorithms and memory layouts against independent Python output oracles.

The test harness validates boundary conditions and the full timed input before recording execution samples. It alternates execution order, reporting individual samples, median, median absolute deviation (MAD), compiler versions, and binary checksums.

```sh
# Correctness verification with C address and undefined behavior sanitizers:
python3 scripts/bench.py --still build-cmake/still \
  --quick --verify-only --sanitize-c --json build-cmake/verified.json

# Measurements on an idle system:
python3 scripts/bench.py --still build-cmake/still \
  --runs 9 --warmups 2 --json build-cmake/measurements.json
```

### Reviewed matched workloads

* `runtime_mix`: Runtime-seeded scalar recurrence with carried loop dependencies and bounded 64-bit integer arithmetic.
* `dynamic_gather`: Checked random read, modify, and write operations across an initialized 4096-slot buffer with varying active lengths.
* `indexed_graph`: In-place mutation during dependent traversal across inline graph nodes.
* `overlap_views`: Sequential updates through overlapping slice views of the same buffer.
* `matrix4`: 4x4 integer matrix and vector recurrence with explicit output buffers.
* `ring_pipeline`: Three capacity-64 batch buffers executing an identical single-thread pipeline schedule.

Byte-exact stdout comparison is standard across workloads. When floating-point rounding differs across platforms, workloads specify bounded absolute error tolerances.

### Historical benchmarks

The historical test suite (`b1` to `b16` and `bench2`) provides differential regression coverage. These tests do not establish comparative language speed rankings:
* `b6_chan` compares cooperative Whisky coroutines with C `ucontext` switches and Rust operating system threads. Different scheduling mechanisms cannot be treated as equivalent costs.
* `b14_format` measures standard formatting routines and stdout buffer flush policies.
* `b4_reductions` contains loop-invariant operations that optimizers can hoist depending on compilation flags.
* Prior published numbers from earlier compiler versions do not apply to revised sources.

## Managed memory and compiler tooling suites

Specialized harnesses test managed memory performance, ownership costs, and compiler driver throughput:

* `bench_memory.py`: Tests bounded reference lookups and stability scope pins (`stable`), comparing checked reference overhead against sanitized C implementations.
* `bench_owners.py`: Measures allocation, cloning, transfer, and drop cycles for complex ownership graphs.
* `bench_semantics.py`: Verifies typed result propagation, enum payload matching, and single-threaded channel transfers.
* `bench_tools.py`: Tests `still` compilation throughput, syntax check speeds (`--check`), and code formatting (`--format`).

Run the automated correctness verification script:

```sh
bash scripts/ci/verify_benchmarks.sh build-cmake
```

Continuous integration runs this check as a correctness gate rather than measuring execution speed on shared virtual machines.
