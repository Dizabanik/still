# Standard Library: `stdc`

The `stdc` module provides direct, zero-overhead access to the host C standard library and POSIX environment.

---

## 1. Overview

Whisky treats the C standard library as its primary native FFI surface. Importing `stdc` makes standard C symbols callable directly from Whisky code without boilerplate wrapper stubs or overhead:

```wky
import stdc;

fn i32 main() {
    let s = "Hello from Whisky";
    let len = stdc.strlen(s);

    println("String length via stdc.strlen: {len}");
    return 0;
}
```

---

## 2. Common Functions

### Memory Allocation & Copying
```wky
import stdc;

fn i32 main() {
    // Allocate 1024 bytes of heap memory
    let ptr = stdc.malloc(1024);

    // Fill memory
    stdc.memset(ptr, 0, 1024);

    // Free memory
    stdc.free(ptr);

    println("Memory allocated and freed successfully");
    return 0;
}
```

### Mathematical Functions
```wky
import stdc;

fn i32 main() {
    let angle = 0.785398; // pi / 4
    let s = stdc.sin(angle);
    let c = stdc.cos(angle);

    println("sin={s:.4f} cos={c:.4f}");
    return 0;
}
```

---

## 3. ABI & Codegen Guarantees

* **Exact Prototype Mapping**: Known standard C library functions (`strlen`, `strcpy`, `memcpy`, `memcmp`, `memset`, `puts`) are declared with their exact platform signatures in the LLVM module, ensuring correct register routing on x86-64 and ARM64.
* **Direct Symbol Calls**: Function invocations compile directly to single `call` or `bl` assembly instructions without runtime shims or marshalling layers.
* **Optimizer Cooperation**: LLVM recognizes standard C library symbols and can fold constant inputs (e.g. `strlen("abc")` folds to `3` at compile time).
