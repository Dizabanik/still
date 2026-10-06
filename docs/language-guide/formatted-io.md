# Formatted I/O and string interpolation

Whisky compiles string interpolation and formatting directly into typed calls to a buffered I/O runtime.

## Interpolation syntax

Use `println(...)` to print formatted text with a trailing newline, or `print(...)` without a newline. Expressions enclosed in `{}` are evaluated and formatted based on their static types:

```wky
import stdc;

fn i32 main() {
    let name = "Whisky";
    let iterations = 1_000_000;
    let ratio = 1.618;

    println("Project {name}: {iterations} runs, ratio = {ratio:.2f}");
    return 0;
}
```

Output:
```
Project Whisky: 1000000 runs, ratio = 1.62
```

## Format specifiers

Format specifiers follow an expression after a colon (`:`):

| Specifier | Description | Example | Output |
|:---|:---|:---|:---|
| `{v:.Nf}` | Fixed-point float with $N$ fractional digits | `println("{pi:.2f}")` | `3.14` |
| `{v:0Nd}` | Zero-padded integer with width $N$ | `println("{42:04d}")` | `0042` |
| `{v:x}` | Lowercase hexadecimal integer | `println("{255:x}")` | `ff` |
| `{v:X}` | Uppercase hexadecimal integer | `println("{255:X}")` | `FF` |
| `{{` and `}}` | Escaped literal braces | `println("{{val}}")` | `{val}` |

### Combined formatting example

```wky
import stdc;

fn i32 main() {
    i64 id = 42;
    u64 hex_val = 0xABCD;
    f64 ratio = 3.14159265;

    println("ID: {id:05d} | HEX: 0x{hex_val:X} | RATIO: {ratio:.3f}");
    return 0;
}
```

Output:
```
ID: 00042 | HEX: 0xABCD | RATIO: 3.142
```

## Runtime implementation details

1. Compile-time typed desugaring: The parser transforms interpolation expressions into direct AST calls to runtime routines (such as `__wky_print_str`, `__wky_print_f64_prec`, and `__wky_print_nl`). The runtime parses no format strings.
2. Buffered output: The runtime manages a static 64KB thread buffer (`__wky_io_buf`). Output accumulates in user-space memory and flushes to the operating system in larger chunks to reduce write system call frequency.
3. Lock management: The runtime flushes without per-character locking, synchronizing during flush operations and querying terminal line buffering using standard POSIX checks.
4. Two-digit decimal formatting: Integer printing (`__wky_print_u64`) uses a 100-entry two-digit ASCII lookup table (`__wky_digits_2`), formatting 64-bit integers with fewer hardware division instructions.
5. Fixed-point float scaling: Precision specifiers (`{v:.2f}`) use 128-bit scaling and IEEE-754 round-to-nearest/even arithmetic to match standard C library formatting outputs.
