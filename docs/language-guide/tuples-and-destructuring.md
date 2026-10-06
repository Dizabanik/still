# Tuples and destructuring

Whisky provides first-class tuples and pattern destructuring. Tuples compile to standard C aggregate layouts that scalarize directly into CPU registers during optimization passes.

## First-class tuples

A tuple groups heterogeneous values together without requiring an explicit struct declaration:

```wky
import stdc;

fn i32 main() {
    let pair = (42, "hello", 3.14);

    let int_part = pair.0;
    let str_part = pair.1;
    let flt_part = pair.2;

    println("int={int_part} str={str_part} flt={flt_part:.2f}");
    return 0;
}
```

Tuple fields can be accessed with dot-index notation (`pair.0`, `pair.1`) or underscore notation (`pair._0`, `pair._1`).

## Returning tuples from functions

Functions declare tuple return types using `(T1, T2, ...)`:

```wky
import stdc;

fn (i64, i64) minmax([4]i64 items) {
    let min_val = items[0];
    let max_val = items[0];

    for (i in 1..4) {
        if (items[i] < min_val) {
            min_val = items[i];
        }
        if (items[i] > max_val) {
            max_val = items[i];
        }
    }

    return (min_val, max_val);
}

fn i32 main() {
    [4]i64 data = {12, 45, 3, 89};
    let bounds = minmax(data);

    println("min={bounds.0} max={bounds.1}");
    return 0;
}
```

## Tuple destructuring

The `let (a, b) = tuple_expr;` syntax unpacks tuple elements into local variables:

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

## Struct destructuring

Whisky supports named, renamed, and positional destructuring for structs.

### Named field destructuring

Extract fields into local variables matching the field names:

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

### Renamed field destructuring

Bind fields to custom local variable names using `:`:

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

### Positional struct destructuring

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

## Array destructuring

Fixed-size arrays can be unpacked into individual elements positionally:

```wky
import stdc;

fn i32 main() {
    [2]i32 coords = {10, 20};
    let (x, y) = coords;

    println("x={x} y={y}");
    return 0;
}
```

## Machine code scalarization

Tuples and destructuring patterns do not allocate heap memory or introduce pointer indirection. During compilation, LLVM's scalar replacement pass decomposes tuple and struct components into independent SSA scalar values, placing them directly into CPU registers.
