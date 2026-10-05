# Structs, Methods & Impls

Whisky uses explicit struct declarations combined with separate `impl` blocks for defining methods, associated functions, and associated constants.

---

## 1. Struct Declarations & Field Defaults

Structs define named collections of fields. Fields can have default initializers:

```wky
import stdc;

struct ServerConfig {
    str host;
    i32 port;
    i32 max_conns;
}

fn i32 main() {
    let cfg = ServerConfig {
        host: "127.0.0.1",
        port: 8080,
        max_conns: 1000,
    };

    println("Listening on {cfg.host}:{cfg.port} (max {cfg.max_conns})");
    return 0;
}
```

---

## 2. Methods and Receiver Semantics

Methods are defined inside `impl StructName` blocks. A method takes an explicit `self` receiver as its first parameter:

* `self`: Passed by value (copied). Ideal for small, immutable types like vectors or coordinates.
* `self*`: Passed by pointer (reference). Ideal for mutating the receiver in-place or avoiding copies of large structs.

```wky
import stdc;

struct Point {
    f64 x;
    f64 y;
}

impl Point {
    // Value receiver (immutable)
    fn f64 distance_sq(self) {
        return self.x * self.x + self.y * self.y;
    }

    // Pointer receiver (mutable in-place)
    fn void scale(self*, f64 factor) {
        self.x = self.x * factor;
        self.y = self.y * factor;
    }
}

fn i32 main() {
    let pt = Point { x: 3.0, y: 4.0 };
    println("dist_sq = {pt.distance_sq():.1f}");

    pt.scale(2.0);
    println("scaled = ({pt.x:.1f}, {pt.y:.1f})");
    return 0;
}
```

Output:
```
dist_sq = 25.0
scaled = (6.0, 8.0)
```

---

## 3. Associated Functions & Constructors

Functions defined in an `impl` block without a `self` receiver are **associated functions**. They are invoked using the type name directly (`StructName.func_name`):

```wky
import stdc;

struct Matrix2 {
    f64 m00;
    f64 m01;
    f64 m10;
    f64 m11;
}

impl Matrix2 {
    // Associated constructor
    fn Matrix2 identity() {
        return Matrix2 {
            m00: 1.0, m01: 0.0,
            m10: 0.0, m11: 1.0,
        };
    }
}

fn i32 main() {
    let id = Matrix2.identity();
    println("m00={id.m00:.1f} m11={id.m11:.1f}");
    return 0;
}
```

---

## 4. Associated Constants

Constants scoped to a struct type are declared inside the `impl` block and accessed via `StructName.CONST_NAME`:

```wky
import stdc;

struct Mat4 {
    [16]f32 data;
}

impl Mat4 {
    const DIM = 4;
    const SIZE = 16;
}

fn i32 main() {
    println("Mat4 Dimension: {Mat4.DIM}x{Mat4.DIM}, Size: {Mat4.SIZE}");
    return 0;
}
```

---

## 5. Struct Composition & Method Promotion

Whisky supports composition by embedding one struct directly inside another without inheritance overhead. Methods from embedded structs are promoted to the enclosing struct automatically:

```wky
import stdc;

struct Position {
    f64 x;
    f64 y;
}

impl Position {
    fn void move_by(self*, f64 dx, f64 dy) {
        self.x = self.x + dx;
        self.y = self.y + dy;
    }
}

struct Entity {
    i64 id;
    Position pos;
}

fn i32 main() {
    let e = Entity {
        id: 101,
        pos: Position { x: 0.0, y: 0.0 },
    };

    // move_by is promoted from pos to Entity
    e.pos.move_by(5.0, 10.0);
    println("Entity {e.id} at ({e.pos.x:.1f}, {e.pos.y:.1f})");
    return 0;
}
```
