# Basic Types & Variables

Whisky is statically typed with full type deduction. All primitive types have fixed bit-widths and deterministic representations across all target platforms.

---

## 1. Primitive Types

| Type | Bit-Width | Description | Example Literal |
|:---|:---:|:---|:---|
| `bool` | 1 | Boolean logical value | `true`, `false` |
| `char` | 8 | ASCII / UTF-8 code unit | `'a'`, `'\n'`, `'\0'` |
| `i8`, `i16`, `i32`, `i64` | 8, 16, 32, 64 | Two's complement signed integers | `-42`, `100_000` |
| `u8`, `u16`, `u32`, `u64` | 8, 16, 32, 64 | Unsigned modular integers | `42u`, `0xFFu32` |
| `f16` | 16 | IEEE 754 half-precision float | `1.5f16` |
| `bf16` | 16 | Brain floating-point (ML format) | `1.25bf16` |
| `f32` | 32 | IEEE 754 single-precision float | `3.14159f32`, `0.5` |
| `f64` | 64 | IEEE 754 double-precision float | `2.718281828459045` |
| `str` | 128 | String slice view `{ ptr, len }` | `"hello world"` |

### Literal Bases & Digit Separators
Integer literals support decimal, hexadecimal (`0x`), binary (`0b`), and octal (`0o`) notation. Underscores `_` can be placed anywhere between digits for readability:

```wky
import stdc;

fn i32 main() {
    let dec = 1_000_000;
    let hex = 0xDEAD_BEEF;
    let bin = 0b1010_1011;
    let oct = 0o755;

    println("dec={dec} hex={hex:x} bin={bin} oct={oct}");
    return 0;
}
```

---

## 2. Variables & Constants

Variables are declared with `let` or with an explicit type annotation:

```wky
import stdc;

fn i32 main() {
    // Type-inferred variables
    let count = 10;
    let pi = 3.14159;
    let is_ready = true;

    // Explicitly typed variables
    i64 big_id = 9876543210;
    u8 byte_val = 255;
    f32 factor = 1.25f32;

    // Mutation
    count = count + 5;
    println("count={count} big_id={big_id} byte_val={byte_val}");
    return 0;
}
```

### Compile-Time Constants (`const`)
Constants are evaluated at compile time. They must have a constant initializer and do not occupy runtime stack or memory slots:

```wky
import stdc;

const i64 MAX_USERS = 10000;
const f64 EPSILON = 1e-6;

fn i32 main() {
    println("MAX_USERS={MAX_USERS} EPSILON={EPSILON:.6f}");
    return 0;
}
```

---

## 3. Type Conversions & Casting

Whisky requires explicit casting (`as`) between different numeric types to prevent accidental precision loss or unexpected sign extensions:

```wky
import stdc;

fn i32 main() {
    let int_val: i32 = 42;
    let float_val: f64 = int_val as f64;
    let byte_val: u8 = int_val as u8;

    println("float={float_val:.2f} byte={byte_val}");
    return 0;
}
```

### Unsigned Arithmetic Wrapping
In Whisky, unsigned integer arithmetic (`u8`, `u16`, `u32`, `u64`) modularly wraps on overflow, conforming to standard two's-complement modular arithmetic without undefined behavior. Signed integer arithmetic (`i8`, `i16`, `i32`, `i64`) adheres to standard optimizing sign semantics.
