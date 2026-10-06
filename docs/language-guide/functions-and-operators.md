# Functions and operator overloading

Functions in Whisky compile ahead of time, adhere to standard platform C ABIs, and emit `nounwind` attributes across the call graph.

## Function declarations

Functions declare their return type before the function name, followed by parameter declarations:

```wky
import stdc;

fn i32 add(i32 a, i32 b) {
    return a + b;
}

fn void greet(str name) {
    println("Hello, {name}!");
}

fn i32 main() {
    let sum = add(10, 20);
    greet("Whisky");
    println("sum={sum}");
    return 0;
}
```

Functions that do not return a value specify `void` as their return type.

## Multi-return functions

Functions can return multiple values using tuple return types:

```wky
import stdc;

fn (i32, i32) divmod(i32 numerator, i32 denominator) {
    let quot = numerator / denominator;
    let rem = numerator % denominator;
    return (quot, rem);
}

fn i32 main() {
    let (q, r) = divmod(17, 5);
    println("17 / 5 = {q} remainder {r}");
    return 0;
}
```

Multi-return tuples are returned in CPU registers according to the SysV or ARM64 calling convention without heap allocations.

## Pure functions: `pure fn`

A function declared with `pure fn` guarantees that it reads only its arguments and creates no memory side effects:

```wky
import stdc;

pure fn f64 square(f64 x) {
    return x * x;
}

fn i32 main() {
    let sq = square(4.0);
    println("sq={sq:.2f}");
    return 0;
}
```

The compiler adds LLVM's `memory(none)` attribute to pure functions, allowing common subexpression elimination, loop hoisting, and dead call elimination.

## Unsafe functions

Functions that perform raw pointer arithmetic, unchecked indexing, inline assembly, or foreign calls require an explicit unsafe designation:

```wky
import stdc;

unsafe fn i32 read_raw(i32* ptr) {
    return ptr[0];
}

@unsafe
fn void write_raw(i32* ptr, i32 val) {
    ptr[0] = val;
}
```

Callers must invoke these functions within an `unsafe { ... }` block or from inside an enclosing unsafe function.

## Attributes

Attributes use `@name` before a declaration. Multiple attributes can occupy one
line or consecutive lines. Visibility can precede or follow the attributes:
`pub @noalloc fn ...` and `@noalloc pub fn ...` have the same meaning.

| Function attribute | Meaning |
| --- | --- |
| `@test` | Include the function in the `--test` runner. |
| `@ignore` | Skip an attributed test. |
| `@unsafe` | Require an unsafe context at calls, like `unsafe fn`. |
| `@noalloc` | Verify that the function does not allocate. |
| `@nocapture` | Verify that pointer arguments do not escape. |
| `@fp_contract` | Permit floating-point contraction. |
| `@fp_reassoc` | Permit floating-point reassociation. |
| `@fp_finite` | Permit finite-only floating-point assumptions. |

`@soa` applies to a struct declaration. Unknown attributes and attributes on
the wrong declaration kind are rejected. Hash-prefixed attributes are no
longer supported; `#` is reserved for future syntax. Comments use `//`.

## Operator overloading

Whisky supports operator overloading through defined method names inside struct `impl` blocks. Arbitrary custom operator symbols or user-defined precedence rules are not supported.

### Supported operator mappings

| Syntax | Implemented method | Description |
|:---|:---|:---|
| `a + b` | `self_add(self, other)` | Addition |
| `a - b` | `self_sub(self, other)` | Subtraction |
| `a * b` | `self_mul(self, other)` | Multiplication |
| `a / b` | `self_div(self, other)` | Division |
| `a % b` | `self_mod(self, other)` or `self_rem(self, other)` | Modulo or remainder |
| `a & b` | `self_bitand(self, other)` | Bitwise AND |
| `a | b` | `self_bitor(self, other)` | Bitwise OR |
| `a ^ b` | `self_bitxor(self, other)` | Bitwise XOR |
| `a << b` | `self_shl(self, other)` | Left bit shift |
| `a >> b` | `self_shr(self, other)` | Right bit shift |
| `-a` | `self_neg(self)` | Unary negation |
| `~a` | `self_bitnot(self)` | Bitwise NOT |
| `!a` | `self_not(self)` | Logical NOT |
| `a == b` | `self_eq(self, other)` | Equality comparison |
| `a != b` | `self_ne(self, other)` | Inequality comparison |
| `a < b` | `self_lt(self, other)` | Less than |
| `a <= b` | `self_le(self, other)` | Less than or equal |
| `a > b` | `self_gt(self, other)` | Greater than |
| `a >= b` | `self_ge(self, other)` | Greater than or equal |
| `a[i]` | `self_index(self, i)` | Index read |
| `a[i] = v` | `self_index_set(self*, i, v)` | Index write |
| `a += b` | Evaluates target once, calls `self_add` | Compound assignment |

Compound assignment operators (`+=`, `-=`, `*=`, `/=`, etc.) desugar automatically to the corresponding binary arithmetic method.

### Complete operator example

```wky
import stdc;

struct Vec2 {
    f64 x;
    f64 y;
}

impl Vec2 {
    fn Vec2 new(f64 x, f64 y) {
        return Vec2 { x: x, y: y };
    }

    fn Vec2 self_add(self, Vec2 other) {
        return Vec2 { x: self.x + other.x, y: self.y + other.y };
    }

    fn Vec2 self_sub(self, Vec2 other) {
        return Vec2 { x: self.x - other.x, y: self.y - other.y };
    }

    fn Vec2 self_neg(self) {
        return Vec2 { x: -self.x, y: -self.y };
    }

    fn bool self_eq(self, Vec2 other) {
        return self.x == other.x && self.y == other.y;
    }
}

fn i32 main() {
    let v1 = Vec2.new(10.0, 20.0);
    let v2 = Vec2.new(5.0, 15.0);

    let v3 = v1 + v2;
    let v4 = -v1;
    let equal = (v1 == v2);

    println("v3 = ({v3.x:.1f}, {v3.y:.1f})");
    println("v4 = ({v4.x:.1f}, {v4.y:.1f})");
    println("equal = {equal}");
    return 0;
}
```

Output:
```
v3 = (15.0, 35.0)
v4 = (-10.0, -20.0)
equal = false
```
