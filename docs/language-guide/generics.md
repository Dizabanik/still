# Generics and monomorphization

Whisky supports generic structs and generic `impl` blocks. The compiler monomorphizes generic definitions at compile time, emitting specialized, concrete machine code for each distinct type combination without runtime virtual method tables or dynamic boxing.

## Multi-parameter generic structs

Generic structs declare type parameter names within parentheses following the struct name:

```wky
import stdc;

struct Pair(A, B) {
    A first;
    B second;
}

fn i32 main() {
    let p = Pair(i32, str) { first: 101, second: "Order" };
    println("first={p.first} second={p.second}");
    return 0;
}
```

Whisky supports up to eight type parameters per generic struct.

## Generic impl blocks

Methods and associated constructors for generic types are declared in `impl` blocks parameterized over the corresponding type variables:

```wky
import stdc;

struct Pair(A, B) {
    A first;
    B second;
}

impl(A, B) Pair(A, B) {
    fn Pair(A, B) new(A f, B s) {
        return Pair(A, B) { first: f, second: s };
    }

    fn A get_first(self) {
        return self.first;
    }

    fn B get_second(self) {
        return self.second;
    }

    fn Pair(B, A) swap(self) {
        return Pair(B, A) { first: self.second, second: self.first };
    }
}

fn i32 main() {
    let p = Pair(i64, str).new(42, "answer");
    println("Original: ({p.get_first()}, {p.get_second()})");

    let swapped = p.swap();
    println("Swapped: ({swapped.get_first()}, {swapped.get_second()})");
    return 0;
}
```

Output:
```
Original: (42, answer)
Swapped: (answer, 42)
```

## Method chaining on generic instances

Generic method return types are tracked by the type deduction pass, allowing method calls to chain across changing type parameters:

```wky
import stdc;

struct Pair(A, B) {
    A first;
    B second;
}

impl(A, B) Pair(A, B) {
    fn Pair(A, B) new(A f, B s) {
        return Pair(A, B) { first: f, second: s };
    }

    fn Pair(B, A) swap(self) {
        return Pair(B, A) { first: self.second, second: self.first };
    }
}

fn i32 main() {
    let p = Pair(i32, i64).new(10, 20);

    // Initial: Pair(i32, i64) -> swap: Pair(i64, i32) -> swap: Pair(i32, i64)
    let p2 = p.swap().swap();

    println("p2 = ({p2.first}, {p2.second})");
    return 0;
}
```

## Monomorphization model

Each concrete instantiation of a generic struct produces a dedicated type symbol in the generated LLVM module.

* Struct fields are laid out contiguously according to target alignment rules for the specialized types.
* Function bodies are specialized for the concrete argument and return types, enabling direct call inlining and register passing.
* Unused specializations are omitted from the final binary by the compiler and linker.
