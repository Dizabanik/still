# Struct and impl readiness

The current struct/impl system is **not production-ready**. Common examples
work, but visibility, declaration validation, and method resolution have
correctness defects. Passing the compiler regression suite does not close
these gaps.

## Reproduce the audit

```sh
python3 scripts/audit_structs.py --still build-cmake/still \
    --json build-cmake/struct-readiness.json
```

The checklist lives in `tests/struct-readiness.json`. It contains 35 independent
programs, compiled at O0, O2, and O3 with bounds checking enabled. Run cases use
specified output oracles; rejection cases require a particular diagnostic.
Compiler crashes, timeouts, acceptance of invalid source, and incorrect output
all fail. Each compilation has a private working directory.

The initial native ARM64 audit on 2026-10-06 produced **48 PASS and 57 FAIL**:
16 capabilities passed at every optimization level, and 19 requirements failed
at every level. This audit intentionally returns a nonzero exit status while
defects remain. It is separate from CTest's passing regression contracts and
has no expected-failure exemptions. Reports record compiler and checklist hashes.

## What passed

Verified capabilities include field defaults and spread initialization, value
and explicitly unsafe pointer receivers, associated functions and constants,
single/multiple/nested generic struct specialization, forward field types,
recursive pointer types, explicit embedding, nominal type mismatch rejection,
and const receiver mutation rejection. Direct private field/method access is
rejected across an imported module.

Checked storage can call a value receiver through `items[0].get()`. An explicit
`ref<Point>` receiver works through `Point.change(view, 11)` and performs checked
access without an unsafe block.

## Verified gaps

| Area | Observed behavior | Required behavior |
| --- | --- | --- |
| Method overloading | An i32/f64 overload pair prints `11 11` where the independent oracle is `11 22`. | Resolve by the complete receiver/argument signature and call the correct body. |
| Privacy: initialization | Positional initialization writes a private field from another module. | Apply visibility rules to every initialization form. |
| Privacy: destructuring | `let {hidden} = create()` extracts a private field across modules. | Apply visibility rules to destructuring. |
| Privacy: promotion | Promoted private fields and methods are accessible across modules. | Validate every step of a promoted path and the final member. |
| Duplicate declarations | Duplicate fields, structs, method signatures, and associated constants are accepted. | Diagnose collisions before generating LLVM objects. |
| Ambiguous embedding | Two equally applicable promoted fields or methods are accepted. | Require explicit qualification or report ambiguity. |
| Embedding boundaries | A named ordinary struct field also promotes its members. | Distinguish `Inner;` embedding from `Inner member;` composition. |
| Promotion depth | A deeper method declared through the first embedded branch wins over a shallower one in a later branch. | Resolve by minimum depth, then diagnose equal-depth ambiguity. |
| Promoted field types | Printing a promoted `str` field fails with an incompatible value type diagnostic; explicit qualification works. | Preserve the declared type through member promotion and inference. |
| Declaration order | A call before its impl declaration fails with “cannot find function”. | Collect declarations before resolving method calls and inferring returns. |
| Recursive value layout | `struct Bad { Bad child; }` is accepted when unused. | Reject non-finite by-value layouts with a source diagnostic. |
| Field capacity | A 65-field struct is accepted, then accessing field 65 reports that it does not exist. | Support the complete layout, or diagnose the limit at the declaration. |
| Safe mutation ergonomics | Ordinary `self*` field mutation requires an unsafe block. | Provide checked receiver binding for routine safe methods. |
| Checked receiver dispatch | `view.change(11)` fails for `ref<Point>` although the explicit associated call works. | Integrate checked receiver types into method lookup. |

The duplicate declaration probes do not depend on runtime behavior. Acceptance
alone demonstrates that the collision diagnostics are missing. The recursive
value probe establishes missing declaration validation; it does not claim that
every use of a recursive value layout succeeds.

## Additional design and implementation work

These points come from reviewing the compiler and are not counted as passing
or failing executable audit cases:

* A coherent interface/constraint system for generic APIs is absent. Generic
  structs specialize, but there is no declared protocol conformance, constrained
  impl system, associated type system, or documented specialization/coherence
  policy. These are design choices to settle, not reasons to add inheritance.
* Layout/export contracts need a defined ABI, explicit alignment/packing rules
  where needed, and C/C++ aggregate calling-convention validation. Natural LLVM
  layout and scalar C imports do not establish general struct interoperability.
* User-defined construction/destruction protocols and their interaction with
  moves, partial initialization, error propagation, and effects need a defined
  contract. `new` is currently an ordinary associated function; automatic cleanup
  supports managed fields but is not a general custom resource protocol.
* Declaration tables use fixed capacities: 128 struct names, 32 generic struct
  templates, 32 instantiations per template, 8 type parameters, 512 signatures,
  and 256 registered impl methods. Several paths silently stop registering
  entries. Replace these limits with growing tables or explicit diagnostics.
* Debug info, incremental/separate compilation, and public API compatibility
  need broader struct-specific coverage before promising a stable language ABI.

## Order of work

First build a declaration pass with duplicate, finite-layout, and capacity
validation. Then route initialization, destructuring, explicit member access,
and promotion through one visibility-aware resolver. Complete overload and
checked receiver resolution, independent of declaration order. Finally settle
generic constraints, layout/resource contracts, and add the corresponding
cross-platform acceptance and interoperability tests.

These checks belong in compiler analysis; successful direct method calls and
field access can retain their current static dispatch and native layouts.
