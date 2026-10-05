# Vision, Philosophy & Roadmap

This document outlines the design philosophy of **Whisky**, what the language is engineered to achieve, its non-negotiable architectural principles, and its roadmap for native Machine Learning (ML) and high-performance numerical computing.

---

## 1. What Whisky Tries to Be

Whisky is a compiled systems and numerical programming language designed to occupy a distinct, unoccupied space in the modern programming landscape. Its identity is governed by four core pillars:

1. **Abnormally Fast (≥ C, Faster Than Rust in Numerical & I/O Pipelines)**
   - Zero abstraction penalties.
   - Native SIMD vectorization, deterministic memory layouts, and stackless coroutine frame elision.
   - Direct LLVM code generation with whole-program Link-Time Optimization (`--lto`) and Profile-Guided Optimization (`--pgo-use`).
2. **Boringly Simple (Go's Ergonomics Without Go's Runtime)**
   - An explicit, readable grammar without hidden control flow or operator precedence games.
   - Transparent compiler behavior: what is written in source maps directly to generated machine code.
   - No hidden heap allocations, no garbage collector, and no background runtime pauses.
3. **Safe Where It's Free**
   - Eliminates vulnerabilities like buffer overflows via compile-time static bounds-check elision (`--bounds-check=safe`).
   - Pure functions (`pure fn`) that LLVM can mathematically reorder, vectorize, and eliminate.
   - Predictable resource management without the cognitive tax of Rust-style borrow-checker lifetime annotations (`<'a, 'b>`).
4. **The Easiest Language for an LLM to Read, Write, and Verify**
   - Deterministic single-pass parsing and type deduction.
   - Multi-span diagnostic engine with stable error codes (`E0001`–`E0010`) and machine-readable hints.
   - Predictable AST structure that AI coding assistants can generate without subtle hallucinated semantics.

---

## 2. Guiding Principles & Non-Negotiable Anti-Goals

Every design decision in Whisky is measured against a strict set of anti-goals:

| Anti-Goal | Why Whisky Rejects It |
|:---|:---|
| **Garbage Collection (GC)** | GC pauses introduce latency spikes. High-throughput server loops, game engines, and ML training pipelines cannot tolerate unpredictable stop-the-world pauses. |
| **Exceptions & Stack Unwinding** | Unwinding requires landing pads, code bloat, and prevents tail-call optimizations. Whisky enforces `nounwind` across the entire call graph; errors are explicit values and branch tables. |
| **Textual / AST Macros** | Macros obscure syntax trees, break IDE tooling, and create domain-specific mini-languages that confuse both humans and LLMs. Whisky uses compile-time evaluation (`comptime`) instead. |
| **Implicit Type Coercions** | Silent precision loss (e.g. converting `f64` to `f32` or signed to unsigned) is where numerical bugs hide. Conversions must be explicit via `as`. |
| **Hidden Control Flow** | No operator overloading by default, no implicit constructor conversions, and no hidden `Deref` polymorphism. If code executes, it is spelled out in the source. |

---

## 3. The Future Roadmap: Native Machine Learning & Numerics

Whisky's long-term differentiator is native, compiler-integrated numerical computing and machine learning that bypasses the layers of glue code (Python wrappers, C++ bindings, CUDA runtime overhead) typical of modern AI stacks.

### 3.1 The Native Matrix Multiplication Operator: `@`
NumPy popularized `@` for matrix multiplication. Whisky aims to be the first compiled systems language with first-class, syntax-level matrix multiplication:

```wky
// Proposed syntax
[3][3]f32 c = a @ b;           // Small/static: unrolled inline SIMD FMA kernel
c @= b;                        // Compound assignment
let y = W @ x + b;             // Direct neural layer formulation
```

* **Small / Static Shapes** ($\le 64$ dimensions): The compiler generates straight-line, zero-temporary unrolled FMA instructions (AVX-512 / ARM NEON) directly into the caller. This eliminates function call overhead and can run faster than generic BLAS libraries.
* **Dynamic / Large Shapes**: Automatically routes to cache-blocked micro-kernels or platform accelerators (Apple Accelerate, OpenBLAS, oneMKL).

### 3.2 Native Automatic Differentiation (Autodiff)
Modern machine learning requires computing gradients. Whisky's architecture is uniquely suited for automatic differentiation:

* In Whisky, `pure fn` already emits LLVM's `memory(none)` attribute, proving to the compiler that the function is mathematical and produces zero memory side-effects.
* **Planned `grad fn` syntax**:
  ```wky
  // Proposed syntax
  pure fn f32 loss(Tensor weights, Tensor inputs, Tensor targets) {
      let pred = weights @ inputs;
      return sum((pred - targets) * (pred - targets));
  }

  // Compiler synthesizes reverse-mode adjoint pass
  let (val, grad_weights) = grad(loss)(weights, inputs, targets);
  ```
* Seamless integration with LLVM-level autodiff engines (such as [Enzyme](https://enzyme.mit.edu/)) to generate reverse-mode adjoints directly from LLVM IR.

### 3.3 Scoped Fast-Math Attributes: `@fastmath`
Floating-point arithmetic in Whisky strictly follows IEEE-754 rules by default. However, vector reduction loops in neural network kernels require algebraic reassociation:

```wky
// Proposed syntax
@fastmath pure fn f32 dot([]f32 a, []f32 b) {
    let acc = 0.0f32;
    for i in 0..a.len {
        acc = acc + a[i] * b[i];
    }
    return acc;
}
```

The `@fastmath` attribute instructs LLVM to enable SIMD contraction (`fma`), algebraic reassociation, and ignore NaNs/Infs strictly within that function's lexical scope without polluting the rest of the codebase.

### 3.4 Multi-Dimensional Index Overloading: `self_index`
Extending whitelisted operator overloading to multi-dimensional bracket indexing:

```wky
// Proposed syntax
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

### 3.5 First-Class Complex Numbers (`complex f32`, `complex f64`)
Direct support for `{re, im}` pairs with native SIMD arithmetic for FFTs, digital signal processing, scientific physics simulations, and quantum computing kernels:

```wky
// Proposed syntax
complex f64 z1 = 1.0 + 2.0i;
complex f64 z2 = 3.0 - 4.0i;
complex f64 z3 = z1 * z2;
```

### 3.6 GPU & Kernel Targets: `@compute fn`
Targeting heterogeneous compute kernels (Metal Shading Language, Vulkan SPIR-V, CUDA PTX) directly from Whisky:

```wky
// Proposed long-term vision
@compute fn void vector_add([]f32 out, []f32 a, []f32 b) {
    let idx = gpu.thread_id_x();
    out[idx] = a[idx] + b[idx];
}
```

Because Whisky avoids pointers-to-pointers and maintains predictable flat memory layouts, tensor buffers can be mapped directly to GPU unified memory with zero serialization overhead.

---

## 4. Summary

Whisky is not designed to replace high-level scripting languages for web servers or glue scripts. It is designed to be the **most efficient, predictable, and ergonomic language for high-performance systems engineering, numerical computing, and machine learning**.
