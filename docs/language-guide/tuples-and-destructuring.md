# Tuples & Destructuring

Whisky provides first-class tuples and pattern destructuring. Tuples compile to canonical C-ABI struct layouts that scalarize directly into CPU registers via LLVM's Scalar Replacement of Aggregates (SROA).

---

## 1. First-Class Tuples

A tuple groups heterogeneous values together without requiring an explicit struct definition:

```wky
import stdc;

fn i32 main() {
    // Tuple literal
    let pair = (42, "hello", 3.14);

    // Positional element access (.0, .1, .2)
    let int_part = pair.0;
    let str_part = pair.1;
    let flt_part = pair.2;

    println("int={int_part} str={str_part} flt={flt_part:.2f}");
    return 0;
}
```

---

## 2. Multi-Return Functions

Functions can return tuples directly. Return types use tuple notation `(T1, T2, ...)`:

```wky
import stdc;

fn (i64, i64) stats([4]i64 items) {
    let min_val = items[0];
    let max_val = items[0];

    for i in 1..4 {
        if items[i] < min_val { min_val = items[i]; }
        if items[i] > max_val { max_val = items[i]; }
    }

    return (min_val, max_val);
}

fn i32 main() {
    [4]i64 data = {12, 45, 3, 89};
    let result = stats(data);

    println("min={result.0} max={result.1}");
    return 0;
}
```

---

## 3. Tuple Destructuring

The `let (a, b) = tuple_expr;` syntax unpacks tuple members into local variables in a single statement:

```wky
import stdc;

fn (str, i32) get_user() {
    return ("Alice", 28);
}

fn i32 main() {
    let (name, age) = get_user();

    println("User: {name}, Age: {age}");
    return 0;
}
```

---

## 4. Struct Destructuring

Whisky also supports pattern destructuring for structs.

### Named Field Destructuring
Extract fields directly into local variables of the same name:

```wky
import stdc;

struct Point {
    i64 x;
    i64 y;
}

fn i32 main() {
    let pt = Point { x: 100, y: 200 };

    let Point { x, y } = pt;
    println("x={x}, y={y}");
    return 0;
}
```

### Renamed Field Destructuring
Bind extracted fields to different local variable names:

```wky
import stdc;

struct Point {
    i64 x;
    i64 y;
}

fn i32 main() {
    let pt = Point { x: 15, y: 30 };

    let Point { x: px, y: py } = pt;
    println("px={px}, py={py}");
    return 0;
}
```

### Positional Struct Destructuring
Unpack struct fields positionally in order of their declaration:

```wky
import stdc;

struct RGB {
    u8 r;
    u8 g;
    u8 b;
}

fn i32 main() {
    let color = RGB { r: 255, g: 128, b: 0 };

    let (red, green, blue) = color;
    println("R={red} G={green} B={blue}");
    return 0;
}
```

---

## 5. Performance: Zero-Cost Scalarization

Tuples and destructuring in Whisky produce zero runtime memory allocations or pointer indirections. LLVM scalarizes tuple components into machine registers:

```
mov w0, 42         ; first tuple element in register
adrp x1, .strlit   ; second tuple element in register
```

In benchmark `b15_tuples`, 20,000,000 tuple creation and destructuring iterations execute in **0.034s**, matching hand-tuned C and Rust clock-for-clock.
