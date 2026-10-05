# Slices, Arrays & Memory Safety

Whisky treats memory buffers through two distinct representations: fixed-size contiguous **Arrays** and lightweight, non-owning **Slices**.

---

## 1. Fixed-Size Arrays: `[N]T`

Arrays are fixed-size collections of elements stored contiguously on the stack or in global data:

```wky
import stdc;

fn i32 main() {
    // Array declaration and initialization
    [4]i32 numbers = {10, 20, 30, 40};

    // Indexing
    let first = numbers[0];
    numbers[3] = 99;

    println("first={first} last={numbers[3]} len={numbers.len}");
    return 0;
}
```

The length of an array is a compile-time constant accessible via `.len`.

---

## 2. Slice Views: `[]T`

A slice is a non-owning fat pointer consisting of a pointer to the data and an integer length:

```wky
struct Slice(T) {
    T* data;
    i64 len;
}
```

Slices can view arrays, heap memory, or sub-regions of other slices:

```wky
import stdc;

fn i64 sum_slice([]i64 s) {
    let total = 0;
    for i in 0..s.len {
        total = total + s[i];
    }
    return total;
}

fn i32 main() {
    [6]i64 buffer = {1, 2, 3, 4, 5, 6};

    // Slice a sub-region [start..end]
    []i64 sub_view = buffer[1..5]; // elements 2, 3, 4, 5

    println("sub_view.len = {sub_view.len}");
    println("sub_view sum = {sum_slice(sub_view)}");
    return 0;
}
```

Output:
```
sub_view.len = 4
sub_view sum = 14
```

---

## 3. Bounds-Checking Policy

Whisky provides fine-grained control over bounds-checking via the `--bounds-check` compiler flag:

| Policy | Flag | Description |
|:---|:---|:---|
| **Safe (Default)** | `--bounds-check=safe` | Checks indices at runtime unless proven safe by static analysis. |
| **Always** | `--bounds-check=always` | Checks all array and slice accesses at runtime. Violations trap immediately. |
| **Never** | `--bounds-check=never` | Elides all bounds checks (for maximum performance in trusted inner loops). |

### Static Bounds-Check Elimination
Under `--bounds-check=safe` (the default), `still` statically elides bounds checks in two common scenarios:

1. **Constant Folding**: When an index is a constant less than the known array length (e.g. `arr[2]` where `arr` has length 4), the bounds check is proven safe and eliminated at compile time.
2. **Induction Variable Analysis**: In range-based loops (`for i in 0..N`) where `N <= arr.len`, the compiler proves the loop variable cannot exceed the buffer boundary and completely removes the bounds check from the generated loop kernel.

In benchmark `b13_bounds` (51.2M array accesses), Whisky with elided bounds checks runs in **0.013s**, matching unchecked C clock-for-clock.
