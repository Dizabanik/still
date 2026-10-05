# Compiler CLI Reference

The `still` driver provides options for code generation, optimization passes, debugging, and profile-guided tuning.

---

## Synopsis

```bash
still [options] <source.wky>
```

---

## Options Summary

| Flag | Argument | Default | Description |
|:---|:---|:---|:---|
| `-o` | `<name>` | Source stem | Output binary filename |
| `-c` | None | Off | Emit LLVM bitcode (`output.bc`) only; skip native linking |
| `-O0`, `-O1`, `-O2`, `-O3` | None | `-O2` | Optimization level passed to LLVM pass manager |
| `--lto` | None | Off | Enable Link-Time Optimization (passes `lto<O%d>` and `-flto`) |
| `--pgo-gen` | `[=file]` | `default.profraw` | Instrument binary for Profile-Guided Optimization generation |
| `--pgo-use` | `=<file>` | None | Optimize binary using recorded profile data file |
| `--bounds-check` | `<safe\|always\|never>` | `safe` | Array and slice bounds-checking policy |
| `--emit-hash` | None | Off | Compute and print cryptographic NIST SHA-256 hash of output module |
| `--test` | None | Off | Compile and run `#[test]` functions instead of `main` |
| `--debug`, `-g` | None | Off | Emit DWARF debug metadata and force runtime traps on violations |
| `--color` | `<auto\|always\|never>` | `auto` | Configure ANSI color output in compiler diagnostics |
| `--version` | None | None | Print compiler version and exit |
| `-h`, `--help` | None | None | Display help screen and exit |

---

## Detailed Flag Descriptions

### `-o <name>`
Specifies the name of the final native executable. If omitted, the executable defaults to the base filename of the source file (e.g., `main.wky` produces `./main`).

### `-c`
Compiles the input file into an LLVM bitcode file (`output.bc`) and halts before invoking the system linker. Useful for inspecting intermediate bitcode representations or custom build pipelines.

### `-O0` to `-O3`
Controls the optimization pipeline. Whisky configures the modern LLVM PassBuilder pipeline:
* `-O0`: Disables optimizations for fastest compilation speed and transparent debugging.
* `-O2`: Balanced optimization level with loop vectorization and instruction combining (default).
* `-O3`: Aggressive optimization enabling unrolling, function inlining, and SIMD vectorization.

### `--lto`
Enables whole-program Link-Time Optimization. Configures LLVM's `lto<O%d>` pass pipeline and instructs the native linker to run link-time dead code elimination and cross-module inlining.

### `--pgo-gen[=file]` and `--pgo-use=<file>`
Implements Profile-Guided Optimization (PGO):
1. **Instrument build**:
   ```bash
   still --pgo-gen=bench.profraw program.wky -o program_prof
   ```
2. **Collect execution profile**:
   ```bash
   ./program_prof
   llvm-profdata merge -output=code.profdata bench.profraw
   ```
3. **Optimized compilation**:
   ```bash
   still -O3 --pgo-use=code.profdata program.wky -o program_opt
   ```

### `--bounds-check=<safe|always|never>`
Controls runtime index bounds checks for arrays and slices:
* `safe`: Emits bounds checks unless eliminated by compile-time constant analysis or loop induction proofs.
* `always`: Enforces checks on every indexing operation.
* `never`: Omits all bounds checks for maximum execution speed in trusted code.

### `--emit-hash`
Computes the NIST SHA-256 cryptographic checksum of the compiled module, printing the 64-character hexadecimal digest. Guarantees deterministic, reproducible compilation artifacts across identical inputs.

### `--test`
Compiles in test-runner mode. Replaces the standard `@main` entry point with a test harness that discovers and executes all functions marked with `#[test]`, reporting pass/fail status per test.
