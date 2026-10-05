# The Whisky Programming Language

**Whisky** is an experimental systems and numerical programming language.
Its compiler is **`still`**, and source files use **`.wky`**. It compiles
ahead of time to native machine code via LLVM.

These guides describe the language and its goals. For the current implementation
and remaining work, consult the [implementation ledger](../IMPLEMENTATION.md)
and executable contracts in `tests/`. Use [INSTALL.md](../INSTALL.md) for the
supported toolchain and [CODE_STYLE.md](../CODE_STYLE.md) for contributor naming.

Whisky is engineered with four core tenets:
1. **Performance**: Aim for efficient native code. Compare reviewed workloads using independent output oracles and reported measurements; no general speed ranking is established.
2. **Boringly Simple**: An explicit, transparent syntax inspired by Go and C without hidden control flow, runtime garbage collection pauses, or hidden heap allocations.
3. **Memory Safety with Simple Use**: Combine managed owners, bounded references, and stability scopes with static checks and runtime validation. The complete safety model remains in progress.
4. **LLM-First Ergonomics**: Deterministic grammar, single-pass type inference, stable compiler diagnostics (`E0001`–`E0010`), and machine-readable output formats.

---

## Documentation Contents

### 1. Vision & Architecture
* [Vision, Philosophy & Roadmap](vision-and-roadmap.md): Architectural identity, non-negotiable anti-goals, and the roadmap for native Machine Learning, autodiff, and GPU targets.

### 2. [Getting Started](getting-started/installation.md)
* [Installation & Toolchain Setup](getting-started/installation.md): Building the compiler from source, dependencies, and environment setup.
* [Hello World & First Steps](getting-started/hello-world.md): Writing, compiling, and running your first Whisky program.

### 3. Language Guide
* [Basic Types & Variables](language-guide/basic-types-and-variables.md): Primitive integer, float, boolean, and character types, constants, variables, and type deduction.
* [Control Flow](language-guide/control-flow.md): Conditionals (`if`/`else`), loops (`while`, range `for`), `match` expressions, and deterministic `defer`.
* [Functions & Operator Overloading](language-guide/functions-and-operators.md): Function declarations, `pure fn`, parameter attributes, and whitelisted operator overloading.
* [Structs, Methods & Impls](language-guide/structs-methods-and-impls.md): Struct declarations, `impl` blocks, `self`/`self*` receivers, associated functions, and composition.
* [Tuples & Destructuring](language-guide/tuples-and-destructuring.md): First-class tuples, multi-return functions, positional indexing, and pattern destructuring.
* [Generics & Monomorphization](language-guide/generics.md): Multi-parameter generic structs, generic `impl` blocks, associated constructors, and method chaining.
* [Enums & Tagged Unions](language-guide/enums-and-tagged-unions.md): Tagged union variants with payloads, exhaustive pattern matching, and memory layout.
* [Slices, Arrays & Memory Safety](language-guide/slices-and-arrays.md): Fixed-size arrays, non-owning slice views, bounds-checking policies, and induction variable elision.
* [Coroutines & Channels](language-guide/coroutines-and-channels.md): Cooperative stackless coroutines (`brew`/`sip`/`drop`), power-of-two channel rings, and `select`.
* [High-Performance Formatted I/O](language-guide/formatted-io.md): String interpolation, compile-time typed desugaring, and zero-allocation buffered output.

### 4. Standard Library
* [`stdc`: Native C Runtime Interop](stdlib/stdc.md): Seamless zero-overhead C standard library FFI.
* [`std.io`: Low-Level & Streaming I/O](stdlib/std-io.md): Direct POSIX write wrappers, standard file descriptors, and buffered streams.
* [`std.process`: OS & Runtime Introspection](stdlib/std-process.md): Command-line argument access, execution environment, and exit codes.

### 5. Compiler & Toolchain
* [Compiler CLI Reference](compiler-and-toolchain/cli-reference.md): All flags and options: `-O3`, `--lto`, `--pgo-gen`, `--pgo-use`, `--bounds-check`, `--emit-hash`, and `--test`.
* [Diagnostics & Error Codes](compiler-and-toolchain/diagnostics-and-errors.md): Stable error codes, multi-span terminal reporting, and did-you-mean suggestions.
* [Performance & Benchmark Suite](compiler-and-toolchain/performance-guide.md): Matched workloads, managed-memory checks, historical cases, and measurement policy.
