# Ownership, raw access, and typed errors

Explicit declarations use `Type name`. Inferred bindings use `let name = expression` to determine a fixed static type. Ownership is visible in the type and in transfer operations. The language does not use garbage collection, reference counting headers, or lifetime annotation syntax.

## Managed storage

```wky
import stdc;

fn i32 main() {
    owner<i64> values = own(2);
    values[0] = 20;
    values[1] = 22;

    ref<i64> first = ref_of(values);
    ref<i64> second = ref_of(values);
    first[0] += 1;

    println("{second[0]} {second[1]}");

    stable(first) {
        println("{first[0] + first[1]}");
    }
    return 0;
}
```

`owner<T>` owns zero-initialized heap storage. Local owners clean up automatically at lexical scope exit.

`ref<T>` is a bounded, non-owning view. Mutable aliases are allowed. A reference access validates allocation identity, generation, thread, and bounds before loading or storing data. A stale view traps even if the allocator reused its virtual address. References do not extend an allocation's lifetime.

Global owning values clean up on normal program termination through an exit callback. Explicit `release` controls early deallocation. Process abort does not run cleanups.

### Transfer and cloning

`move(value)` transfers ownership and clears the source binding. It is required for assignment, function arguments, returns, owned enum variant matching, and channel messages. Using a moved local owner produces a compile error. Nullable managed slots can be inspected at runtime with `allocated(value)`.

`clone(value)` deep-copies owned allocations. Borrowed reference fields within cloned structures remain aliases to their original targets. Owning coroutine handles and arenas cannot be cloned, including when nested inside aggregates.

`release(value)` frees an owning value early. Replacing an owning field drops its previous value. Structs, fixed arrays, tagged enums, options, results, channels, and coroutine handles participate in this cleanup. Cleanup follows lexical last-in, first-out order. Child allocations follow reverse acquisition order.

The `@noalloc` attribute checks callees before optimization. Cleanup of handles or arenas can run user callbacks, so those operations are rejected inside a function marked `noalloc`. Cleanup and transfer of ordinary owned buffers remain allocation free.

### Stability scopes: `stable` and `try_stable`

`stable(view) { ... }` protects an allocation from invalidation or deallocation during the enclosed block. It does not exclude mutable aliases:

```wky
stable(view) {
    println("{view[0]}");
}
```

`try_stable(view) { ... } else { ... }` tests view validity and handles stale views without aborting:

```wky
try_stable(view) {
    println("valid: {view[0]}");
} else {
    println("view is stale");
}
```

`resize(owner, count)` alters allocation length, grows capacity geometrically, and invalidates existing views into that buffer. Removed owned elements are destroyed.

`try_own`, `try_clone`, and `try_resize` expose allocation failure as fallible operations instead of aborting, preserving existing values if an allocation cannot be fulfilled.

## Raw memory and unsafe boundaries

Raw pointer access, pointer arithmetic, casts, raw slice construction, foreign function calls, inline assembly, and unchecked blocks require an explicit unsafe boundary:

```wky
import stdc;

unsafe fn i32 read_raw(i32* address) {
    return address[0];
}

fn i32 main() {
    i32 value = 42;
    unsafe {
        printf("%d\n", read_raw(&value));
    }
    return 0;
}
```

An `unsafe fn` declaration indicates that callers must verify its preconditions. `@unsafe` can also be applied as an attribute. An `unsafe { ... }` block permits raw operations inside an otherwise safe function. Unsafe blocks do not disable managed reference lifetime or bounds checks.

`unchecked { ... }` blocks must be nested within an unsafe context. Taking a mutable address or view of immutable inline storage is rejected, including implicit array decay and pointer-receiver method calls on constant storage.

Formatted byte arrays and slices print exactly their declared length, including
embedded null bytes, and evaluate their source once. Printing a raw character
pointer scans a C string and requires `unsafe`. Source-written calls to private
`__wky_` helpers do not receive compiler privileges; managed memory entry points
are reserved for generated code. Large formatting widths and precisions are
handled without exceeding the print runtime's buffers.

## Options and results

`option<T>` provides `Some(T)` and `None`. `result<T, E>` provides `Ok(T)` and `Err(E)`. Values are constructed using `some(v)`, `none()`, `ok(v)`, and `err(e)`:

```wky
import stdc;

fn result<i64, i32> read_value(i64 input) {
    if (input < 0) {
        return err(7);
    }
    return ok(input + 1);
}

fn result<i64, i32> process(i64 input) {
    let value = try(read_value(input));
    return ok(value * 2);
}

fn i32 main() {
    let outcome = process(20);
    let final_val = match outcome {
        Ok(v) => v,
        Err(e) => -(i64)e,
    };
    println("result = {final_val}");
    return 0;
}
```

Constructing or propagating scalar options and results requires no heap memory.

`try(expression)` evaluates its operand once. On success, it unwraps the payload. On failure, it returns `Err(error)` or `None` from the enclosing fallible function, or transfers control to the innermost matching `dregs` handler. Scope cleanups and active `defer` blocks execute before the error transfers.

Fallible functions must return on all reachable code paths.

## Scoped error handling: `filter` and `dregs`

Block-level typed error handling uses `filter` and `dregs` blocks:

```wky
import stdc;

struct ErrorPayload {
    i32 code;
    i32 line;
}

fn i32 main() {
    filter {
        println("starting work");
        ErrorPayload payload = ErrorPayload { code: 404, line: 12 };
        press err = payload;
        println("unreachable");
    } dregs (ErrorPayload err) {
        println("caught code={err.code} line={err.line}");
    }
    return 0;
}
```

Output:
```
starting work
caught code=404 line=12
```

`press err = payload;` transfers control to the matching typed `dregs` handler. If the enclosing function returns a `result<T, E>`, an unhandled `press` returns `Err(payload)`. Control transfer does not use stack unwinding or exception tables.
