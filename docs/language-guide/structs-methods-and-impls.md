# Structs, methods, and impl blocks

Whisky provides struct declarations for defining data layouts, with methods and associated functions declared in separate `impl` blocks.

## Struct declarations and field initialization

Structs declare named fields with explicit types:

```wky
import stdc;

struct ServerConfig {
    str host;
    i32 port;
    i32 max_connections;
}

fn i32 main() {
    let cfg = ServerConfig {
        host: "127.0.0.1",
        port: 8080,
        max_connections: 1000,
    };

    println("Listening on {cfg.host}:{cfg.port} (max {cfg.max_connections})");
    return 0;
}
```

Fields are laid out contiguously according to their natural data alignment.

## Methods and receiver semantics

Methods are defined inside an `impl StructName` block. The first parameter specifies how the instance is passed:

* `self`: Passed by value (copied). Used for small, read-only instances.
* `self*`: Passed by pointer. Used to mutate the receiver in place or avoid copying large structures.

```wky
import stdc;

struct Point {
    f64 x;
    f64 y;
}

impl Point {
    fn f64 distance_squared(self) {
        return self.x * self.x + self.y * self.y;
    }

    fn void translate(self*, f64 dx, f64 dy) {
        self.x += dx;
        self.y += dy;
    }
}

fn i32 main() {
    let pt = Point { x: 3.0, y: 4.0 };
    println("dist_sq = {pt.distance_squared():.1f}");

    pt.translate(2.0, 1.0);
    println("translated = ({pt.x:.1f}, {pt.y:.1f})");
    return 0;
}
```

Output:
```
dist_sq = 25.0
translated = (5.0, 5.0)
```

## Associated functions and constructors

Functions declared in an `impl` block without a `self` parameter are associated functions. They are invoked using the type name:

```wky
import stdc;

struct Matrix2 {
    f64 m00;
    f64 m01;
    f64 m10;
    f64 m11;
}

impl Matrix2 {
    fn Matrix2 identity() {
        return Matrix2 {
            m00: 1.0, m01: 0.0,
            m10: 0.0, m11: 1.0,
        };
    }
}

fn i32 main() {
    let mat = Matrix2.identity();
    println("m00={mat.m00:.1f} m11={mat.m11:.1f}");
    return 0;
}
```

## Associated constants

Constants associated with a struct are declared inside the `impl` block and accessed via `StructName.CONSTANT_NAME`:

```wky
import stdc;

struct Mat4 {
    [16]f32 data;
}

impl Mat4 {
    const DIM = 4;
    const TOTAL_ELEMENTS = 16;
}

fn i32 main() {
    println("Dimension: {Mat4.DIM}x{Mat4.DIM}, Elements: {Mat4.TOTAL_ELEMENTS}");
    return 0;
}
```

## Struct embedding and promotion

Whisky supports struct composition through embedding. Placing an unadorned struct type as a field embeds that struct directly into the outer layout, promoting its fields and methods to the outer struct:

```wky
import stdc;

struct Person {
    str name;
    u32 age;
}

impl Person {
    fn u32 get_age(self) {
        return self.age;
    }

    fn void have_birthday(self*) {
        self.age += 1;
    }
}

struct Employee {
    Person;
    u32 badge;
}

fn i32 main() {
    Employee emp = Employee {
        Person: Person { name: "Alice", age: 30 },
        badge: 1042,
    };

    println("Name: {emp.name}, Age: {emp.age}, Badge: {emp.badge}");
    println("Method promotion: age={emp.get_age()}");

    emp.have_birthday();
    println("After birthday: age={emp.age}");

    emp.Person.age = 25;
    println("Explicit access: age={emp.Person.age}");
    return 0;
}
```

### Collision rules

Direct fields and methods on the outer struct shadow promoted fields or methods with the same name. Promoted members from the embedded struct remain accessible by qualifying through the embedded type name (for example, `emp.Person.age`).
