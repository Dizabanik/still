# Vision and roadmap

Whisky is a compiled systems programming language designed for numerical computing and systems engineering. This document describes the language design priorities, rejected language features, and planned extensions.

## Core design priorities

1. Low overhead execution: The compiler generates native code via LLVM without an interpreter or virtual machine. Optimization pipelines support whole-program link-time optimization (`--lto`), profile-guided optimization (`--pgo-use`), and SIMD loop vectorization.
2. Explicit language model: Source syntax directly reflects the generated machine code. The runtime does not include a background garbage collector, reference-counting headers, or implicit heap allocations.
3. Selective static safety: The compiler eliminates array bounds checks at compile time when indices are proven safe by constant folding or loop induction analysis. Pure functions (`pure fn`) emit LLVM `memory(none)` attributes, allowing instruction reordering and dead code elimination.
4. Deterministic compiler interface: The grammar parses in a single pass with predictable type deduction. Diagnostics provide stable error codes (`E0001` to `E0010`) and machine-readable JSON output for automated tooling.

## Design constraints and rejected features

| Rejected feature | Rationale |
|:---|:---|
| Garbage collection | Garbage collection pauses introduce unpredictable latency spikes. Systems loops and numerical pipelines require deterministic deallocation. |
| Exceptions and stack unwinding | Stack unwinding requires landing pads, increases binary size, and prevents tail-call optimizations. Whisky marks functions `nounwind` across the call graph. Error handling uses explicit values and branch tables. |
| Macro expansion | Textual and AST macros obscure syntax trees, impede language server tooling, and complicate static analysis. Compile-time evaluation relies on `comptime` constructs instead. |
| Implicit type coercion | Silent conversions between integer widths or between floating-point representations conceal precision loss. Conversions require explicit casts: `(TargetType)expr`. |
| Implicit control flow | Operator overloading requires explicitly named methods in `impl` blocks. Constructors, dereferences, and conversions remain visible in the source. |

## Proposed numerical and machine learning extensions

The language roadmap includes compiler-integrated numerical features intended to reduce external foreign-function wrapper overhead.

### Matrix multiplication operator: `@`

A syntax-level matrix multiplication operator:

```wky
[3][3]f32 c = a @ b;
c @= b;
let y = W @ x + b;
```

For small fixed dimensions, the compiler can generate inline fused multiply-add (FMA) instructions directly into the calling function. For larger or dynamic dimensions, the operator will route to platform BLAS libraries such as OpenBLAS or Apple Accelerate.

### Automatic differentiation

Because `pure fn` guarantees zero memory side effects, functions marked pure can serve as inputs for reverse-mode automatic differentiation:

```wky
pure fn f32 loss(Tensor weights, Tensor inputs, Tensor targets) {
    let pred = weights @ inputs;
    return sum((pred - targets) * (pred - targets));
}

let (val, grad_weights) = grad(loss)(weights, inputs, targets);
```

Integration with LLVM-level differentiation tools like Enzyme will synthesize gradient passes directly from intermediate representations.

### Lexically scoped fast-math attributes: `@fastmath`

Floating-point arithmetic in Whisky follows IEEE-754 rules by default. The `@fastmath` attribute allows algebraic reassociation and contraction strictly within an annotated function:

```wky
@fastmath pure fn f32 dot([]f32 a, []f32 b) {
    let acc = 0.0f32;
    for (i in 0..a.len) {
        acc = acc + a[i] * b[i];
    }
    return acc;
}
```

This isolates reciprocal approximations and reassociation to specific numerical kernels without altering IEEE-754 semantics across the rest of the codebase.

### Multi-dimensional index overloading: `self_index`

Multi-parameter bracket indexing extends custom container access to multi-dimensional data:

```wky
impl Tensor {
    fn f32 self_index(self*, i64 row, i64 col) {
        return self.data[row * self.stride + col];
    }
    fn void self_index_set(self*, i64 row, i64 col, f32 val) {
        self.data[row * self.stride + col] = val;
    }
}

let val = tensor[2, 5];
tensor[2, 5] = 4.2;
```

### Native complex numbers

Direct support for complex floating-point types (`complex f32`, `complex f64`) provides arithmetic support for signal processing and simulation kernels:

```wky
complex f64 z1 = 1.0 + 2.0i;
complex f64 z2 = 3.0 - 4.0i;
complex f64 z3 = z1 * z2;
```

### Compute kernel targets: `@compute fn`

Targeting heterogeneous compute kernels directly from Whisky:

```wky
@compute fn void vector_add([]f32 out, []f32 a, []f32 b) {
    let idx = gpu.thread_id_x();
    out[idx] = a[idx] + b[idx];
}
```

Contiguous, flat memory layouts allow passing buffers directly to unified GPU memory without serialization layers.
