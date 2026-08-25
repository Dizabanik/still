# Benchmarks

Each `<name>.kawa` is paired with byte-identical-output twins:
`<name>.c` (C) and `<name>.rs` (Rust). `scripts/bench.py` builds the Kawa
source, times all three best-of-N, and reports deltas.

## Prebuilt twins

The harness expects executables named `<prefix>_c` and `<prefix>_rs`
(e.g. `b5_c`, `b5_rs`) in this directory. Build flags used for the
recorded numbers:

    # C twins
    cc -O3 -march=native -o <prefix>_c <name>.c
    # b6 needs ucontext on macOS:
    cc -O3 -march=native -Wno-error=incompatible-function-pointer-types \
       -o b6_c b6_chan.c

    # Rust twins
    rustc -C opt-level=3 -C target-cpu=native -C codegen-units=1 \
          -o <prefix>_rs <name>.rs

## Notes

- `b6_chan`: pipeline of brew{} coroutines over cap-64 rings. The Kawa
  version drains the output ring per scheduler round (`while c4.count > 0`)
  and marks `step` as `pure fn` so the sent value never spills through a
  coroutine frame. Twins: ucontext coroutines (C) and std::sync::mpsc +
  threads (Rust). On macOS swapcontext costs ~870ns (sigprocmask et al.
  per switch), so Kawa's userspace resumes win big here; on Linux the gap
  narrows but does not close. At N=1e8 Kawa finishes ~9x ahead.
- Outputs must be byte-identical across the three languages -- that is
  the correctness check tying the twins together.
