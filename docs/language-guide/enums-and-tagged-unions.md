# Enums & Tagged Unions

Whisky provides both simple enumerations and **tagged unions** (discriminated unions / sum types). Variants can carry arbitrary typed payloads packed into inline storage without heap allocations.

---

## 1. Simple Enums

Simple enums define a set of named integer constants:

```wky
import stdc;

enum Direction {
    North,
    East,
    South,
    West,
}

fn i32 main() {
    let d = Direction.East;

    match d {
        Direction.North => { println("Heading North"); }
        Direction.East  => { println("Heading East"); }
        Direction.South => { println("Heading South"); }
        Direction.West  => { println("Heading West"); }
    }

    return 0;
}
```

---

## 2. Tagged Unions with Payloads

Variants can carry one or more data payloads. Payload storage is sized to the largest variant and aligned automatically:

```wky
import stdc;

enum Shape {
    Circle(i64),
    Rect(i64, i64),
    Point,
}

fn i64 compute_area(Shape s) {
    match s {
        Shape.Circle(r) => {
            return 3 * r * r;
        }
        Shape.Rect(w, h) => {
            return w * h;
        }
        Shape.Point => {
            return 0;
        }
    }
}

fn i32 main() {
    let c = Shape.Circle(10);
    let r = Shape.Rect(4, 5);
    let p = Shape.Point;

    println("Circle area: {compute_area(c)}");
    println("Rect area: {compute_area(r)}");
    println("Point area: {compute_area(p)}");
    return 0;
}
```

Output:
```
Circle area: 300
Rect area: 20
Point area: 0
```

---

## 3. Memory Layout & Safety

Tagged unions in Whisky are laid out as a contiguous struct containing:
1. **Tag Discriminant**: A 32-bit integer indicating which variant is active.
2. **Payload Union**: Raw storage sized to $\max(\text{sizeof}(\text{variant\_payload}))$.

```
+----------------+-----------------------------------------------+
|  Tag (32-bit)  |  Payload Storage (max variant size, aligned)  |
+----------------+-----------------------------------------------+
```

Because payload storage is entirely inline on the stack, creating and matching tagged unions involves **zero heap allocation (`malloc`) and zero garbage collection overhead**.

In benchmark `b11_tagged_union`, 20,000,000 iterations of tagged union creation and pattern matching complete in **0.019s**, matching native C bitwise union benchmarks.
