# Benchmarks

`scripts/bench.py` builds **Whisky, C and Rust from their source files on every invocation**. It never discovers or runs the old `*_c`/`*_rs` executables. An explicit [manifest](manifest.json) selects sources, input sets, workload contracts, and output comparisons.

```sh
# Fast correctness check, including separate untimed C ASan/UBSan builds:
python3 scripts/bench.py --still ./still --quick --verify-only --sanitize-c

# Measurements: run alone on an idle host, with stable power settings.
python3 scripts/bench.py --still ./still --runs 9 --warmups 2 --json build/bench.json

# If a workload is too short, increase work inside every language's process:
python3 scripts/bench.py --still ./still --filter runtime_mix --iterations 50000000

# Historical implementation comparisons, explicitly requested:
python3 scripts/bench.py --still ./still --suite historical --verify-only
python3 scripts/bench.py --list
```

`cmake --build build-cmake --target bench-verify` runs the quick correctness gate. CI runs this gate, never a speed threshold on shared hardware.

## What the measurements mean

Every new workload has runtime inputs (no source substitution), dependent observable output, multiple boundary/seed cases, and an independent arbitrary-precision Python oracle in `scripts/bench_oracles.py`. Verification includes **the full timed input**, not just small cases. Every warmup and timed run must also produce the expected output. A bad build, wrong output, unexpected stderr, crash, or timeout fails the workload and produces no timing comparison.

The timer measures **whole-process elapsed time**: process launch, argument parsing, initialization, work, formatting, stdout-pipe handling, and teardown. These are not isolated kernel-cycle measurements. Standard-library startup and argument parsing can allocate differently even though the new kernels do not allocate. The zero-allocation descriptions below apply to the kernel, not the entire process.

Each measured round runs each language once, in seeded randomized order. All receive the same number of warmups and samples. Reports contain every sample, median, median absolute deviation (MAD), minimum, maximum, order, inputs, source/binary hashes, compiler versions and paths, build commands/times, executable sizes, and host details. Builds and Python oracles are outside timed intervals. Sanitizer runs are also outside timing.

Durations below 50 ms and MAD above 5% produce warnings. `--quick` is for checking the harness, not publishing speed claims. Increase `--iterations` if startup noise dominates; that scales work *inside* the process, not repeated process launches. There is no universal Whisky-versus-C pass threshold and no aggregate language ranking. Median ratios are descriptive and appear only for the matched suite; they are not statistical significance claims.

Build policies are explicit:

| Implementation | Policy |
| --- | --- |
| Whisky | `-O3 --bounds-check=safe`; compiler selects host target; current compiler's O3 pipeline includes its LTO pass. |
| C | `-O3 -march=native -std=c11 -ffp-contract=off`; dynamic index guards are written explicitly where needed. |
| Rust | `--edition=2021`, O3, native CPU, one codegen unit, panic abort, overflow checks off; normal indexed accesses retain bounds semantics. |

The new workloads use bounded i64 arithmetic: for the declared domain, intermediates fit without signed overflow. The original matched workloads use legacy storage; managed-reference safety is exercised separately below. “Matched” means equivalent algorithms, storage layouts, scheduling policies and valid-input behavior, not proof of identical safety implementations or assembly. Whisky's argument accessor returns a checked, sized byte view. The setup parser explicitly decays that view to a C pointer to parse the controlled, NUL-terminated decimal arguments; this keeps the workload algorithm unchanged.

## New matched workloads

| Workload | Measures and controls |
| --- | --- |
| `runtime_mix` | Runtime-seeded scalar recurrence, carried dependence, bounded i64 arithmetic. |
| `dynamic_gather` | Checked random read/modify/write of a fully initialized 4096-slot buffer, with varying active lengths. |
| `indexed_graph` | Mutation during dependent traversal of inline `{next,value}` nodes. A baseline for future checked pointers, not yet a pointer-safety benchmark. |
| `overlap_views` | Sequential updates through overlapping views of the same storage. Rust expresses the same update order using one mutable slice. |
| `matrix4` | Explicit 4×4 integer matrix/vector recurrence with output storage, no implicit temporaries or BLAS. Future shape/aliasing baseline. |
| `ring_pipeline` | Three capacity-64 batch buffers and an identical one-thread fill/stage/drain schedule. This measures buffer/pipeline work, **not** coroutine or channel overhead. |

Each includes zero/one iteration, multiple seeds, and boundaries such as 63/64/65. Variable-length workloads include small, non-power-of-two and full capacity lengths. Arithmetic oracles use Python integers; the ring checksum uses a closed-form equation independent of the batch implementation. The input domain is nonnegative iterations up to 100 million, seed 0..65535, and active length 1..4096 (at least 2 for overlap).

## Historical suite

The original 17 source triples remain available under `--suite historical`. They are freshly built and correctness-checked too, but have fixed inputs and no independent oracle, so agreement among twins is weaker evidence. Their timings receive no language-ranking ratios.

- `b6_chan` compares cooperative Whisky coroutines, C `ucontext`, and Rust OS threads. Equal output does not make their scheduling costs equivalent. `ring_pipeline` supplies a separate equal-policy baseline; it does not replace this end-to-end implementation comparison.
- `b14_format` measures different formatting/buffering libraries, including output handling. It is not evidence of a generally faster language.
- `b4_reductions` and `bench2` contain invariant work that an optimizer may hoist. Do not interpret their source iteration counts as proof of executed loads or memory bandwidth.
- `b3_particles` uses a declared **2e-6 absolute tolerance** on its six-decimal accumulator, with exact output structure and finite values required. Different historical FP contraction/rounding paths are not byte-identical. The report preserves per-language output hashes and the comparison policy.
- Signed-overflow hazards in `b6`, `b7`, `b8`, and `b16` have been replaced by bounded arithmetic across all three languages. Their outputs changed intentionally. Old recorded numbers and generated documentation do not describe the revised sources.

All other historical output comparisons are byte-exact. Prefer the matched suite for new comparisons. Do not restore the old timing table without a fresh report and a workload-specific interpretation.

Historical hash and tuple kernels now use explicit `wrap_mul`/`wrap_add`, and
the typed-error input mix uses `lossy_u32` for its deliberate truncation. Their
unsigned masks and shifts retain unsigned types. These spellings preserve the
C/Rust algorithms under Whisky's checked numeric policy; they do not disable
checks elsewhere. CI verifies all 17 historical triples as well as the matched
suite, while keeping their weaker differential evidence separate.

## Managed memory and compiler tooling

```sh
python3 scripts/bench_memory.py --still ./build-cmake/still --verify-only --sanitize-c
python3 scripts/bench_memory.py --still ./build-cmake/still --workload subobject_walk --rounds 10000000 --sanitize-c --json build/subobject-benchmark.json
python3 scripts/bench_owners.py --still ./build-cmake/still --verify-only
python3 scripts/bench_owners.py --still ./build-cmake/still --workload owned_values --trials 64 --json build/owned-values-benchmark.json
python3 scripts/bench_tools.py --still ./build-cmake/still --verify-only
python3 scripts/bench_semantics.py --still ./build-cmake/still --verify-only
python3 scripts/bench_semantics.py --still ./build-cmake/still --json build/semantics-benchmark.json
```

The reference and field-view walks compare Whisky and C using the same descriptor runtime, reference ABI, initialization, and arithmetic policy. Each has an independent Python checksum oracle, 80 boundary runs, and eight separately instrumented cases. The field-view walk constructs two bounded references per iteration into a 32-byte row. Checked mode performs four indexed validations per iteration. Stable mode performs zero indexed validations, but acquires two guards per iteration plus one outer guard. This workload includes that guard cost; it does not assume stability always improves speed.

The ownership lifecycle workload verifies deep chains and fanout trees, allocation failure, clone/drop behavior, and matching runtime counters. The owned-values workload holds four buffers in a 136-byte aggregate, then clones, moves, traverses, and drops it. Its two modes transfer directly between stack values or through a managed heap slot. Each ownership workload verifies 40 edge runs, four instrumented cases and two untimed C sanitizer runs. Their checksum oracles are closed-form Python equations, with explicit allocation, copied-byte, peak-byte and validation-count formulas. Compiler tooling uses deterministic source corpora with independent execution checksums, formatter fixed points, and repeated bitcode/IR/object hashes. It measures check, format, and build separately.

Compiler tooling corpus version 2 includes four cases: 12 kernels, 256 kernels,
64 kernels calling 17 nested generic helpers at four widths, and 64 kernels
using type-first managed declarations. The last case covers nested owners,
references, qualified bindings, arrays of refs, generic struct instances,
stable access, and scope cleanup. Each program reads its input at runtime and
has a separately calculated execution checksum. Verification reports no timings;
run without `--verify-only` on an idle host to measure the current compiler.

Managed timings use seven samples and two warmups by default. Reports preserve native process times, median/MAD, individual RSS and CPU samples, binary sizes, hashes, compiler versions, and all counter checks. Instrumentation and sanitizer builds run outside timed intervals. On macOS, sanitizer verification uses the system Clang and records its version separately from the LLVM compiler used for native comparisons.

`--optimization-report[=path]` produces static IR facts and source-associated operations. Its counts describe emitted instructions, branches, and calls, not executed operations. A disappearing source tag or direct call can result from inlining or metadata loss, so reports do not treat it as proof that a runtime check or allocation was removed.

## Future performance acceptance

[future-contracts.json](future-contracts.json) specifies workloads, input distributions, correctness oracles, and required metrics for every proposed language area. It is linked from [future compiler acceptance cases](../tests/future-contracts.json).

The plan includes per-access checked references versus inferred/explicit stability, descriptor reuse, cyclic arenas, resize invalidation, ownership moves/clones, noalloc effects, typed error paths, strings/C conversion, FFI, synchronization/transfer, shaped numerics and compiler tooling. It asks for allocation/check counts and peak memory where relevant, as well as time. Counters and native implementations must exist before those entries become measurements; no fabricated placeholder timings are reported.

## Typed values and ownership transfer

`bench_semantics.py` builds fresh Whisky/C pairs at O0, O2 and O3, instrumented
O3 pairs, and separate untimed C ASan/UBSan builds. Five boundary inputs per
binary include empty work, all failures/scalars, non-power-of-two channel sizes,
partial batches and ring wraparound. The three workloads are:

- `typed_result`: two function boundaries with `result<u64,u64>` and `try`,
  a runtime failure period, and independent success/error checksum formulas.
- `owned_enum`: a 40-byte union alternates scalar garbage pointer bits and an
  owned buffer. Clone and consuming match check independence and active-tag cleanup.
- `owned_channel`: owned buffers cross a FIFO ring under the same batch schedule
  in both languages. Requested capacity stays exact, backing storage rounds up,
  and a position-weighted checksum detects ordering errors.

The C ownership pairs inline the same runtime and use the same ABI, zero
initialization, checks, transfer and drop policy. Instrumented runs must match
allocation/free/peak/cloned-byte/validation counters as well as their independent
Python oracle. Instrumentation, sanitizers, builds and oracles are excluded from
reported timings; normal runs use fresh processes, balanced randomized order,
warmups, samples, median/MAD, individual peak RSS, hashes and compiler versions.
These workloads measure the specified whole process. The channel workload does
not measure coroutine suspension or cross-thread scheduling. Scalar result
propagation allocates no heap storage; owning payloads allocate explicitly.

Use `--rounds` for owning workloads and `--result-rounds` for the scalar workload.
Defaults are two million owning iterations and 100 million scalar iterations;
increase them when the reported duration is too short for your machine.
Durations below 50 ms and MAD over 5% are flagged. CI runs verification only,
including all optimization levels; it imposes no performance threshold.

Compound assignments now evaluate their target once. C comparison sources use
the same already-validated address for reading the old value, and ownership
counter formulas account for that reduced validation count. Runtime resource
adoption affects descriptor accounting. Opaque coroutine frame bytes are not
included in managed payload-byte metrics; those require allocator/RSS measurements.
