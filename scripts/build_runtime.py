#!/usr/bin/env python3
import subprocess
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "runtime", "kawa_runtime.c")
BC = os.path.join(ROOT, "src", "runtime", "kawa_runtime.bc")
OUT_H = os.path.join(ROOT, "src", "codegen", "kawa_runtime_bc.h")

# Compile kawa_runtime.c to LLVM bitcode
cmd = ["clang", "-O3", "-emit-llvm", "-c", SRC, "-o", BC]
subprocess.run(cmd, check=True)

with open(BC, "rb") as f:
    data = f.read()

with open(OUT_H, "w") as f:
    f.write("// Auto-generated from src/runtime/kawa_runtime.c. Do not edit directly.\n")
    f.write("#ifndef KAWA_RUNTIME_BC_H\n")
    f.write("#define KAWA_RUNTIME_BC_H\n\n")
    f.write("#include <stddef.h>\n\n")
    f.write(f"static const size_t kawa_runtime_bc_len = {len(data)};\n")
    f.write("static const unsigned char kawa_runtime_bc[] = {\n")
    for i, b in enumerate(data):
        f.write(f"0x{b:02x}, ")
        if (i + 1) % 16 == 0:
            f.write("\n")
    f.write("\n};\n\n")
    f.write("#endif // KAWA_RUNTIME_BC_H\n")

print(f"Generated {OUT_H} ({len(data)} bytes bitcode)")
