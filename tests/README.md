# Compiler tests

Run `./run.sh` to build and run CTest, or run the dependency-free runner against a specific compiler:

```sh
python3 scripts/test.py --still ./still --json build/test-results.json
python3 scripts/test.py --still ./still --filter contracts/
python3 -m unittest discover -s scripts/harness_tests -v
```

The standalone runner and `tests/wky_format.py` (the optional lit adapter) share `scripts/test_support.py`. FileCheck must be on PATH for IR tests; override it with `FILECHECK=/absolute/path/FileCheck`. CTest uses the compiler it just built, not a stale executable elsewhere in the repository.

## Executable contracts

Existing `.wky.out` examples run at **O0, O2, and O3**, with `--bounds-check=safe`. Each build has a private working directory. Tests compare stdout as **bytes**, require the declared exit status, and reject unexpected stderr. Compiler switches inherited through `STILL_NO_OPT`, `STILL_NO_PRINTF`, and `STILL_DUMP_BAD` are cleared. Timeouts kill the subprocess group, including compiler children.

A `.wky.json` sidecar adds a precise contract:

```json
{
  "kind": "run",
  "optimizations": [0, 2, 3],
  "flags": ["--bounds-check=safe"],
  "args": ["hello world", ""],
  "stdin": "",
  "stdout": "expected bytes\n",
  "stderr": "",
  "exit": 0
}
```

Supported kinds:

| Kind | What passes |
| --- | --- |
| `run` | Executable's stdout, stderr and exit code match the oracle. |
| `reject` | Compiler exits 1 and emits the required diagnostic regex. Acceptance, an unrelated parser error, a compiler crash, or a timeout fails. |
| `trap` | Program terminates through the specified checked failure, with the required stderr diagnostic and exit code (SIGABRT by default). A segfault or bus error never counts as memory safety. |
| `ir` | FileCheck verifies emitted IR at the explicit optimization level and flags. |

An `inputs` list can supply multiple argument/stdin/output cases for one compilation. `runtime_integer_stream.wky` runs 16 deterministic seeds/counts, including zero, unsigned extrema, and array/loop boundaries, at every optimization level. Its checked-in expected values come from `scripts/generate_property_cases.py`, never from compiler output. Run that generator with `--check` to verify the vectors. Known failures must use separate cases, so one failure cannot hide the rest of a multi-input test.

IR files accept `// OPT: 2` and `// FLAGS: --bounds-check=safe`. In particular, a check-elimination test must actually enable checks first. Both a retained dynamic check and an eliminated static check are covered. Output tests accompany code-shape tests; a missing trap alone does not prove a loop was correctly optimized.

### Known gaps are visible

An `xfail` object must state the reason, failure phase, and (where appropriate) a narrow identifying pattern. It may apply to specific optimization levels. A different failure is **FAIL**, and a newly successful contract is **XPASS**, which fails the suite until the obsolete expectation is removed. Results preserve each optimization separately in JSON. `--strict` makes documented XFAILs fail too.

The original aliasing, purity, local escape, slice construction, default bounds, main/argv, embedded NUL, and diagnostic contracts are now required passes. No checked-in executable contract currently uses XFAIL. CTest runs with `--strict`.

Additional regression cases cover pointer inference and returns, purity through aliases/calls, escaped aggregate fields and slices, invalid argument indices, empty string comparisons, 5,000-byte literals, debug builds, mutated loop indices, and malformed signatures. The matrix test checks both independently calculated output and removal of redundant stack copies.

The lifetime check analyzes local address flow through SSA values and aggregates, rejecting returned local addresses and direct stores into escaping memory. It conservatively treats pointer-returning calls as potentially returning an argument. This is not the future ownership/checked-pointer system: raw pointer arithmetic, arbitrary FFI retention, heap use-after-free, and data races still require the planned model. Purity permits pointer reads and local mutation; external writes and calls without verified effects are rejected. LLVM infers optimization attributes from bodies.

Legacy tests without output oracles now have explicit expected outputs (generic parameters, operator coverage, tuples). `test_new.wky` no longer reads freed memory: pointer-to-pointer access occurs before free. A future invalid-access test must demand a checked failure, never a particular value from freed memory.

The suite also exercises empty and overlapping slices, negative/upper bounds, short-circuit side effects, wrapping unsigned arithmetic, independently calculated signed division/remainder, argument bytes, and both success/failure of `--test` with `@ignore`.

Attributes use `@name`; contracts cover stacked attributes, public functions and
SoA structs, misspellings, wrong declaration kinds, and rejection of the former
hash syntax. Raw malloc contracts exercise aliasing, dereferences, indexing,
element offsets/differences, null tests, and cleanup at all optimization levels.

The separate `scripts/audit_structs.py` readiness audit reads
`struct-readiness.json` and returns failure for every unimplemented requirement.
It is not included in the passing CTest count and never marks a known defect as
a pass. See [the audit report](../docs/compiler-and-toolchain/struct-readiness.md)
for verified bugs and missing capabilities.

Declaration contracts require `Type name` for explicit types and
`let name = expression` for inference. They cover managed and composite types,
globals, qualified bindings, `for` initializers, aliases, type-like value names,
and malformed nested delimiters. The tooling suite rejects suffix annotations
in check/format modes and compares bitcode, IR, and native objects for equivalent
explicit and inferred bindings at O0/O2/O3. Formatter tests run the formatted
managed examples against independently specified output.
Unsigned inference cases include tuple literals and function returns above the
signed 64-bit range, plus explicit wrapping with signed literal operands and
deliberate truncation. Their expected values use integer arithmetic modulo the
target width, independently of the compiler.

## Future language contracts

[future-contracts.json](future-contracts.json) defines setup/action/expected behavior for the proposed pointer, ownership, arena, stability, effect, container, concurrency, error, arithmetic, string, FFI, numerical and tooling features. Each group links to a workload in [the future benchmark plan](../bench/future-contracts.json).

These are **planned acceptance specifications**, not executable compiler tests. They deliberately avoid freezing speculative syntax. They do not count as PASS, XFAIL, or implemented features. Once syntax and diagnostics are defined, promote each scenario to an executable `.wky` case with an independent oracle and remove or link its pending entry. The harness tests check uniqueness, completeness of each scenario, and benchmark links.

Adversarial cases include descriptor reuse and generation exhaustion, accessing metadata after arena destruction, reentrant invalidation during stable access, suspension boundaries, union-payload replacement, allocation failure, locking and transfer, and transitive effects. Implementing only happy paths is insufficient to close a feature.
