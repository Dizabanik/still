# Standard library: `stdc`

The `stdc` module provides access to the host C standard library and POSIX runtime symbols.

## Built-in declarations

Importing `stdc` makes standard C runtime functions callable through compiler-provided prototypes:

```wky
import stdc;

#[unsafe]
fn i32 main() {
    let s = "Hello from Whisky";
    let len = strlen(s);

    println("String length: {len}");
    return 0;
}
```

Raw foreign calls require an unsafe context (`unsafe fn`, `#[unsafe]`, or an `unsafe { ... }` block). Callers must provide valid pointers, buffer sizes, and lifetimes.

### Memory management

```wky
import stdc;

#[unsafe]
fn i32 main() {
    char* buf = malloc(1024);
    memset(buf, 0, 1024);
    free(buf);

    println("Memory allocated and freed");
    return 0;
}
```

## External function declarations

Foreign C functions outside the built-in standard set are declared using `extern "c"`:

```wky
import stdc;

extern "c" fn f64 sqrt(f64 x);
extern "c" fn i32 abs(i32 n);

#[unsafe]
fn i32 main() {
    let root = sqrt(16.0);
    let val = abs(-42);

    println("sqrt={root:.1f} abs={val}");
    return 0;
}
```

Output:
```
sqrt=4.0 abs=42
```

## ABI and lowering properties

* Exact signatures: Built-in declarations (`strlen`, `memcpy`, `memset`, `malloc`, `free`) map directly to platform C calling conventions on x86-64 and ARM64.
* Direct calls: Foreign invocations compile directly to platform call instructions without runtime wrapper shims.
* Optimization: LLVM recognizes standard C library symbols and can fold constant inputs, such as evaluating `strlen("abc")` to `3` at compile time.
