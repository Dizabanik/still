# Whisky and still code conventions

## Names and namespaces

**Whisky** is the language. **`wky`** is its machine-readable abbreviation and
**`.wky`** is its source extension. **`still`** is the compiler executable and
compiler distribution. Choose names by what they represent:

| Domain | Convention | Examples |
| --- | --- | --- |
| Language in prose and diagnostics | `Whisky` | Whisky memory trap, Whisky program |
| Source files, code fences, language keys | `wky`, `.wky` | `hello.wky`, benchmark `sources.wky` |
| Compiler executable, build targets, release archives | `still` | `still_debug`, `still-0.1.0-linux-x86_64.tar.gz` |
| Compiler lifecycle, diagnostics, reports, debug emission | `still_`, `Still` | `still_compile`, `still_diag_error`, `StillCompiler` |
| Language semantics, lowering, runtime data types | `wky_`, `Wky` | `wky_comptime_eval`, `wky_memory_layout`, `WkyRef` |
| Compiler controls, diagnostic constants, compiler header guards | `STILL_` | `STILL_NATIVE_CPU`, `STILL_NO_OPT`, `STILL_E_TYPE`, `STILL_CODEGEN_H` |
| Runtime controls and language execution policies | `WKY_` | `WKY_MEMORY_METRICS`, `WKY_GENERATION_MAX`, `WKY_COMPTIME_MAX_STEPS` |
| Generated runtime symbols and LLVM type/attribute/metadata names | `__wky_`, `wky.` | `__wky_mem_address`, `wky.site`, `wky.source` |

Use **`wky_`**, not `why_` or a full `whisky_` prefix, for language interfaces.
Use **`still_`** when an API manages the compiler or its output. For example,
`still_init` initializes a compiler; `wky_memory_layout` describes a language
value's runtime representation even though the implementation lives in codegen.

Keep useful subsystem names such as `arena_`, `lexer_`, `parser_`, and
`codegen_`. Local helpers and user programs do not need a brand prefix.
Do not rename LLVM, libc, or timbr interfaces: those namespaces belong to their
dependencies. Update repository links and Git remotes together for an
authorized repository rename; preserve unrelated dependency URLs.

`__wky_` names are reserved for compiler-generated code. The backend entry
points `wky_main`, `wky_globals_init`, and `wky_trap` are also reserved. User
declarations must not impersonate these symbols. C runtime definitions share
their emitted symbol names so bitcode linking does not require an adapter.
Derive literal prefix and LLVM name lengths with `sizeof("literal") - 1`;
never hard-code a length next to a name that can change.

Use `snake_case` for C functions, variables, and files; `PascalCase` for named
types; and `UPPER_SNAKE_CASE` for constants and configuration macros. A compiler
context can be called `c` in lowering functions or `compiler` in the driver.
Stable diagnostic numbers (`E0001`, etc.) remain independent of branding.

## Whisky declaration syntax

Put an explicit type before the name for variables, parameters, struct fields,
and typed error-handler bindings. Use `let name = expression;` only for type
inference. `let` requires an initializer; it does not make a value dynamically
typed. Do not add suffix type annotations or combine `let` with an explicit type.

```wky
fn i64 read(ref<i64> input) { return input[0]; }
fn int main() {
    int count = 10;
    let inferred = 10;
    owner<i64> values = own(count);
    ref<i64> view = ref_of(values);
    let another_view = ref_of(values);
    arena pool = arena();
    chan<i64> queue = make_chan(16);
    [4]i64 samples;
    []i64 window = samples[0..4];
    owner<owner<i64>> rows = own(2);
    const i64 LIMIT = 64;
    orbit int next := count + 1;
    return 0;
}
```

Named types and aliases follow the same rule: `Buffer values = own(4);` or
`Box(i64) box = {value: 42};`. Prefer `fn ReturnType name(Type parameter)` in
new examples. Typed handlers use `dregs (Error error)`; `dregs (error)` retains
the default integer payload. Destructuring with `let` infers each binding's type.
Colons in struct field labels, destructuring renames, named call arguments, and
control-flow labels are unrelated to type annotations.

Comma-separated declarations initialize in source order: `let x = 2, y = 10;`
or `i64 x = 2, y = 10;`. Inferred bindings need individual initializers.
Use `orbit name := expression;` for a read-only derived expression, not a
mutable cached binding. Const qualifies inline storage; crossing an owner/ref
indirection does not make its referent immutable.
Orbit callees, including custom index getters, must have verified pure effects.

Use `option<T>`/`result<T, E>` and `some`/`none`/`ok`/`err` for typed optional
and fallible values. Propagate with `try(expression)`, or handle errors with
`filter { ... } dregs (Error error) { ... }`. Explicitly move owning payloads.

Raw operations belong inside `unsafe { ... }`. A function whose callers must
establish raw preconditions is `unsafe fn` (or `#[unsafe]`); a function can
keep a safe interface when it establishes those preconditions internally.
Prefer the smallest boundary whose assumptions can be reviewed. `unchecked`
requires an unsafe context and does not disable managed identity checks.

Use the language's printing and memory operations instead of calling private
`__wky_` helpers. The compiler distinguishes generated helper calls from source
calls; a private name does not grant a source call privileged access. Formatted
byte arrays and slices retain their lengths, while raw C-string pointers require
an unsafe context.
Coroutine handles are `handle` values: `sip(task)` resumes, `drop(value)` yields,
and `cancel(task)` destroys. Do not use Rust-style declaration annotations.

## Formatting and file organization

Use C11, `.clang-format`, and `.editorconfig`. Follow the surrounding indentation
when making a focused change; avoid reformatting unrelated code. Keep headers
self-contained, use include guards, and put private declarations in the relevant
subsystem rather than expanding the public compiler API.

Split files by responsibility when that makes ownership or invariants clearer.
Keep runtime code in `src/runtime/`, executable language contracts in `tests/`,
and independent benchmark models in `scripts/bench_oracles.py`. Generate runtime
bitcode headers in the build directory. Build outputs, caches, and measurement
reports must stay in ignored directories.

## Correctness and performance

Document ownership, bounds, and lifetime assumptions at interface boundaries.
Preserve the distinction between owners, bounded references, and stability
guards. Check overflow and allocation failure before committing a mutation.
Do not bypass safety checks to improve a timing result.

Avoid work in hot paths unless it is required by semantics: prefer compile-time
facts, reuse metadata, and keep allocation and instrumentation deliberate. Changes
to representations or calling conventions must update both lowering and runtime
contracts. Gate metrics so normal builds do not pay for measurement.

Regression tests need independent expected behavior. Preserve `.wky.out`
fixtures byte-for-byte except when an intentional behavior change requires a
reviewed update. A memory-safety test must prove the intended diagnostic or trap;
a crash alone is not success. Rejection tests should pin stable diagnostic codes.

Benchmarks must rebuild all implementations, validate outputs before timing,
use equivalent workloads for comparisons, and report samples, toolchains, and
median/MAD. Keep sanitizers and correctness counters outside timed runs. Hosted
CI validates correctness; it does not establish a performance ranking.
