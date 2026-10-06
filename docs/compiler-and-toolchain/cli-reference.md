# Compiler CLI reference

The `still` compiler driver provides options for code generation, optimization passes, debugging, and diagnostics.

## Synopsis

```sh
still [options] <source.wky>
```

## Options summary

| Flag | Argument | Default | Description |
|:---|:---|:---|:---|
| `-o` | `<name>` | Source stem | Output executable filename |
| `-c` | None | Off | Compile to bitcode (`output.bc`) without linking |
| `-O0`, `-O1`, `-O2`, `-O3` | None | `-O2` | Optimization level passed to the LLVM pass manager |
| `--lto` | None | Off | Enable link-time optimization |
| `--pgo-gen` | `[=file]` | `default.profraw` | Instrument binary for profile generation |
| `--pgo-use` | `=<file>` | None | Use recorded profile data for optimization |
| `--bounds-check` | `<safe\|always\|never>` | `safe` | Array and slice bounds-checking policy |
| `--emit-hash` | None | Off | Emit SHA-256 hash of the generated module |
| `--check` | None | Off | Validate syntax and types without linking or writing artifacts |
| `--memory-metrics` | None | Off | Instrument reference validations and stability guards |
| `--optimization-report` | `[=file]` | `optimization.json` | Write static IR optimization facts |
| `--diagnostic-format` | `<text\|json>` | `text` | Diagnostic output format |
| `--explain` | `<E####\|W####>` | None | Explain a diagnostic code |
| `--format` | None | Off | Format source and print canonical output to stdout |
| `--format-check` | None | Off | Check formatting and exit with error if unformatted |
| `--debug`, `-g` | None | Off | Trap on runtime bounds violations and emit debug metadata |
| `--test` | None | Off | Run functions marked with `#[test]` instead of `main` |
| `--color` | `<auto\|always\|never>` | `auto` | Configure ANSI color output in diagnostics |
| `--version` | None | None | Print compiler version and exit |
| `-h`, `--help` | None | None | Display help screen and exit |

## Detailed flag descriptions

### `-o <name>`
Sets the output binary path. If omitted, the executable defaults to the base name of the input source file (for example, `app.wky` compiles to `./app`).

### `-c`
Emits an LLVM bitcode file (`output.bc`) and stops before invoking the system linker.

### `-O0` to `-O3`
Configures the LLVM PassBuilder optimization pipeline:
* `-O0`: Disables optimizations for faster compilation and step debugging.
* `-O1`: Basic optimization passes.
* `-O2`: Balanced optimization level with loop vectorization and instruction combining (default).
* `-O3`: Aggressive optimization with loop unrolling, function inlining, and SIMD vectorization.

### `--lto`
Enables link-time optimization. Passes `-flto` and link-time optimization flags to LLVM and the system linker to enable whole-program dead code elimination and cross-module inlining.

### `--pgo-gen[=file]` and `--pgo-use=<file>`
Enables profile-guided optimization:
1. Build an instrumented binary:
   ```sh
   still --pgo-gen=run.profraw app.wky -o app_prof
   ```
2. Run the workload to gather profile statistics:
   ```sh
   ./app_prof
   llvm-profdata merge -output=run.profdata run.profraw
   ```
3. Recompile using profile data:
   ```sh
   still -O3 --pgo-use=run.profdata app.wky -o app_opt
   ```

### `--bounds-check=<safe|always|never>`
Controls runtime index bounds checks for arrays and slices:
* `safe`: Emits bounds checks unless proven safe by constant analysis or loop induction proofs.
* `always`: Enforces checks on every indexing operation.
* `never`: Omits bounds checks in generated code.

### `--emit-hash`
Computes the SHA-256 cryptographic digest of the compiled bitcode module and prints the 64-character hexadecimal string.

### `--check`
Runs parsing, type analysis, and safety checks on the input source file without emitting object files or invoking the linker.

### `--memory-metrics`
Instruments managed reference operations and stability scopes to record validation counts and active allocations.

### `--optimization-report[=<file>]`
Writes static intermediate-representation metrics and transformation records to a JSON report file (default: `optimization.json`).

### `--diagnostic-format=<text|json>`
Controls diagnostic formatting. `json` emits machine-readable structured diagnostic events containing file locations, severity, message, and error codes.

### `--explain <E####|W####>`
Displays detailed documentation and resolution guidance for a specific compiler error or warning code.

### `--format` and `--format-check`
* `--format`: Parses the source file and outputs canonically formatted source text to standard output.
* `--format-check`: Validates formatting against the canonical style, exiting with status code 1 if reformatting is required.

### `--test`
Replaces the standard program entry point with a test harness that finds and runs all functions annotated with `#[test]`, printing individual test results.
