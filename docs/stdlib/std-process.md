# Standard Library: `std.process`

The `std.process` namespace provides access to process execution metadata, command-line arguments, and process termination.

---

## 1. Command-Line Arguments

Whisky automatically captures `argc` and `argv` from the operating system entry point into private module globals during the `@main` function prologue.

### Functions

* `std.process.arg_count() -> i32`: Returns the number of command-line arguments passed to the program.
* `std.process.arg_at(i32 index) -> str`: Returns the argument at the given index as a string slice view.

### Example

```wky
import stdc;

fn i32 main() {
    let count = std.process.arg_count();
    println("Total arguments: {count}");

    for i in 0..count {
        let arg = std.process.arg_at(i);
        println("  arg[{i}] = {arg}");
    }

    return 0;
}
```

Running the compiled program:
```bash
./myprog foo bar 123
# Output:
# Total arguments: 4
#   arg[0] = ./myprog
#   arg[1] = foo
#   arg[2] = bar
#   arg[3] = 123
```

---

## 2. Process Termination: `std.process.exit`

`std.process.exit(i32 code)` terminates the current process immediately with the specified exit status code.

```wky
import stdc;

fn void validate_input(i32 val) {
    if val < 0 {
        std.io.eputs("Fatal: negative value not permitted\n");
        std.process.exit(1);
    }
}

fn i32 main() {
    validate_input(-5);
    println("This line is never reached");
    return 0;
}
```

The compiler attaches LLVM's `noreturn` attribute to `std.process.exit`, allowing LLVM to eliminate dead code and omit unnecessary epilogues following an exit call.
