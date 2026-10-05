# Coroutines & Channels

Whisky provides native, cooperative, stackless concurrency primitives built on top of LLVM coroutine intrinsics: `brew` (coroutine generation), `sip` (resumption / yielding), `drop` (cancellation / cleanup), and `chan<T>` (bounded channel ring buffers).

---

## 1. Coroutines: `brew`, `sip`, and `drop`

A coroutine is created using the `brew` expression, which compiles to an LLVM coroutine handle:

```wky
import stdc;

fn i32 count_generator() {
    println("Yielding 1");
    sip 1;
    println("Yielding 2");
    sip 2;
    println("Yielding 3");
    return 3;
}

fn i32 main() {
    let task = brew count_generator();

    let v1 = sip task;
    let v2 = sip task;
    let v3 = sip task;

    println("Received: {v1}, {v2}, {v3}");
    drop task;
    return 0;
}
```

### Stack Frame Elision
Unlike traditional runtime coroutine libraries that allocate a full stack or call `malloc` on every invocation, `still` statically analyzes coroutine frame lifetimes. Coroutine frames are allocated directly inside the caller's stack frame whenever possible, eliminating heap allocation completely.

---

## 2. Channels: `chan<T>`

Channels allow type-safe message passing between coroutines and tasks:

```wky
import stdc;

fn i32 main() {
    // Allocate a channel with capacity 64
    chan<i32> ch = make_chan(64);

    // Send values
    ch <- 100;
    ch <- 200;

    // Receive values
    let val1 = <-ch;
    let val2 = <-ch;

    println("Received: {val1}, {val2}");
    return 0;
}
```

### Power-of-Two Ring Buffers
Under the hood, `make_chan(cap)` rounds capacity up to a power of two. Channel head and tail pointers index directly using a bitwise mask (`head & mask`) rather than an integer division (`head % cap`). This eliminates costly hardware division instructions and keeps pipeline throughput high.

---

## 3. The `select` Statement

The `select` statement waits on multiple channel operations simultaneously:

```wky
import stdc;

fn i32 main() {
    chan<i32> c1 = make_chan(16);
    chan<str> c2 = make_chan(16);

    c1 <- 42;

    select {
        val = <-c1 => {
            println("Received from c1: {val}");
        }
        msg = <-c2 => {
            println("Received from c2: {msg}");
        }
        else => {
            println("No channel ready");
        }
    }

    return 0;
}
```

---

## 4. Benchmark Performance (`b6_chan`)

In benchmark `b6_chan`, a multi-stage pipeline of coroutines passing messages across capacity-64 channel rings was benchmarked against C (using `ucontext`) and Rust (using `std::sync::mpsc` + threads):

| Language | Wall Time (5 Runs) | vs Whisky |
|:---|:---:|:---:|
| **Whisky** | **0.011s** | **Baseline** |
| **C (`ucontext`)** | 0.098s | 8.9x slower |
| **Rust (`mpsc`)** | 0.225s | 20.4x slower |

Whisky avoids thread-context switches, kernel traps, and heap allocations, achieving near-zero latency cooperative multitasking.
