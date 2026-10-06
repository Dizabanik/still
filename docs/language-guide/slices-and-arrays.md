# Slices and arrays

Whisky handles contiguous sequences through fixed-size arrays and non-owning slice views.

## Fixed-size arrays: `[N]T`

Arrays have compile-time fixed lengths and are stored contiguously on the stack or in static data:

```wky
import stdc;

fn i32 main() {
    [4]i32 numbers = {10, 20, 30, 40};

    let first = numbers[0];
    numbers[3] = 99;

    println("first={first} last={numbers[3]} len={numbers.len}");
    return 0;
}
```

The length of an array is a constant accessible through `.len`.

## Slices: `[]T`

A slice is a fat pointer consisting of a pointer to element data and a 64-bit integer length:

```wky
import stdc;

fn i64 sum_slice([]i64 s) {
    let total = 0;
    for (i in 0..s.len) {
        total += s[i];
    }
    return total;
}

fn i32 main() {
    [6]i64 buffer = {1, 2, 3, 4, 5, 6};

    // Subslice [start..end]
    []i64 sub_view = buffer[1..5];

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

Slices expose two fields:
* `.len`: The number of elements in the slice.
* `.data`: A pointer to the underlying element buffer.

An array can decay into a slice view when passed to a function or assigned to a slice variable. Mutations through the slice mutate the underlying array elements.

### Range slicing syntax

Slices can be constructed using range syntax:
* `arr[start..end]`: Half-open subslice from `start` up to `end - 1`.
* `arr[..end]`: Subslice starting from index 0 up to `end - 1`.
* `arr[start..]`: Subslice starting from `start` through the end of the buffer.
* `arr[start..=end]`: Inclusive subslice from `start` up to and including `end`.

## Bounds-checking policies

The compiler controls runtime index checks through the `--bounds-check` option:

| Mode | Command flag | Behavior |
|:---|:---|:---|
| Safe (default) | `--bounds-check=safe` | Checks indices at runtime unless proven safe by static analysis. |
| Always | `--bounds-check=always` | Checks all array and slice index operations at runtime. Violations trap immediately. |
| Never | `--bounds-check=never` | Omits bounds checks in generated code. |

### Static bounds-check elimination

Under `--bounds-check=safe`, the compiler eliminates bounds checks in two situations:

1. Constant folding: When an index expression is a constant smaller than the statically known array length (for example, `arr[2]` on an array of length 4), the check is proven safe and omitted during lowering.
2. Induction variables: In range-based loops (`for i in 0..N`) where `N <= arr.len`, the induction variable cannot exceed the buffer boundary, so the loop kernel runs without dynamic bounds checks.
