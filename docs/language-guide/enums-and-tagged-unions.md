# Enums and tagged unions

Enums in Whisky are nominal tagged unions with inline payload storage. A variant may be a unit value or carry one or more typed payloads.

## Declaration and matching

Variants are declared inside an `enum` block. Pattern matching must cover all variants or include an `else` arm:

```wky
import stdc;

enum Shape {
    Circle(f64),
    Rect(f64, f64),
    Point
}

fn f64 area(Shape shape) {
    return match shape {
        Circle(radius) => 3.141592653589793 * radius * radius,
        Rect(width, height) => width * height,
        Point => 0.0,
    };
}

fn i32 main() {
    let c = Shape.Circle(10.0);
    let r = Shape.Rect(4.0, 5.0);
    let p = Shape.Point;

    println("circle={area(c):.2f} rect={area(r):.2f} point={area(p):.2f}");
    return 0;
}
```

Patterns can be qualified with the enum name (`Shape.Point`) or unqualified (`Point`). Unqualified variants resolve against the target expression type. The compiler rejects duplicate variants, arity mismatches, missing arms, and conversions between distinct enum types even when their binary layouts match.

## Owning payloads

When an enum variant contains an owning type (`owner<T>`), the enum becomes move-only:

```wky
import stdc;

enum Message {
    Data(owner<i64>),
    Empty
}

fn i32 main() {
    owner<i64> buffer = own(1);
    buffer[0] = 42;

    Message message = Message.Data(move(buffer));
    Message copied = clone(message);

    match move(message) {
        Data(value) => {
            println("value={value[0]}");
        }
        Empty => {}
    }

    release(copied);
    return 0;
}
```

Matching with `match move(message)` transfers the active payload into the arm bindings. Those bindings clean up when their arm block exits or returns.

An `else` arm destroys an unbound owned payload when the match statement finishes. Replacement, moves, and explicit `clone` inspect the active discriminator tag so inactive union memory is never treated as owning headers.

## Memory layout

On supported 64-bit targets, the enum layout consists of an `i64` tag followed by an 8-byte aligned union sized to the largest variant payload.

For example, an enum containing a 32-byte owner header and scalar alternatives occupies 40 bytes. Scalar enum construction and pattern matching require no heap memory. Standard library types like `option<T>` and `result<T, E>` use this tagging and cleanup model.
