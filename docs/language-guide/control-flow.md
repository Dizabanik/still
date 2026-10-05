# Control Flow

Whisky provides a clear set of control flow primitives designed for predictable execution and optimization.

---

## 1. Conditionals: `if` / `else`

Conditionals evaluate boolean expressions. Parentheses around conditions are optional, but code blocks `{}` are mandatory:

```wky
import stdc;

fn i32 main() {
    let score = 85;

    if score >= 90 {
        println("Grade: A");
    } else if score >= 80 {
        println("Grade: B");
    } else {
        println("Grade: C");
    }

    return 0;
}
```

---

## 2. Loops: `while`

The `while` loop continues executing as long as the condition evaluates to `true`:

```wky
import stdc;

fn i32 main() {
    let i = 0;
    let sum = 0;

    while i < 10 {
        sum = sum + i;
        i = i + 1;
    }

    println("sum={sum}");
    return 0;
}
```

---

## 3. Range-Based Loops: `for .. in`

Whisky includes native range syntax `start..end` (half-open, `[start, end)`), which compiles to zero-overhead counter loops:

```wky
import stdc;

fn i32 main() {
    let total = 0;

    for i in 0..10 {
        total = total + i;
    }

    println("total={total}");
    return 0;
}
```

Range loops also integrate with array and slice iteration without bounds-check overhead:

```wky
import stdc;

fn i32 main() {
    [5]i32 values = {10, 20, 30, 40, 50};
    let acc = 0;

    for v in values {
        acc = acc + v;
    }

    println("acc={acc}");
    return 0;
}
```

---

## 4. Pattern Matching: `match`

The `match` expression provides exhaustive pattern matching over integer values, enum variants, and tagged unions:

```wky
import stdc;

enum State {
    Idle,
    Running,
    Finished,
}

fn i32 main() {
    let s = State.Running;

    match s {
        State.Idle => {
            println("State is Idle");
        }
        State.Running => {
            println("State is Running");
        }
        State.Finished => {
            println("State is Finished");
        }
    }

    return 0;
}
```

For tagged unions with payloads, `match` unpacks payloads into variables directly (see [Enums & Tagged Unions](enums-and-tagged-unions.md)).

---

## 5. Cleanups: `defer`

The `defer` statement schedules a statement or block to run when the enclosing function returns, regardless of which exit path is taken:

```wky
import stdc;

fn void process_data() {
    println("Acquiring resource");
    defer println("Releasing resource");

    println("Performing work inside function");
}

fn i32 main() {
    process_data();
    return 0;
}
```

Output:
```
Acquiring resource
Performing work inside function
Releasing resource
```

Multiple `defer` statements execute in reverse order of declaration (LIFO), ensuring resources are torn down cleanly.
