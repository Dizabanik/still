# Basic types and variables

Whisky is statically typed with local type deduction. Primitive scalar types have fixed bit-widths and deterministic representations across all supported target platforms.

## Primitive types

| Type | Bit width | Representation | Example literal |
|:---|:---:|:---|:---|
| `bool` | 1 | Boolean logical value | `true`, `false` |
| `char` | 8 | ASCII / UTF-8 code unit | `'a'`, `'\n'`, `'\0'` |
| `i8`, `i16`, `i32`, `i64` | 8, 16, 32, 64 | Two's complement signed integers | `-42`, `100_000` |
| `u8`, `u16`, `u32`, `u64` | 8, 16, 32, 64 | Unsigned modular integers | `(u64)42`, `(u32)0xFF` |
| `f16` | 16 | IEEE 754 half-precision float | `1.5f16` |
| `bf16` | 16 | Brain floating-point format | `1.25bf16` |
| `f32` | 32 | IEEE 754 single-precision float | `3.14159f32`, `0.5` |
| `f64` | 64 | IEEE 754 double-precision float | `2.718281828459045` |
| `str` | 128 | String slice view `{ ptr, len }` | `"hello world"` |

`int` is a built-in alias for `i32`.

### Integer literal bases and digit separators

Integer literals support decimal, hexadecimal (`0x`), binary (`0b`), and octal (`0o`) bases. Underscores can be placed between digits to improve readability:

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

## Variable declarations

Explicit variable declarations place the type before the variable name:

```wky
i32 counter = 0;
f64 ratio = 1.618;
bool active = true;
```

Type inference uses the `let` keyword:

```wky
let counter = 0;
let ratio = 1.618;
let active = true;
```

Inferred variables determine their static type from the initializing expression. A `let` declaration requires an initializer. Uninitialized variables must specify their type explicitly:

```wky
i32 uninit_count;
```

### Multiple declarations

Declarations separated by commas initialize from left to right. Later initializers can read earlier bindings:

```wky
import stdc;

fn i32 main() {
    let x = 2, y = 10, total = x + y;
    i64 first = 3, second = 4;

    println("total={total} sum={first + second}");
    return 0;
}
```

This syntax is also valid in C-style loop headers:

```wky
for (let i = 0, limit = 10; i < limit; i += 1) {
    // loop body
}
```

### Constants

Constants use the `const` keyword, with either an explicit type or an inferred type:

```wky
import stdc;

const i64 MAX_CLIENTS = 10000;
const f64 EPSILON = 1e-6;
const TIMEOUT_SECS = 30;

fn i32 main() {
    println("MAX_CLIENTS={MAX_CLIENTS} EPSILON={EPSILON:.6f}");
    return 0;
}
```

Constant storage cannot be reassigned or moved. Inline array elements and struct fields inherit the constant qualifier.

## Type conversions and arithmetic policies

Explicit numeric conversions use C-style casts: `(TargetType)expression`.

```wky
import stdc;

fn i32 main() {
    i32 int_val = 42;
    f64 float_val = (f64)int_val;
    u8 byte_val = (u8)int_val;

    println("float={float_val:.2f} byte={byte_val}");
    return 0;
}
```

Unsigned integer operations (`u8`, `u16`, `u32`, `u64`) wrap modulo the bit width of the type. Signed integer operations (`i8`, `i16`, `i32`, `i64`) check for overflow by default. Explicit functions such as `wrap_*` or `sat_*` specify wrapping or saturated arithmetic when requested.

## Derived bindings: `orbit`

An `orbit` binding is a read-only expression that evaluates in its declaring lexical scope whenever it is read. It tracks mutations to its referenced variables without requiring manual dependency caches or callbacks:

```wky
import stdc;

fn i32 main() {
    i32 x = 2;
    orbit total := x + 10;
    println("{total}");

    x = 5;
    println("{total}");

    {
        i32 x = 100;
        println("{total} {x}");
    }

    return 0;
}
```

Output:
```
12
15
15 100
```

Lexical shadowing inside the inner block does not redirect the orbit binding's original dependency on the outer `x`.

Orbit initializers must be side-effect free, may call verified `pure fn` functions, and must produce copyable scalar or value types.
