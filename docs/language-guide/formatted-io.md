# High-Performance Formatted I/O

Whisky features compile-time string interpolation coupled with an ultra-high-speed buffered runtime that outperforms both C's `printf` and Rust's `BufWriter` formatting.

---

## 1. Syntax & String Interpolation

Use `println(...)` to print formatted text with an automatic trailing newline, or `print(...)` without a newline. Expressions inside `{}` are evaluated at compile time and formatted based on their concrete data type:

```wky
import stdc;

fn i32 main() {
    let name = "Whisky";
    let iterations = 1_000_000;
    let speedup = 4.69;

    println("Project {name}: {iterations} runs, speedup = {speedup:.2f}x");
    return 0;
}
```

Output:
```
Project Whisky: 1000000 runs, speedup = 4.69x
```

---

## 2. Format Specifiers

Format specifiers can follow an expression separated by a colon (`:`):

| Specifier | Description | Example Input | Output |
|:---|:---|:---|:---|
| `{v:.Nf}` | Fixed-point float with $N$ fractional digits | `println("{pi:.2f}")` | `3.14` |
| `{v:0Nd}` | Zero-padded integer of width $N$ | `println("{42:04d}")` | `0042` |
| `{v:x}` | Lowercase hexadecimal representation | `println("{255:x}")` | `ff` |
| `{v:X}` | Uppercase hexadecimal representation | `println("{255:X}")` | `FF` |
| `{{` and `}}` | Escapes literal `{` and `}` characters | `println("{{literal}}")` | `{literal}` |

### Complete Formatting Example

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

---

## 3. Architecture: Why Whisky Formats Faster Than C and Rust

In standard benchmarks printing 1,000,000 formatted lines (`bench/b14_format`):
* **C `printf` took 0.122s**
* **Rust `BufWriter<StdoutLock>` took 0.068s**
* **Whisky took 0.026s** (2.6x faster than Rust, 4.7x faster than C)

Whisky achieves this through five structural optimizations:

### 1. Compile-Time Typed Desugaring
Rather than parsing a format string at runtime, the Whisky parser desugars `println("val={v:.2f}")` into direct typed AST function calls:
* `__wky_print_str("val=", 4)`
* `__wky_print_f64_prec(v, 2)`
* `__wky_print_nl()`

Zero format-string parsing occurs at runtime.

### 2. Zero-Allocation 64KB User Buffer
The runtime maintains a static 64KB output buffer (`__wky_io_buf`). Output strings are copied into user memory and flushed to the operating system in large batches, avoiding small-buffer thrashing and frequent kernel `write()` system calls.

### 3. Zero Per-Call Mutex Locking
Standard libc `printf` invokes `flockfile(stdout)` and `funlockfile(stdout)` on every call (1,000,000 lock/unlock cycles). Whisky locks once during flushing and checks TTY line-buffering status automatically via `isatty(1)`.

### 4. Branch-Free Radix-10 2-Digit Integer Lookup
Integer formatting (`__wky_print_u64`) uses a 100-entry two-digit ASCII table (`__wky_digits_2`), formatting 64-bit integers in ~5 CPU cycles without repeated hardware divisions.

### 5. Exact 128-Bit Fixed-Point Float Rendering
For fixed float precision (`{v:.2f}`), exact 128-bit scaling and IEEE-754 round-to-nearest/even arithmetic produces 100% byte-for-byte identical output to libc `snprintf` in pure bit-arithmetic.
