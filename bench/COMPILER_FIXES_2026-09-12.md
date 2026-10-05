# Compiler fixes and performance — 2026-09-12

All 12 previously failing contracts are now required passes. The strict suite contains 340 passing optimization variants/IR cases, with no XFAILs; the 20 harness tests also pass. The 56 future feature specifications remain planned work.

## Compiler changes

- Ordinary pointer parameters may alias. Removed unconditional `noalias` and unverified purity attributes that allowed incorrect optimization.
- A compile-time analysis verifies pure functions and rejects local addresses escaping through returned pointers, aggregate fields, slices, branches, and direct stores into escaping memory. Pointer reads and local mutation remain valid in pure functions; LLVM infers their memory effects.
- Bounds checks default to safe mode. Slice construction checks endpoint order and extent before pointer arithmetic, including inclusive endpoints. Index-check elimination relies on LLVM's proofs rather than an iterator name that a loop body can mutate.
- The entry wrapper forwards integer exit codes, flushes output, and builds proper `str` views for `main(i32, str*)`. Raw `char**` arguments remain supported. `arg_at` checks the index and returns a sized byte view.
- String literals preserve decoded byte lengths, embedded NULs, and long literals. Comparisons check lengths before reading bytes and skip memory access for empty prefixes. Literal storage has one terminator and can be merged by LLVM.
- Calls reject incorrect arity before LLVM verification; missing fields use E0008. Pointer return types and inferred pointer locals now work. Malformed signatures terminate with a diagnostic rather than exhausting the arena.
- All generated basic blocks use their function's LLVM context. The former global/private context mismatch left redundant same-looking casts and copies that blocked scalar replacement. A final scalar cleanup also runs after optimized pipelines. Debug metadata is finalized before analysis/optimization, and pass errors are reported.

The lifetime analysis is conservative and local. It is not the proposed ownership/checked-pointer system: arbitrary FFI retention, heap use-after-free, raw-pointer extents, and data races still need that design. Pointer-returning calls are conservatively treated as potentially returning an argument. These limitations are documented in the test guide.

## Measured result

The strongest result is the checked matrix recurrence. In 11 randomly ordered before/after pairs, both compilers built the **same source**, with `-O3 --bounds-check=safe`, 50 million iterations and two warmups. Every output matched the independently verified oracle hash. Timing includes the whole process. The host was macOS on arm64 with LLVM 21.1.5.

| Matrix recurrence | Median | MAD |
| --- | ---: | ---: |
| Before | 350.615 ms | 1.927 ms |
| After | 134.587 ms | 0.508 ms |

**2.61× faster** in this paired measurement. Inspection of the optimized IR confirms the redundant stack copies are gone; a regression test checks this alongside independently calculated numerical output.

The complete six-workload runs used seven samples and two warmups per language, with 50 million iterations. The table below shows Whisky medians from the separate full-suite runs; only the matrix result was additionally checked in paired before/after order.

| Workload | Before | After |
| --- | ---: | ---: |
| runtime_mix | 46.814 ms | 46.918 ms |
| dynamic_gather | 67.438 ms | 62.423 ms |
| indexed_graph | 73.126 ms | 72.340 ms |
| overlap_views | 120.094 ms | 116.771 ms |
| matrix4 | 350.697 ms | 133.332 ms |
| ring_pipeline | 29.329 ms | 28.892 ms |

The other differences are small or accompanied by similar changes in the C control. Do not interpret them as demonstrated compiler speedups. `runtime_mix` and `ring_pipeline` are below 50 ms and have explicit startup/noise warnings. No meaningful regression appeared in these measurements; that is evidence for these workloads on this host, not a guarantee for arbitrary programs.

## Reproduction and raw evidence

The before compiler was preserved from commit `79ba376`; the after compiler is this change. Reports contain binary/source hashes, exact build commands, input/output hashes, sample order and raw durations. The benchmark Whisky sources changed only in an explanatory argument-parser comment during this work; the paired matrix run used byte-identical source for both builds.

```sh
ctest --test-dir build-cmake --output-on-failure
python3 scripts/bench.py --iterations 50000000 --runs 7 --warmups 2 --json build/performance-after-fixes.json
python3 scripts/bench.py --suite all --quick --verify-only --sanitize-c
```

Local reports (build artifacts, not checked-in universal performance claims):

- [Before full run](../build/performance-before-fixes.json)
- [After full run](../build/performance-after-fixes.json)
- [Paired matrix samples](../build/performance-matrix-paired.json)
- [Strict test results](../build-cmake/unit-results.json)
- [All 23 benchmark correctness and C sanitizer checks](../build/benchmark-fixes-verification.json)
- [Historical verification after the final string change](../build/historical-fixes-verification.json)

The six matched workloads were reverified in the final measured run. C sanitizer runs are untimed and validate the C reference programs, not generated Whisky machine code. All validation here ran on macOS; the Linux CI workflow has not been executed locally.
