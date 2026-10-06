# Standard library: `std.process`

The `std.process` module provides access to process execution arguments and process termination.

## Command-line arguments

Whisky captures `argc` and `argv` from the host operating system entry point during program startup.

### Functions

* `std.process.arg_count() -> i32`: Returns the number of command-line arguments passed to the process.
* `std.process.arg_at(i32 index) -> str`: Returns the argument at the specified index as a string slice view.

### Example

```wky
import stdc;

fn i32 main() {
    let count = std.process.arg_count();
    println("Total arguments: {count}");

    for (i in 0..count) {
        let arg = std.process.arg_at(i);
        println("  arg[{i}] = {arg}");
    }

    return 0;
}
```

Running the executable:
```sh
./myprog foo bar
```

Output:
```
Total arguments: 3
  arg[0] = ./myprog
  arg[1] = foo
  arg[2] = bar
```

## Process termination: `std.process.exit`

`std.process.exit(i32 code)` terminates the current process immediately with the specified exit status:

```wky
import stdc;

fn void check_limit(i32 val) {
    if (val < 0) {
        std.io.eputs("Error: negative value not permitted\n");
        unsafe { std.process.exit(1); }
    }
}

fn i32 main() {
    check_limit(-1);
    return 0;
}
```

The compiler attaches LLVM's `noreturn` attribute to `std.process.exit`, enabling dead-code elimination and omitting epilogues for code following an exit call.
