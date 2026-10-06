# Control flow

Whisky provides conditional branching, loops, pattern matching, and deterministic scope cleanup.

## Conditionals: `if` and `else`

Conditionals evaluate boolean expressions. Parentheses around conditions and brace-delimited blocks are required:

```wky
import stdc;

fn i32 main() {
    let score = 85;

    if (score >= 90) {
        println("Grade: A");
    } else if (score >= 80) {
        println("Grade: B");
    } else {
        println("Grade: C");
    }

    return 0;
}
```

## Loops

### While loops

The `while` loop executes its block as long as its condition evaluates to `true`:

```wky
import stdc;

fn i32 main() {
    let i = 0;
    let sum = 0;

    while (i < 10) {
        sum += i;
        i += 1;
    }

    println("sum={sum}");
    return 0;
}
```

### Range-based loops

Whisky supports half-open ranges (`start..end`, representing `[start, end)`) and inclusive ranges (`start..=end`, representing `[start, end]`):

```wky
import stdc;

fn i32 main() {
    let total_half = 0;
    for (i in 0..10) {
        total_half += i;
    }

    let total_incl = 0;
    for (i in 0..=10) {
        total_incl += i;
    }

    println("half={total_half} incl={total_incl}");
    return 0;
}
```

### Collection and slice iteration

A `for` loop iterates directly over fixed-size arrays and slices:

```wky
import stdc;

fn i32 main() {
    [5]i32 values = {10, 20, 30, 40, 50};
    let acc = 0;

    for (v in values) {
        acc += v;
    }

    println("acc={acc}");
    return 0;
}
```

### C-style for loops

Three-part `for` loops are also supported:

```wky
import stdc;

fn i32 main() {
    let count = 0;
    for (let i = 0; i < 5; i += 1) {
        count += i;
    }
    println("count={count}");
    return 0;
}
```

### Batch iteration

The `batch` statement provides zero-copy iteration across contiguous buffers:

```wky
import stdc;

fn i32 main() {
    [4]i32 items = {1, 2, 3, 4};
    let total = 0;

    batch (item in items) {
        total += item;
    }

    println("total={total}");
    return 0;
}
```

## Pattern matching: `match`

The `match` construct supports exhaustive pattern matching over integer values, enum variants, and tagged union payloads. A match can act as a statement or as an expression that returns a value:

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

    let code = match s {
        State.Idle => 0,
        State.Running => 1,
        State.Finished => 2,
    };

    println("code={code}");
    return 0;
}
```

If variants are not exhaustively covered, an `else` branch must be provided:

```wky
let description = match code {
    1 => "active",
    else => "inactive",
};
```

## Scoped cleanup: `defer`

The `defer` statement schedules a statement or block to execute when the enclosing lexical scope exits. Execution occurs on normal return, loop break or continue, block exit, and error propagation.

Multiple `defer` actions execute in reverse order of declaration (last in, first out):

```wky
import stdc;

fn void run_work() {
    println("start");
    defer println("cleanup 1");
    defer println("cleanup 2");
    println("work done");
}

fn i32 main() {
    run_work();
    return 0;
}
```

Output:
```
start
work done
cleanup 2
cleanup 1
```

### Value capture in defer

By default, a defer block accesses enclosing variables by reference at the moment the defer executes. Specifying capture variables in parentheses (`defer(x)`) evaluates and copies those values at the point the defer is scheduled:

```wky
import stdc;

fn i32 main() {
    i32 a = 100;
    i32 b = 200;

    defer(a) {
        println("captured a={a}");
    }
    defer {
        println("live b={b}");
    }

    a = 111;
    b = 222;
    return 0;
}
```

Output:
```
live b=222
captured a=100
```

### Defer in loops and blocks

`defer` binds to the nearest enclosing block scope. In a loop, deferred operations run at the end of each iteration:

```wky
import stdc;

fn i32 main() {
    for (i in 0..3) {
        defer println("end iter {i}");
        println("begin iter {i}");
    }
    return 0;
}
```

Output:
```
begin iter 0
end iter 0
begin iter 1
end iter 1
begin iter 2
end iter 2
```
