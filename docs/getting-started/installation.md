# Installation and toolchain setup

`still` is the compiler for Whisky. It compiles `.wky` source files to native machine code through the LLVM C API.

## Prerequisites

The compiler requires the following dependencies:
* LLVM and Clang 21
* A C11 compiler
* CMake 3.28 or later
* Python 3.9 or later
* A POSIX operating system (Linux x86-64 or macOS ARM64)

Rust is required only if you run the comparative benchmark suite. Refer to [INSTALL.md](../../INSTALL.md) in the repository root for detailed package setup, binary release instructions, and checksum verification.

## Building from source

Clone the repository with submodules:

```sh
git clone --recurse-submodules https://github.com/Dizabanik/still.git still
cd still
```

If you already cloned without submodules, initialize them:

```sh
git submodule update --init --recursive
```

### macOS configuration

Install prerequisites through Homebrew and point your environment to the LLVM 21 toolchain:

```sh
brew install llvm@21 cmake ninja
export PATH="$(brew --prefix llvm@21)/bin:$PATH"
export CC=/usr/bin/clang
```

### Linux configuration

On Ubuntu 24.04, install LLVM 21 from the official LLVM APT repository, then place `/usr/lib/llvm-21/bin` first on your `PATH`.

### Compiling the compiler

Configure and build with CMake and Ninja:

```sh
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --parallel
```

Verify that the compiler binary runs:

```sh
./build-cmake/still --version
./build-cmake/still --help
```

You can also build using `make -j4`. Both build systems generate embedded runtime bitcode headers with the configured LLVM toolchain.

## Running verification tests

Run the test suite through CTest:

```sh
ctest --test-dir build-cmake --output-on-failure --parallel 2
```

Compile and run the hello world example:

```sh
./build-cmake/still examples/hello.wky -o build-cmake/hello
./build-cmake/hello
```

Local CMake builds enable `-DSTILL_NATIVE_CPU=ON` by default to use the host CPU features. When building compiler binaries for distribution across different machines, pass `-DSTILL_NATIVE_CPU=OFF`.
