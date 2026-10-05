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

Explicit declarations put the type before the name: `Type name = expression;`.
`int` is an alias for `i32`, so `int x = 10;` is valid. Use
`let name = expression;` to infer the type from the initializer. The inferred
type stays fixed; `let` does not introduce dynamic typing. An inferred binding
needs an initializer. For an uninitialized binding, write `Type name;`.

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

The same type-first rule applies to managed memory, channels, and composite
types:

```wky
fn i32 main() {
    owner<i64> values = own(2);
    values[0] = 20;
    values[1] = 22;
    ref<i64> view = ref_of(values);
    let alias_view = ref_of(values);
    arena pool = arena();
    ref<i64> item = arena_new(pool, 1);
    chan<i64> queue = make_chan(16);
    [4]i64 samples;
    []i64 window = samples[0..4];
    owner<owner<i64>> rows = own(2);
    println("{view[0] + alias_view[1]}");
    return 0;
}
```

Constructors such as `own(count)` and `arena_new(pool, count)` need a declared
element type. Expressions that already have a type, such as `ref_of(values)`,
`clone(values)`, and `move(values)`, work with inference. Function parameters use
`fn i64 read(ref<i64> input)`, and typed error handlers use
`dregs (Error error)`. Type annotations after a name are not accepted.

### Constants (`const`)
Use `const Type name = expression;` for an explicit type, or
`const name = expression;` for inference. Literal and foldable initializers
can be evaluated at compile time. Local bindings can also hold runtime values,
such as `const ref<i64> view = ref_of(values);`. A constant reference does not
freeze the storage it refers to or transfer ownership.

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

Numeric conversions use C-style casts: `(TargetType)expression`. Checked
conversions diagnose constant failures or trap when a runtime value is outside
the target type's range:

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

### Unsigned Arithmetic Wrapping
Unsigned integer arithmetic (`u8`, `u16`, `u32`, `u64`) wraps modulo the type's
width. Signed arithmetic (`i8`, `i16`, `i32`, `i64`) checks overflow by default.
Use explicit `wrap_*` or `sat_*` operations when wrapping or saturation is
intended.
