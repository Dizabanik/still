# Whisky documentation

Whisky is a systems programming language that compiles ahead of time to native machine code using LLVM. Its compiler is `still`, and source files use the `.wky` extension.

The language provides direct memory control through explicit pointers and stack structures, alongside managed owners, bounded references, arenas, and stability scopes.

## Design goals

1. Efficient machine code: Whisky generates native code directly through LLVM, supporting optimizations such as link-time optimization and loop vectorization.
2. Explicit syntax: The grammar avoids hidden control flow, background garbage collection, and silent heap allocations.
3. Structured memory management: The type system distinguishes owned heap buffers (`owner<T>`) from bounded views (`ref<T>`), tracking ownership transfers statically and validating references at runtime.
4. Predictable compiler tooling: Single-pass type deduction, deterministic grammar, stable error codes (`E0001` to `E0010`), and machine-readable JSON diagnostic output.

## Documentation index

### 1. Vision and architecture
* [Vision and roadmap](vision-and-roadmap.md): Design principles, anti-goals, and proposed numerical and compute extensions.

### 2. Getting started
* [Installation and toolchain setup](getting-started/installation.md): Building the compiler from source, required dependencies, and environment setup.
* [Hello world](getting-started/hello-world.md): Writing, compiling, and running your first Whisky program.

### 3. Language guide
* [Basic types and variables](language-guide/basic-types-and-variables.md): Primitive scalar types, constants, variable declarations, and derived bindings (`orbit`).
* [Control flow](language-guide/control-flow.md): Conditionals (`if`/`else`), loops (`while`, range `for`), `match` statements, and scoped cleanup (`defer`).
* [Functions and operator overloading](language-guide/functions-and-operators.md): Function declarations, multi-return values, `pure fn`, unsafe boundaries, and operator overloading.
* [Structs, methods, and impls](language-guide/structs-methods-and-impls.md): Struct declarations, field access, `impl` blocks, `self`/`self*` receivers, associated functions, and struct embedding.
* [Tuples and destructuring](language-guide/tuples-and-destructuring.md): First-class tuples, positional indexing, and pattern destructuring for tuples, structs, and arrays.
* [Generics and monomorphization](language-guide/generics.md): Parameterized generic structs, generic `impl` blocks, and compile-time monomorphization.
* [Enums and tagged unions](language-guide/enums-and-tagged-unions.md): Variant declarations, payload storage, exhaustive pattern matching, and memory layout.
* [Slices and arrays](language-guide/slices-and-arrays.md): Fixed-size arrays, non-owning slices (`[]T`), slicing syntax, and bounds-checking policies.
* [Ownership, raw access, and typed errors](language-guide/memory-and-errors.md): Managed owners (`owner<T>`), references (`ref<T>`), arenas, unsafe operations, options, results, and `filter`/`dregs` blocks.
* [Coroutines and channels](language-guide/coroutines-and-channels.md): Cooperative stackless tasks (`brew`/`sip`/`drop`), managed channel rings, and `select` expressions.
* [Formatted I/O](language-guide/formatted-io.md): String interpolation, format specifiers, and compile-time desugared printing.

### 4. Standard library
* [`stdc`: Native C runtime](stdlib/stdc.md): C standard library function declarations and foreign function calls.
* [`std.io`: Stream output](stdlib/std-io.md): Standard output and standard error writing functions.
* [`std.process`: Process metadata](stdlib/std-process.md): Command-line argument access and process termination.

### 5. Compiler and toolchain
* [Compiler CLI reference](compiler-and-toolchain/cli-reference.md): Command-line options for `still`, optimization settings, diagnostic modes, and formatting flags.
* [Diagnostics and error codes](compiler-and-toolchain/diagnostics-and-errors.md): Compiler error code catalog (`E0001` to `E0010`), warnings, and terminal formatting.
* [Performance and benchmarks](compiler-and-toolchain/performance-guide.md): Matched workload suite, measurement methodology, and testing harnesses.

### 6. Design proposals
* [C and C++ interoperability proposal](design/c-cpp-interop.md): Prospective design for bidirectional C and C++ header import and symbol export.
