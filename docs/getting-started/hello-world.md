# Hello world and basic workflow

This guide shows how to write, compile, and run a Whisky program.

## Writing a first program

Create a source file named `hello.wky`:

```wky
import stdc;

fn i32 main() {
    println("Hello, Whisky!");
    return 0;
}
```

The program includes the following components:
* `import stdc;`: Imports C runtime declarations and hooks the standard I/O runtime.
* `fn i32 main()`: The program entry point. Functions specify their return type before the function name.
* `println(...)`: Formatted printing with an automatic trailing newline.
* `return 0;`: Returns status code 0 to the host operating system.

## Compiling and executing

Compile the source file into a native executable using `still`:

```sh
still hello.wky -o hello
```

Run the compiled executable:

```sh
./hello
```

Output:
```
Hello, Whisky!
```

## String interpolation

Expressions enclosed in `{}` inside string literals are evaluated and formatted according to their static types at compile time:

```wky
import stdc;

fn i32 main() {
    let language = "Whisky";
    let version_major = 0;
    let version_minor = 1;
    let ratio = 3.14159;

    println("Language: {language} v{version_major}.{version_minor}");
    println("Ratio formatted: {ratio:.2f}");
    return 0;
}
```

Running this program prints:

```
Language: Whisky v0.1
Ratio formatted: 3.14
```

## Common compiler options

Check syntax and types without generating object files or invoking the linker:

```sh
still --check hello.wky
```

Format source code in place:

```sh
still --format hello.wky
```

Enable full optimizations and link-time optimization:

```sh
still -O3 --lto hello.wky -o hello
```

Force runtime bounds checks on all array and slice indexing operations:

```sh
still --bounds-check=always hello.wky -o hello
```

Emit intermediate LLVM bitcode without linking:

```sh
still -c hello.wky
```
