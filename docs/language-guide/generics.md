# Generics & Monomorphization

Whisky supports multi-type-parameter generic structs and generic `impl` blocks. Generics compile via compile-time monomorphization: each distinct instantiation produces a specialized, concrete machine-code struct and method suite with zero runtime indirection or virtual method table (vtable) overhead.

---

## 1. Multi-Parameter Generic Structs

Generic structs define type parameters within parentheses after the struct name:

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

Up to 8 type parameters per generic struct are supported.

---

## 2. Generic `impl` Blocks

Methods and constructors for generic types are declared in `impl` blocks:

```wky
import stdc;

struct Pair(A, B) {
    A first;
    B second;
}

impl(A, B) Pair(A, B) {
    // Associated constructor
    fn Pair(A, B) new(A f, B s) {
        return Pair(A, B) { first: f, second: s };
    }

    // Methods
    fn A get_first(self) {
        return self.first;
    }

    fn B get_second(self) {
        return self.second;
    }

    // Swapping type parameters
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

---

## 3. Method Chaining on Generic Types

Generic methods can be chained seamlessly. The compiler infers the intermediate monomorphized return types at each step:

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

    // Swap twice: Pair(i32, i64) -> Pair(i64, i32) -> Pair(i32, i64)
    let p2 = p.swap().swap();

    println("p2 = ({p2.first}, {p2.second})");
    return 0;
}
```

---

## 4. Performance: Zero-Overhead Monomorphization

Each instantiation of a generic struct (e.g. `Pair(i64, i32)`) produces a concrete specialized type symbol (e.g. `Pair__i64__i32`).

* Struct fields are laid out contiguously with exact natural alignment.
* Methods are emitted directly for the target types without boxing, heap pointers, or type erasure.
* In benchmark `b16_generics_multi`, 20,000,000 iterations of generic struct creation, swapping, and field reads finish in **0.020s**, outperforming Rust (0.021s) and matching C (0.021s).
