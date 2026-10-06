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

- Type-first explicit declarations for locals, globals, parameters, fields,
  and typed error handlers: `int x`, `owner<T> x`, `ref<T> p`, `arena pool`,
  `chan<T> queue`, arrays/slices, tuples, aliases and generic struct instances.
  `let name = expression` is inference-only and needs an initializer.
  Suffix type annotations and `let Type name` are rejected with E0001.
  Destructuring infers binding types; field labels and renames retain colons.
  Boolean literals carry bool types rather than falling back to integers.
  Comma declarations initialize in source order; inferred bindings need their
  own initializers. Array literals accept positional variables as well as literals.
- Const is enforced for local/global bindings, inline fields and arrays,
  consuming operations, mutable view/address creation and pointer receivers.
  Const owner/ref bindings do not freeze their referents. Orbit expressions
  reevaluate in their declaration environment on every read, including after
  alias writes/calls and under lexical shadowing. Side effects/owned results
  are rejected, including impure custom index getters; LLVM may reuse reads
  it proves unchanged.
- Raw pointer access/arithmetic/casts, foreign calls, raw slice construction,
  inline assembly and unchecked blocks require an unsafe context. `unsafe fn`
  and `#[unsafe]` require callers to establish preconditions. Vetted scalar
  math and standard-library wrappers retain their existing contracts.
  Source calls cannot gain privileges from a private runtime name. Formatted
  byte arrays/slices use their length, preserve embedded null bytes and evaluate
  once; raw C-string pointer printing requires unsafe.
  Large field widths stream padding in bounded chunks; high float precision
  uses a correctly sized temporary. Minimum signed integers and negative zero
  format without signed overflow or loss of their sign.
  This migrates existing raw programs with explicit annotations, without
  changing their output contracts or disabling managed checks.
- Managed allocation identity: zero-initialized owner/ref/arena allocations,
  byte extents, checked indexing/slicing/forward offsets, process-lived pooled
  descriptors, thread confinement, and generation retirement. The reference
  ABI is 32 bytes. Empty allocations have distinct, non-null identities.
- Affine owner bindings: explicit move/clone, implicit copy rejection, branch
  and loop state tracking, reassignment, and LIFO scope cleanup through return,
  break, continue, filter/press, and ordinary exits.
  Global owning values register reverse-declaration cleanup at normal program
  exit, including cancellation of started global coroutine handles.
- Stack owning values: nested structs/fixed arrays/tagged enums use compact static ownership
  layouts for move, deep clone, replacement and scope cleanup. Whole-value heap
  transfers register/detach their owned fields. Owned spreads release replaced
  fields; implicit aggregate copies and borrowing an owned temporary are rejected.
  Clone failure rolls back all new allocations and produces an all-zero value.
- Typed option<T>/result<T,E>: inline Some/None and Ok/Err values, constructors,
  exhaustive matching, `try` across typed functions or compatible dregs handlers,
  result-returning `press`, owned success/error payload transfer, void payloads,
  and structural generic/overload keys including both T and E. Failure paths
  run LIFO cleanup without unwinding. Fallible functions cannot silently
  synthesize success on a reachable fallthrough. Partial literal construction
  and owning arguments already evaluated before a later failure are guarded.
- Coroutine handles share managed identity and ownership metadata: affine moves,
  nested aggregate/channel/result storage, automatic cancellation, cleanup at
  initial/intermediate/final suspension, completed promise retention, and pins
  preventing active-frame destruction. LLVM computes exact frame size and may
  perform proven frame elision; no fixed frame buffer or unconditional claim
  of elision remains. Owning handles and aggregates containing them cannot be
  cloned; borrowed handle views can be copied as aliases.
- Cooperative chan<T>: managed ring, exact positive requested capacity with
  power-of-two physical backing, owned FIFO transfers, queued/pending message
  cleanup, closed-channel traps, discard/select cleanup, and single evaluation
  of channel/message expressions. Suspended operations revalidate managed
  containing storage before accessing metadata. Select supports eight cases
  and polls in declaration order; operating-system thread messaging is deferred.
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
  Pure functions can perform checked reference reads and view construction;
  runtime validation/instrumentation is separated from application mutation in
  the analysis copy without changing production inlining or managed guards.
  Handle/arena cleanup and arena-object removal conservatively reject noalloc
  because destruction can invoke user callbacks with unknown allocation effects.
  Plain owning-buffer transfers and cleanup still satisfy noalloc.
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
Local builds optimize the embedded runtimes for the native CPU at O3.
Distribution builds use STILL_NATIVE_CPU=OFF so the compiler and embedded
bitcode do not require the build runner's CPU features. Both I/O and memory
runtime headers are generated with LLVM 21 during the build.
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
temporary descriptor marks. Resource descriptors also carry destruction state
and an optional callback for coroutine cancellation.
A fixed array contributes one repeated layout entry rather than one entry per
element. Aggregate layout glue allocates no heap storage; explicit clone allocates
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

The strict regression runner checks successful execution, exact output,
diagnostics, IR contracts and deliberate traps at O0, O2 and O3. Managed safety
failures must be checked aborts, never segmentation faults. New semantic,
fallible and concurrency fixtures cover comma declarations, const through
aliases/views, orbit dependencies, single evaluation, owned enum layouts,
propagating partial construction/calls, nested handles, cancellation, blocked
owned sends/receives/selects, close/drain, and stale channel containers.

The sanitized C runtime suite checks two-bit generation retirement, byte budgets
for clone failure, 50,000-node iterative clone/drop, pins/cycles, conditional
active-tag layouts with garbage inactive payloads, resource adoption, nested
cleanup and reentrant replacement/destruction. Metadata is retired before a
resource callback runs. Callback-capable trees temporarily protect their
containing storage; plain leaf allocations retain a fast path. Opaque LLVM
coroutine frame bytes are excluded from payload metrics; adopted handles count
as resources and occupy descriptor storage.

All nine CTest suites also cover check mode, formatting, artifact repeatability,
branding, optimization reports, smoke and harness behavior. Final validation on
macOS ARM64 and Linux x86-64 passes the same 1,202 regression combinations across
412 fixtures at O0/O2/O3, with no expected failures or skips. This adds 106
Whisky fixtures. The sanitized runtime suite passes 38 success/trap cases.
Four sanitized print-runtime cases independently check exact bytes for minimum
integers, buffer boundaries, large widths/precision, negative zero and binary data.
The native run passes all nine CTest suites. Linux checks the final rebuilt
compiler with four independent fixture workers and runs the other eight suites.
The emulated unit timeout is configurable
with STILL_UNIT_TEST_TIMEOUT; the default remains 600 seconds.
Result JSON and logs stay in ignored build directories. Thirty-nine complete
programs from nine guides are compiled and run, including the deliberate process
exit example's status and stderr. Earlier fragment checks retain their recorded
results.

Existing matched/historical comparison programs explicitly annotate raw access.
Their algorithms and independent/differential output policies are preserved.
Compound assignment evaluates its target once; C counterparts reuse the same
validated address, and managed validation-count formulas reflect this behavior.
Scalar pointer difference remains a count for provenance checking; encoded raw
addresses retain their taint.

`bench_semantics.py` adds three equal-policy Whisky/C comparisons: typed result
propagation, owned enum clone/consuming match, and owned FIFO channel transfer.
It builds O0/O2/O3 pairs, instrumented O3 pairs and untimed C ASan/UBSan binaries.
Independent closed-form Python equations check checksums, error/owned/message
counts, allocations, frees, peak/cloned/live bytes and indexed validations.
The normal gate verifies 120 edge runs, 15 sanitized runs and six full-input
instrumented runs. Timing uses fresh uninstrumented processes, balanced random
order, warmups, raw samples, median/MAD, peak RSS, executable sizes, source/runtime/
oracle/binary hashes and compiler versions. CI runs correctness without speed
thresholds. These whole-process workloads establish no general language ranking.

`bench_memory.py`, `bench_owners.py`, `bench_tools.py` and the six matched/17
historical triples remain correctness gates. The runtime/oracle policies and
benchmark interpretation are in bench/README.md. Prior recorded timings refer
to earlier sources and are not claimed for this implementation. New measurements
must use a fresh report. Compiler checks and runtime metrics remain independent:
a missing IR call/tag alone is not proof that a runtime check was eliminated.
The fresh native semantics report uses 100 million scalar iterations and two
million iterations for each owning workload, seven samples and two warmups.
Builds, sanitizers and instrumented runs are separate from timing. Native timing
runs after the Linux validation completes, so its compiler work does not compete
with measured programs.

Repository CI runs Linux x86-64 and macOS ARM64. Native ELF objects use PIC for
PIE linkers; libc math is linked after the generated object. Distribution builds
use STILL_NATIVE_CPU=OFF and freshly generate both embedded runtimes with LLVM21.

## Remaining scope

The future-contract manifests still describe the full design scope; their
planned status is not used as a passing-test count. Several concrete contracts
now have executable coverage, but no entire future feature is claimed complete.

The remaining design scope includes shared regions and cross-thread channel
handoff, UTF-8 str versus bytes and C-string contracts, full C/C++ interoperability,
shaped numerical/complex library work, and optimization-report refinements.
Reports currently expose static IR facts and conservative source associations,
not complete LLVM proof traces or executed-operation counts. References stay
thread-confined. Coroutine promises are currently i32, brew does not implicitly
capture locals. Automatic global cleanup does not run on process abort. Void errors
propagate through result returns; typed dregs bindings require a payload.
Raw pointer operations remain programmer-checked inside their explicit boundary.

Design constraints: reference identity cannot depend on an allocation's address;
descriptor storage survives its payload; exhausted generations retire; checking
cannot itself read freed memory; a stable guard prevents invalidation before any
mutation, including reentrant calls. Ordinary references permit mutable aliases
and must never acquire LLVM `noalias` solely from their type.
