# Language evolution implementation ledger

The acceptance scope is `tests/future-contracts.json` (56 contracts) and
`bench/future-contracts.json` (13 workloads). Existing passing tests do not
establish that these future contracts are implemented.

Work order:

1. Managed allocation identity, bounds, owners, arenas, stable access and
   explicit raw boundaries. Separate runtime and lowering from existing files.
2. Effects and ownership across calls, containers and concurrency protocols.
3. Typed errors, integer policies, text/bytes/C strings and numerical policies.
4. Diagnostics, check/format/reproducibility and optimization reporting.
5. Executable acceptance tests and independently checked, instrumented benchmarks.

## Implemented foundation

- Managed allocation identity: zero-initialized owner/ref/arena allocations,
  byte extents, checked indexing/slicing/forward offsets, process-lived pooled
  descriptors, thread confinement, and generation retirement. The reference
  ABI is 32 bytes. Empty allocations have distinct, non-null identities.
- Affine owner bindings: explicit move/clone, implicit copy rejection, branch
  and loop state tracking, reassignment, and LIFO scope cleanup through return,
  break, continue, filter/press, and ordinary exits.
- Stack owning values: nested structs/fixed arrays use compact static ownership
  layouts for move, deep clone, replacement and scope cleanup. Whole-value heap
  transfers register/detach their owned fields. Owned spreads release replaced
  fields; implicit aggregate copies and borrowing an owned temporary are rejected.
  Clone failure rolls back all new allocations and produces an all-zero value.
- Typed filter/dregs payloads also own their fields: handlers clean up on
  fallthrough or return, or transfer the payload with move. Handler bindings
  remain local to the catch body and participate in affine state checking.
- Heap ownership trees: owner elements and owned fields in heap structs/fixed
  arrays, replacement and extraction, cycle prevention, independent deep clone,
  partial-clone rollback, and iterative cleanup/clone for unbounded depth.
  Runtime drop preflights the complete affected tree before any mutation.
- Subobject references: ref_of on managed scalar/struct fields and fixed arrays
  shares the enclosing allocation's identity while narrowing its byte extent.
  Fixed arrays produce element references; empty arrays have zero extent.
  Stack/raw addresses cannot acquire managed identity.
- Stable and optional access: stable(r) / try_stable(r) with transitive pins.
  Ordinary mutable aliases remain allowed. A protected descriptor cannot be
  freed, removed, relocated, or replaced indirectly through a callback.
- Resize: geometric capacity, generation invalidation on every length change,
  allocation failure preserving data and identity, zero initialization on
  growth, and recursive cleanup of removed owner elements. Retained child
  allocations preserve their independent identities and can remain pinned.
- Numeric policies: checked signed arithmetic, wrapping unsigned arithmetic,
  explicit checked_/wrap_/sat_ operations, division/shift guards, checked
  conversions, lossy conversions, and independent floating-point permissions.
  Comptime evaluation respects widths, short circuiting, and typed rounding.
- Effects: transitive pure, noalloc, and nocapture checking before optimization,
  including memory copies, reachable pointers, and indirect/unknown calls.
  Address provenance follows scalar encoding, enum payload words, selected
  aggregate fields and helper return values. Recursive field permutations join
  conservatively; scalar reads and slice lengths remain ordinary values.
- Aggregate initialization: typed SSA construction, zero omitted elements,
  declared defaults only for missing fields, source-order explicit expressions,
  nested arrays, and checked duplicate/excess fields and spread types.
- Generic specialization: independent concrete AST instances, structural keys,
  nested and recursive helper calls, multiple widths, named-argument inference,
  managed signatures, slice inputs and cleanup scopes. Specialization never
  rewrites the shared template or replaces its caller's type environment.
- Nominal enum checks: matching binary layouts cannot convert distinct enum
  types or forge a managed payload. Unqualified patterns use their target enum.
- Tooling: check-only mode, idempotent token-preserving formatter, stable error
  codes, JSON diagnostics and explanations, byte spans, import-aware source
  locations, canonical cyclic/diamond imports, literal linker arguments,
  and repeatable artifacts with a hash of the emitted bitcode.
- Optimization reports: --optimization-report[=path] records function-level
  IR facts before/after optimization, tagged operations, retained guard
  predicates, and constant checks omitted during lowering. Missing tags/calls
  do not establish runtime elimination. Reports are deterministic, preserve
  native object bytes, and reject input/import/artifact path collisions.

Managed lowering, ownership analysis, numeric policies, effects, aggregate
literals, optimization reporting, import expansion, and formatting have
separate source files.
The embedded memory runtime uses the native CPU and O3, matching the current
native-only object emitter.
Reserved compiler runtime helpers have internal linkage for specialization
and removal of unused entry points. Public source functions keep their ABI.

## Ownership semantics

Allocation and scope cleanup are explicit; the runtime never implicitly copies
an owner. Assigning a new owner drops the old tree before installing the new
one. Moves preserve allocation identity, so existing views remain valid until
an actual lifetime/length change. Heap owning slots are nullable after a move
or failed allocation and can be inspected with allocated.

The runtime records acquisition order in each allocation's ownership list and
drops children in reverse acquisition order. Cloning preserves that order,
deep-copies every owned allocation, and copies ordinary ref fields as aliases
to their original targets. It does not retarget borrowed graph edges.

Stack aggregates drop fields in reverse declaration order and fixed arrays in
reverse index order; each owned allocation drops its children in reverse
acquisition order. Scope bindings and explicit defers retain LIFO order.
Aggregate cleanup/replacement checks every affected ownership tree before
changing any payload. Duplicate roots are detected in linear time using
temporary descriptor marks; descriptors retain their existing size.
A fixed array contributes one repeated layout entry rather than one entry per
element. Aggregate glue allocates no heap storage; explicit clone allocates
only the owned allocations it copies. Embedded arenas support move/drop and
are rejected by clone.

Ownership cycles are rejected at the attaching operation. Cyclic graphs use
ordinary refs, including arena objects; those edges do not retain resources.
Shrinking an owning buffer drops removed trees. Growing/relocating its headers
updates their ownership metadata. Views into that buffer become stale; views
into retained, separately allocated children remain valid.

A managed assignment resolves its target once and revalidates the allocation
after evaluating the right-hand side. A callback cannot cause a later store
through an address from freed/recycled storage. Stable scalar writes reuse the
guard instead of repeating lifetime validation.
Ordinary scalar stores also reuse their earlier validation when their
right-hand side consists of built-in expressions that cannot invalidate
storage. Calls, overloaded arithmetic and user indexing require revalidation.
The optimization report records this lowering proof; indexed lifetime/bounds
checks remain in place.
Nested array reads also revalidate after evaluating an index that can invoke
a callback. A callback cannot leave a previously validated array address
unchecked after removing its allocation.

## Evidence and measurement

The regression runner checks behavior and diagnostics at O0, O2, and O3.
The latest complete language checkpoint has 854 passing cases and all seven
CTest suites passing. The sanitized runtime suite
uses a two-bit generation counter and exercises clone allocation failures,
resize retirement/reparenting, pinned descendants, cycles, callback writes,
and a 50,000-node deep clone/drop without recursion. Subobject coverage adds
exact extents, empty/one-past views, stale construction and output/header aliasing.
The runtime now has 30 sanitized cases, including nested value layouts,
duplicate roots and byte-for-byte preservation of pinned aggregates when
cleanup or replacement traps. Value-clone failures exercise every byte budget
before all three source allocations can be copied.
The additional optimization-report suite checks emitted IR against reports,
execution counters, source spans and unchanged native object bytes.

scripts/bench_memory.py verifies 80 edge runs, two sanitized C runs and eight
separately instrumented workloads for each reference or subobject walk.
scripts/bench_owners.py verifies 40 edge runs, two
sanitized C runs, and four separately instrumented lifecycle workloads. Both
use the same descriptor runtime, ABI, initialization, and arithmetic policy as
their C counterparts. Timings use fresh, uninstrumented processes, balanced
randomized execution, warmups, and median/MAD. Lifecycle results also preserve
individual OS high-water RSS samples, binary sizes, hashes, and compiler
versions. These are whole-process timings, not isolated cleanup latency.

The 100,000-node/eight-trial native lifecycle measurement was 83.963/83.073 ms
for Kawa/C chains (MAD 2.531/1.011 ms) and 67.305/62.691 ms for fanout
(MAD 1.000/1.025 ms). Allocation, free, copied-byte, descriptor-byte, and
index-check counters match. These local
measurements do not establish performance on other workloads or machines.
The Homebrew sanitizer runtime on this host stalls before main; verification
uses the working Xcode sanitizer runtime and records its separate version.

The field-view workload constructs two bounded views per iteration in both
Kawa and C, with identical runtime, row layout, initialization and arithmetic.
Its instrumented runs verify four indexed validations per checked iteration,
or zero indexed validations with two pins per iteration plus one outer pin.
Both modes create exactly two views per iteration. The stable variant measures
guard churn deliberately; it is not a claim that per-iteration pins are faster.
At ten million sequential iterations, private runtime helpers reduced the
local Kawa median from 83.876 to 77.021 ms (MAD 0.307/0.159 ms), with C at
71.240/72.049 ms. The Kawa executable shrank from 70,376 to 51,896 bytes.
Each run uses seven samples/two warmups and verifies the full timed input.

The owning-value workload clones four buffers inside a 136-byte value and
transfers that value either between stack bindings or through a managed heap
slot. Its static layout shape matches C's. With 100,000 elements per buffer
and 64 trials, the local Kawa/C medians were 66.029/65.493 ms for stack
transfers (MAD 0.566/0.640 ms) and 65.383/65.109 ms for heap transfers
(MAD 0.183/0.918 ms). Seven samples/two warmups run without instrumentation.
The separate instrumentation verifies 512/576 allocations and frees,
204,800,000 copied bytes, zero final live payload bytes, and
76,800,128/76,800,320 indexed validations in stack/heap modes respectively.
Both implementations use the same runtime, arithmetic, zero initialization,
cleanup and ownership layouts. These results establish no general language
ranking.

scripts/bench_tools.py uses three versioned, deterministic source corpora with
independent execution checksums, formatter fixed points and repeatable
bitcode/IR/object hashes. It measures check, format and build separately with
fresh processes and a warm filesystem. Median build times were 66.985 ms for
12 kernels, 385.419 ms for 256 kernels, and 106.760 ms for 64 kernels calling
17 nested generic helpers at four widths. Short/noisy check and format samples
are flagged; no compiler-cache hit rate or cold-cache claim is invented.

Native Darwin object emission uses the macOS product deployment version instead
of an outdated Darwin-kernel conversion and passes the same target to the linker.
The deployment override is checked, and successful tooling builds reject
unexpected linker warnings.

## Remaining scope

The future-contract manifests still describe the full design scope; their
planned status is not used as a passing-test count. Several concrete contracts
now have executable coverage, but no entire future feature is claimed complete.

Owned enum payloads and their cleanup/transfer glue remain restricted.
Typed Option/Result propagation,
coroutine cancellation cleanup, channel ownership transfer/shared regions,
UTF-8 str versus bytes and C-string contracts, explicit raw/unsafe/FFI
boundaries, shaped numerical/complex library work, and optimization reporting
refinements are still required. Reports currently expose exact static IR facts
and conservative source associations, not complete LLVM proof traces or
executed-operation counts. Legacy raw pointers retain their prior semantics
until the explicit boundary migration is implemented.

Design constraints: reference identity cannot depend on an allocation's address;
descriptor storage survives its payload; exhausted generations retire; checking
cannot itself read freed memory; a stable guard prevents invalidation before any
mutation, including reentrant calls. Ordinary references permit mutable aliases
and must never acquire LLVM `noalias` solely from their type.
