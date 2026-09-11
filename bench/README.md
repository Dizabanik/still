# Benchmarks

Each `<name>.kawa` is paired with byte-identical-output twins:
`<name>.c` (C) and `<name>.rs` (Rust). `scripts/bench.py` builds the Kawa
source, times all three best-of-N, and reports deltas.

## Prebuilt twins

The harness expects executables named `<prefix>_c` and `<prefix>_rs`
(e.g. `b5_c`, `b5_rs`) in this directory. Build flags used for the
recorded numbers:

    # C twins
    cc -O3 -march=native -o <prefix>_c <name>.c
    # b6 needs ucontext on macOS:
    cc -O3 -march=native -Wno-error=incompatible-function-pointer-types \
       -o b6_c b6_chan.c

    # Rust twins
    rustc -C opt-level=3 -C target-cpu=native -C codegen-units=1 \
          -o <prefix>_rs <name>.rs

## Notes

- `b6_chan`: pipeline of brew{} coroutines over cap-64 rings. The Kawa
  version drains the output ring per scheduler round (`while c4.count > 0`)
  and marks `step` as `pure fn` so the sent value never spills through a
  coroutine frame. Twins: ucontext coroutines (C) and std::sync::mpsc +
  threads (Rust). On macOS swapcontext costs ~870ns (sigprocmask et al.
  per switch), so Kawa's userspace resumes win big here; on Linux the gap
  narrows but does not close. At N=1e8 Kawa finishes ~9x ahead.
- `b8_generics`: generic `Box(T)` (`Box(i64)` and `Box(i32)`) monomorphized
  into specialized structs and methods in a 50M-iteration loop. Validates
  zero-overhead generic instantiation and SROA scalarization matching C and Rust.
- `b9_destructure`: comprehensive destructuring `let` benchmark testing named
  patterns (`let Point { x, y } = pt;`), renamed patterns (`let Point { x: px, y: py }`),
  inferred patterns (`let { x, y } = pt;`), positional structs (`let (p1, p2) = p;`),
  and fixed arrays (`let (a0, a1) = arr;`) in a 50M-iteration loop. Validates
  that destructuring compiles to direct register/field extractions with zero runtime penalty.
- `b10_assoc_const`: type-scoped associated constants (`impl Mat4 { const DIM = 4; const SIZE = 16; }`)
  used for array dimension sizing (`[Mat4.SIZE]i64`) and loop bounds in a 20M-iteration
  matrix-vector transformation loop.
- `b11_tagged_union`: tagged union payloads and pattern matching (`enum Shape { Circle(i64), Rect(i64, i64), Point }` and `match s { ... }`) in a 20M-iteration loop. Validates zero-overhead payload packing, inline constructors, and branch switch lowering matching optimized C tagged unions and Rust enums.
- `b12_range_slice`: range-based loops (`for i in 0..len`), zero-copy slice views (`[]i64 view = buf[start..end]`), slice iteration (`for v in s`), and in-place slice mutation across 50K rounds. Validates zero-cost slice abstractions matching raw pointer slices in C and Rust slices.
- `b13_bounds`: bounds check elimination via static constant folding and loop-carried range induction analysis in a 51.2M-access array workload. Validates zero-cost array indexing matching unchecked C access and optimized Rust loops.
- `b14_format`: string interpolation with hoisted arguments, compile-time typed desugaring, 64KB zero-allocation user-space buffered I/O, 2-digit radix-10 integer formatting, exact 128-bit fixed-point float rendering, and zero per-call locks across 1,000,000 formatted lines. Kawa finishes in 0.026s, beating Rust (0.070s) by 2.69x and C (0.122s) by 4.69x with byte-identical output.
- `b15_tuples`: first-class tuples `(T1, T2, ...)`, multi-return functions, tuple destructuring `let (a, b) = ...`, and positional element access (`.0`, `.1`) across 20M iterations. Validates register-passed multi-return SysV/ARM64 ABI and zero-cost scalarization matching C structs and Rust tuples.
- `b16_generics_multi`: multi-parameter generic structs (`Pair(A, B)`), generic impl blocks with methods and associated constructors, and method chaining across 20M iterations. Validates monomorphized code generation parity with C templates and Rust generics.
- Outputs must be byte-identical across the three languages -- that is
  the correctness check tying the twins together.
