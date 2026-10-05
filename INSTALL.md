# Installation

Supported CI targets are Ubuntu 24.04 x86-64 and macOS 15 ARM64. Other POSIX
targets may work but are not covered by the release matrix. Windows and cross
compilation are not currently supported.

## macOS

Install Xcode Command Line Tools if needed (`xcode-select --install`), then:

```sh
brew install llvm@21 cmake ninja
export PATH="$(brew --prefix llvm@21)/bin:$PATH"
export CC=/usr/bin/clang
```

Apple Clang builds the compiler and ASan/UBSan tests. The embedded runtimes
and generated programs use Clang from the LLVM 21 installation. This avoids
the Homebrew sanitizer startup problem observed on the development host.

## Ubuntu 24.04

These commands use the signed [official LLVM APT repository](https://apt.llvm.org/)
for the pinned Ubuntu `noble` suite:

```sh
sudo apt-get update
sudo apt-get install -y ca-certificates curl gnupg cmake ninja-build python3
curl -fL --retry 3 https://apt.llvm.org/llvm-snapshot.gpg.key -o /tmp/still-llvm.key
gpg --batch --yes --dearmor -o /tmp/still-llvm.gpg /tmp/still-llvm.key
sudo install -m 644 /tmp/still-llvm.gpg /usr/share/keyrings/still-llvm.gpg
echo 'deb [signed-by=/usr/share/keyrings/still-llvm.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main' | sudo tee /etc/apt/sources.list.d/still-llvm.list
sudo apt-get update
sudo apt-get install -y clang-21 llvm-21-dev llvm-21-tools libclang-rt-21-dev lld-21
export PATH="/usr/lib/llvm-21/bin:$PATH"
export CC=clang
```

Both systems should report LLVM 21 from `llvm-config --version` and have
`clang`, `FileCheck`, and `llvm-size` available on `PATH`.

## Build and install from source

```sh
git clone --recurse-submodules https://github.com/Dizabanik/still.git still
cd still
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --parallel
ctest --test-dir build-cmake --output-on-failure --parallel 2
cmake --install build-cmake --prefix "$HOME/.local"
```

The release source archive includes the pinned timbr sources, so it also
builds without Git or submodule fetching. GitHub's automatically generated
source archives omit submodule contents; use the attached `*-source.tar.gz`.

Local builds default to `-DSTILL_NATIVE_CPU=ON` for performance. Set
`-DSTILL_NATIVE_CPU=OFF` when distributing a compiler to other CPUs of the same
architecture. This controls the compiler and its embedded runtime bitcode;
compiled Whisky programs still target the machine running `still`.

To build without test binaries, pass `-DBUILD_TESTING=OFF`. Python remains
required for runtime generation. `make -j4` remains available as an alternate
local build; use a fresh build directory when changing `NATIVE_CPU`.

## Binary releases

Download the archive for your OS and architecture and its `.sha256` file from
[GitHub Releases](https://github.com/Dizabanik/still/releases). Verify before
extracting:

```sh
# Linux
sha256sum --check still-0.1.0-linux-x86_64.tar.gz.sha256
# macOS
shasum -a 256 --check still-0.1.0-macos-arm64.tar.gz.sha256
```

Extract the archive and add its `bin/` directory to `PATH`, or install its
contents under a prefix of your choice. `BUILD.json` records the exact source
and submodule commits, build host, LLVM version, architecture, and CPU policy.
Licenses are under `share/licenses/still/`; documentation is under
`share/doc/still/`.

These archives do **not** bundle LLVM, Clang, or an SDK. Install LLVM/Clang 21
as above, with its `bin/` directory on `PATH`. Linux binaries require the
Ubuntu 24.04 glibc baseline or newer; macOS binaries require macOS 15 or newer
and the matching Homebrew LLVM 21 shared library. The installed compiler
records the toolchain library directory in its runtime search path.
Building from source is the supported option for a different LLVM prefix or
older OS. Rust, Python, CMake, and Ninja are not required to use the binary
compiler; Clang, the LLVM shared library, and host development headers are.
