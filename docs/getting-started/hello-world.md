# Hello World & First Steps

This guide walks through writing, compiling, and running your first Whisky program.

---

## 1. Writing Your First Program

Create a new file named `hello.wky`:

```wky
import stdc;

fn i32 main() {
    println("Hello, Whisky!");
    return 0;
}
```

### Key Elements of the Program:
* `import stdc;`: Imports standard C library definitions and hooks the runtime.
* `fn i32 main()`: The program entry point. Functions specify their return type before the function name.
* `println(...)`: Built-in high-performance formatted printing with automatic newline appending.
* `return 0;`: Returns status code `0` to the operating system.

---

## 2. Compiling and Running

Compile the source file into a native executable using `still`:

```bash
still hello.wky -o hello
```

Run the compiled executable:

```bash
./hello
# Output:
# Hello, Whisky!
```

---

## 3. String Interpolation

Whisky features first-class compile-time string interpolation. Expressions enclosed in `{}` are evaluated, formatted according to their concrete type, and printed without any runtime format-string parsing overhead:

```wky
import stdc;

fn i32 main() {
    let language = "Whisky";
    let version_major = 0;
    let version_minor = 1;
    let speed_multiplier = 4.69;

    println("Language: {language} v{version_major}.{version_minor}");
    println("Speedup vs C printf: {speed_multiplier:.2f}x");
    return 0;
}
```

Compile and run:

```bash
still hello.wky -o hello && ./hello
# Output:
# Language: Whisky v0.1
# Speedup vs C printf: 4.69x
```

---

## 4. Compilation Modes

`still` provides options for tuning compilation, safety, and optimization:

### Development & Debugging
Enable runtime bounds checking for all array and slice indexing:
```bash
still --bounds-check=always hello.wky -o hello
```

### Production Release
Enable aggressive LLVM `-O3` optimizations and Link-Time Optimization (LTO):
```bash
still -O3 --lto hello.wky -o hello
```

### Inspecting LLVM Bitcode
To emit LLVM bitcode without invoking the system linker, pass `-c`:
```bash
still -c hello.wky
# Generates output.bc
```
