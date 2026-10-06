# Standard library: `std.io`

The `std.io` module provides basic stream output routines for standard output and standard error.

## Writing to standard output: `std.io.puts`

`std.io.puts` writes a string directly to `stdout`:

```wky
import stdc;

fn i32 main() {
    std.io.puts("Output via std.io.puts\n");
    return 0;
}
```

## Writing to standard error: `std.io.eputs`

`std.io.eputs` writes a string directly to `stderr` (file descriptor 2).

Standard error writes bypass the stdout user-space buffer and invoke the `write` system call directly. This ensures error messages appear immediately without interleaving behind buffered stdout output:

```wky
import stdc;

fn i32 main() {
    std.io.eputs("Fatal error encountered\n");
    return 1;
}
```

## Function summary

| Function | Output stream | Buffering | Formatting support |
|:---|:---|:---|:---|
| `println(...)` | `stdout` | 64KB user buffer | Full string interpolation (`{}`) |
| `print(...)` | `stdout` | 64KB user buffer | Full string interpolation (`{}`) |
| `std.io.puts` | `stdout` | System line buffering | Raw string |
| `std.io.eputs` | `stderr` | Unbuffered direct write | Raw string |
