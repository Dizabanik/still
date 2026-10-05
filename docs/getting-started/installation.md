# Installation and toolchain setup

`still` is the compiler for Whisky. It compiles `.wky` source files to native
machine code through the LLVM C API.

The supported toolchain is LLVM/Clang **21**, a **C11** compiler, CMake
**3.28+**, Python **3.9+**, and a POSIX host. Rust is needed only for comparison
benchmarks. See the root [INSTALL.md](../../INSTALL.md) for platform packages,
binary release requirements, checksums, and installation options.

## Build

```sh
git clone --recurse-submodules https://github.com/Dizabanik/still.git still
cd still
```

On macOS:

```sh
brew install llvm@21 cmake ninja
export PATH="$(brew --prefix llvm@21)/bin:$PATH"
export CC=/usr/bin/clang
```

On Ubuntu 24.04, follow the signed LLVM 21 package instructions in
[INSTALL.md](../../INSTALL.md), then put `/usr/lib/llvm-21/bin` first on `PATH`.

```sh
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --parallel
./build-cmake/still --version
./build-cmake/still --help
```

Alternatively, `make -j4` builds `./still`. Both build systems generate embedded
runtime headers using the selected LLVM toolchain. Keep the timbr submodule
initialized with `git submodule update --init --recursive`.

## Verify

```sh
ctest --test-dir build-cmake --output-on-failure --parallel 2
./build-cmake/still examples/hello.wky -o build-cmake/hello
./build-cmake/hello              # prints 42
```

Local CMake builds enable `STILL_NATIVE_CPU` by default. Use
`-DSTILL_NATIVE_CPU=OFF` for compiler distributions; generated programs still
target the machine running `still`.
