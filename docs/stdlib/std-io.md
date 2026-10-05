# Standard Library: `std.io`

The `std.io` namespace provides basic input and output primitives for writing to standard streams and low-level file descriptors.

---

## 1. Writing to Standard Output: `std.io.puts`

`std.io.puts` writes a null-terminated string to standard output, followed by a newline:

```wky
import stdc;

fn i32 main() {
    std.io.puts("Direct output via std.io.puts");
    return 0;
}
```

---

## 2. Writing to Standard Error: `std.io.eputs`

`std.io.eputs` writes a string directly to `stderr` (file descriptor 2).

Because `stderr` is unbuffered by standard POSIX rules, `std.io.eputs` lowers directly to a raw `write(2, ptr, len)` system call inside a module-local wrapper. This guarantees that error output cannot reorder against buffered stdout streams:

```wky
import stdc;

fn i32 main() {
    std.io.eputs("Error: operation failed");
    return 1;
}
```

---

## 3. Comparison with `println`

| Feature | `println(...)` | `std.io.puts` | `std.io.eputs` |
|:---|:---:|:---:|:---:|
| **Destination** | `stdout` | `stdout` | `stderr` |
| **Formatting / Interpolation** | Yes (`{x}`) | No (Raw string only) | No (Raw string only) |
| **Buffering** | 64KB user buffer | Line buffered | Unbuffered (raw `write`) |
| **Best Used For** | Formatted data & logs | Simple stdout text | Immediate error messages |
