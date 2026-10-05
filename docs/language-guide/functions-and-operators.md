# Functions & Operator Overloading

Functions in Whisky are ahead-of-time compiled, obey platform C ABI standards, and emit `nounwind` attributes across the entire call graph for zero stack-unwinding overhead.

---

## 1. Function Declarations

Functions declare their return type before the function name:

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

---

## 2. Multi-Return Functions

Functions can return multiple values using first-class tuple return syntax:

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

Multi-return functions pass values in CPU registers according to the SysV/ARM64 ABI without heap allocation or hidden out-parameters.

---

## 3. Pure Functions: `pure fn`

A function marked `pure fn` guarantees that it reads only its arguments and produces no side effects:

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

The compiler attaches LLVM's `memory(none)` attribute to `pure fn`, allowing LLVM to aggressively hoist calls out of loops, eliminate common subexpressions, and optimize math kernels.

---

## 4. Whitelisted Operator Overloading

Whisky supports operator overloading through a **whitelisted method name mapping**. Operators cannot be defined with arbitrary symbols or custom precedence; they map directly to canonical method names defined in struct `impl` blocks.

### Supported Operator Table

| Operator | Method Name | Description |
|:---|:---|:---|
| `a + b` | `self_add(self, o)` | Binary addition |
| `a - b` | `self_sub(self, o)` | Binary subtraction |
| `a * b` | `self_mul(self, o)` | Binary multiplication |
| `a / b` | `self_div(self, o)` | Binary division |
| `a % b` | `self_mod(self, o)` / `self_rem(self, o)` | Modulo / Remainder |
| `a & b` | `self_bitand(self, o)` | Bitwise AND |
| `a | b` | `self_bitor(self, o)` | Bitwise OR |
| `a ^ b` | `self_bitxor(self, o)` | Bitwise XOR |
| `a << b` | `self_shl(self, o)` | Left shift |
| `a >> b` | `self_shr(self, o)` | Right shift |
| `-a` | `self_neg(self)` | Unary negation |
| `~a` | `self_bitnot(self)` | Bitwise NOT |
| `!a` | `self_not(self)` | Logical NOT |
| `a == b` | `self_eq(self, o)` | Equality |
| `a != b` | `self_ne(self, o)` | Inequality |
| `a < b` | `self_lt(self, o)` | Less than |
| `a <= b` | `self_le(self, o)` | Less than or equal |
| `a > b` | `self_gt(self, o)` | Greater than |
| `a >= b` | `self_ge(self, o)` | Greater than or equal |
| `a += b` .. `a >>= b` | Compound assignments | Desugared automatically via binary op |

### Complete Example

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
    let are_equal = (v1 == v2);

    println("v3 = ({v3.x:.1f}, {v3.y:.1f})");
    println("v4 = ({v4.x:.1f}, {v4.y:.1f})");
    println("are_equal = {are_equal}");
    return 0;
}
```

Output:
```
v3 = (15.0, 35.0)
v4 = (-10.0, -20.0)
are_equal = false
```
