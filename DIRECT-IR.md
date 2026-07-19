# Direct LLVM IR Generation Plan

## Goal

Generate LLVM IR from Inform's expression and statement code generators before
Glulx assembly instructions have been selected. Keep the existing optimized
LLVM-to-Glulx lowerer and classic Glulx encoder, but remove the assembly-to-LLVM
lifter once direct generation covers the supported language and inline assembly
surface.

Use a pinned upstream Inform 6 compiler, based on the same source revision as
this fork, for classic Glulx behavior and instruction-count baselines. The
fork's Z-machine path remains classic unless separately changed.

This migration is an experiment. The current lifter buys a marginal measured
win (about 2.8% fewer dynamic instructions on the Life benchmark) at a
complexity cost that is not clearly worth the maintenance burden. Direct
generation tests whether producing IR before instruction selection gives a
better result with less reconstruction machinery. Ending the experiment —
keeping the lifter, or retiring the optimized path — is an acceptable outcome
if the evidence says so; record that evidence in `REVIEW.md`. This document is
migration-scoped and is deleted when the migration completes or the experiment
ends.

The project is successful when:

- Normal Glulx source and the project's veneer routines generate LLVM IR
  directly, without first producing a source assembly stream.
- Inline Glulx assembly has a defined direct-IR representation and preserves
  VM faults, callbacks, stack behavior, and control flow.
- The strict test corpus compares the fork against pinned upstream Inform.
- Optimization coverage is explicit; unsupported routines cannot silently
  disappear from the optimized set.
- Dynamic Glulx instruction counts do not regress without an intentional,
  documented target-cost tradeoff.
- The old Glulx assembly lifter and its input capture path can be deleted.

## Status

- [x] Phase 0: pin upstream Inform and move classic Glulx comparisons to it.
- [x] Phase 1: establish direct routine lifecycle and lower trivial functions.
- [x] Phase 2: generate straight-line expressions directly.
- [x] Phase 3: generate structured control flow directly.
- [x] Phase 4: add calls, memory operations, VM effects, and inline assembly.
- [x] Phase 4.1: close the library-code dynamic gap against upstream.
- [ ] Phase 5: make direct IR the default Glulx path.
- [ ] Phase 6: remove the lifter and shadow assembly emission.

Update this checklist only when the corresponding phase exit gate is satisfied.

## Why Change The Current Pipeline

The current capture seam in `src/asm.c` records `assembly_instruction` and label
events before byte encoding. This is better than lifting bytecode, but the
front end has already made several target-level decisions:

- Expression trees have been flattened into instructions.
- Temporary locals and VM-stack values have been introduced.
- Structured conditions have become labels and branches.
- Assignment and return destinations may be separated from their values.
- Lvalue and source-level side-effect information has been reduced to opcode
  behavior.

`src/llvm_codegen.c` reconstructs values, blocks, symbolic stack state, and phis
from that stream. `src/llvm_lower.c` later reconstructs short-circuit trees and
select/store/return relationships when lowering optimized IR. Direct generation
can preserve these relationships instead of discovering them twice.

LLVM IR is still useful as the routine-level value and control-flow form. The
change is to produce it earlier, not to replace LLVM's optimizer or the
Glulx-aware lowerer.

## Scope

### In Scope

- A direct LLVM backend for Glulx expression and statement generation.
- Routine lifecycle integration with `assemble_routine_header()` and
  `assemble_routine_end()`.
- Direct representation of locals, globals, constants, symbols, calls, memory
  operations, branches, loops, switches, and returns.
- Direct handling of Inform inline Glulx assembly.
- Reuse of the current LLVM pass pipeline and `src/llvm_lower.c`.
- A pinned upstream compiler package and upstream-vs-direct test matrix.
- Temporary per-routine fallback while direct coverage is incomplete.
- Removal of the old lifter and input capture path after migration.

### Out Of Scope

- Replacing LLVM with a custom optimizer.
- Rewriting the parser or language grammar.
- Changing the Z-machine backend.
- Interprocedural optimization or inlining.
- Profile-guided branch ordering.
- Treating source operand shape as branch-frequency evidence.
- Replacing the classic Glulx byte encoder, branch shortening, or backpatcher.

## Architectural Decisions

### Keep The Existing Encoder Boundary

Direct IR is optimized and lowered to `assembly_instruction` records. The
existing assembler continues to encode the final stream, shorten branches,
resolve labels, preserve backpatch markers, and write routine headers.

The output side of the capture buffer remains useful. The input side, where
front-end assembly is captured for lifting, is the part to remove.

### Keep LLVM Details Behind A Backend API

Do not include LLVM headers throughout `expressc.c`, `states.c`, and
`syntax.c`. Add an internal direct-codegen API with opaque value and block
handles. LLVM object ownership and C API calls remain in LLVM-specific source
files.

The API should express source semantics rather than Glulx operand modes. It
needs operations in these groups:

- Routine lifecycle: begin, finish, abandon, verify.
- Values: constants, symbolic constants, parameters, locals, globals.
- Lvalues: local, global, RAM address, object/property operations.
- Expressions: unary, binary, comparison, select, call.
- Effects: load, store, opaque Glulx operation, Glk operation, stream operation.
- Control flow: block creation, label binding, branch, switch, return,
  unreachable.
- VM stack: explicit operations only for inline assembly that names `sp`.

Start locals as LLVM allocas and let `mem2reg` promote them. Manual SSA
construction is not required for the initial direct backend.

### Preserve Glulx Semantics Explicitly

The direct backend must carry forward the existing semantic guards:

- Glulx signed division and remainder faults versus LLVM undefined behavior.
- Glulx shift behavior for counts outside the LLVM-defined range.
- Symbolic/backpatchable values represented as opaque tokens.
- Potentially faulting reads retained even when their values are unused.
- Stream operations treated as callbacks under filter I/O.
- RNG operations ordered independently of ordinary RAM when valid.
- Calls with stack operands evaluated in Glulx operand order.
- `jumpabs` represented as an opaque non-returning operation; computed code
  addresses follow the documented unsupported-under-optimization policy.

Effect classification must remain centralized so optimization and lowering use
the same answer.

### Use Upstream Inform As The Classic Oracle

Add a pinned upstream Inform 6 package built from the source revision this fork
tracks. Compile classic fixtures with that package rather than `$LLVM=0` in the
fork.

The upstream comparison should cover:

- Interpreter transcript and completion behavior.
- Expected VM faults and abnormal behavior.
- Deterministic dynamic instruction counts.
- Compliance stories and full library games.

The fork should still test its Z-machine path directly. During migration it
should also retain a small internal test for the shadow assembly fallback, but
byte-identical Glulx capture/replay is no longer the long-term product gate.

### Use Shadow Assembly Only During Migration

The parser is streaming. If direct generation discovers an unsupported
construct near the end of a routine, the source cannot simply be parsed again.
During migration, generate the existing Glulx assembly stream into the capture
buffer while also building direct IR.

At routine end:

- If direct generation, optimization, validation, and lowering succeed, replace
  the shadow stream with lowered output.
- If direct generation rejects the routine, replay the shadow stream.
- Record the exact direct-generation bailout reason.

Shadow emission is temporary migration infrastructure. It must not become the
permanent architecture. Removal requires direct coverage gates described below.

### Decouple Parser State From Assembly Emission

The assembler currently performs bookkeeping that the streaming statement
parser reads while compiling the rest of a routine. This includes consuming
`sequence_point_follows`, updating `execution_never_reaches_here`, and counting
forward branch uses in `labeluse[]`. Suppressing front-end instructions or
labels without replacing those side effects can change implicit returns,
reachability, and label stripping.

Factor this parse-visible bookkeeping into target-independent helpers before
removing shadow emission. The helpers must have exactly one writer per
compile. While shadow emission exists, shadow capture remains that writer — it
already mirrors these side effects — and the direct backend only reads the
state; it must never update it. When shadow emission is removed, the direct
backend becomes the sole writer. Double-applying an update, such as consuming
`sequence_point_follows` twice or counting the same forward branch in
`labeluse[]` from both paths, corrupts parsing far from the fault. Final
lowered-output replay may recompute encoder state, but must not feed duplicate
state back into parsing.

### Computed Code Addresses Are Unsupported

DM4 §41 restricts Inform branch labels to one routine, but `jumpabs` consumes an
ordinary computed operand rather than a branch label. The Glulx specification
(§2.2 "Branches") explicitly permits branches into another function and notes
that functions have no well-defined end. `jumpabs` may target any absolute code
address.

Inform does not, however, guarantee the generated instruction layout needed to
derive an interior address from a routine symbol. Glulxercise computes
`test_jumpabs_2+5`; optimization and compiler-version changes may invalidate
that offset.

Policy: programs that compute code addresses are unsupported under LLVM
optimization, exactly as they are unsupported across upstream compiler
versions. `@jumpabs` remains representable as an opaque non-returning
operation from the jumping routine's perspective; the layout of the target
routine is not preserved, and no story-wide preflight, whole-story buffering,
or layout-preserving mode is required. Direct generation must retain the
existing optimization-time warning for every `jumpabs`, because the compiler
cannot prove whether its operand depends on generated layout.
`tests/complianceTest.nix` pins the known out-of-contract glulxercise failures
so a change in the failure set is still detected. The policy is recorded in
`README.md` and `REVIEW.md`.

## Target Structure

The intended final Glulx path is:

```text
tokens
  -> expression and statement parsing
  -> direct LLVM backend
  -> LLVM verification and optimization
  -> Glulx-aware LLVM lowering
  -> assembly_instruction output events
  -> classic Glulx encoder and backpatcher
```

The intended Z-machine path remains:

```text
tokens -> existing expression/statement code generation -> Z assembly encoder
```

Suggested source responsibilities:

- `src/llvm_direct.c`: direct routine builder and backend API implementation.
- `src/llvm_effects.c` or one existing LLVM module: centralized opcode and call
  effect classification, if splitting it reduces duplication.
- `src/llvm_codegen.c`: pass driver and migration coordination, then deleted or
  reduced after the old lifter is removed.
- `src/llvm_lower.c`: optimized LLVM IR to Glulx output planning and emission.
- `src/expressc.c`: expression-tree traversal chooses classic Z generation or
  direct Glulx backend operations.
- `src/states.c` and `src/syntax.c`: statement-level control-flow hooks.
- `src/asm.c`: routine headers, final output events, encoder, and temporary
  shadow capture.

Exact file splits should stay minimal. Do not split modules solely to match
this suggested layout.

## Migration Phases

### Phase 0: Pin The Baseline And Characterize Dependencies

- Add a flake input or fixed source derivation for upstream Inform 6.
- Verify that its source revision matches the fork's non-LLVM base.
- Add an `inform6-upstream` package.
- Compile classic Glulx stories with upstream in `stories/default.nix`.
- Change behavioral, compliance, and benchmark comparisons to upstream versus
  the LLVM fork.
- Keep a temporary fork-classic/capture check until shadow fallback is removed.
- Add diagnostics that identify whether each routine used the old lifter,
  direct IR, or classic fallback.
- Record direct, lifted, and fallback routine totals separately.

Exit gate:

- Existing tests and benchmarks run against the pinned upstream baseline.
- Baseline differences, if any, are explained before direct generation starts.
- No missing tool or story dependency turns a required test into a skip.
- The computed-code-address policy is documented in `REVIEW.md` and the
  compliance suite still pins its known out-of-contract failures exactly.

### Phase 1: Establish The Direct Routine Lifecycle

- Add direct-backend begin and finish hooks around routine compilation.
- Retain the current ownership model: one lazily created LLVM context for the
  compilation, with a fresh module and function for each routine.
- Create allocas for declared locals and initialize parameters in Glulx call
  order.
- Map source labels to LLVM basic blocks without emitting Glulx labels first.
- Factor parse-visible reachability, sequence-point, and label-use updates
  into shared helpers written by shadow capture; the direct backend reads but
  never writes them while shadow emission exists.
- Verify IR before optimization and after the pass pipeline.
- Feed a directly built trivial function into the existing lowerer.
- Keep shadow assembly as fallback.
- Add a temporary explicit mode that distinguishes direct generation from the
  old lifter during migration.

Initial supported routines:

- Constant return.
- Parameter return.
- Local assignment followed by return.
- Unconditional branch to a return block.

Exit gate:

- Focused trivial routines are proven to use direct IR and lower successfully.
- Upstream and direct behavior match.
- A forced direct-builder failure safely replays the shadow stream.
- Source with compile errors exercises the direct builder's abandon path
  without a crash or emitted code.
- Differential tests show parser reachability, implicit returns, sequence
  points, and forward-label handling are unchanged.

### Phase 2: Generate Straight-Line Expressions Directly

- Add direct values for constants, parameters, locals, and globals.
- Translate arithmetic, bitwise operations, comparisons, and conversions.
- Carry lvalue information through assignment generation.
- Generate stores directly to their final destination.
- Generate returns from expression values without temporary VM-stack traffic.
- Represent backpatchable constants with the existing symbolic-token scheme.
- Preserve source operand evaluation order.
- Add direct diagnostics for unsupported expression operators.

Test shapes:

- Arithmetic with locals, parameters, and globals.
- Assignment expressions and chained assignments.
- Pre/post increment and decrement.
- Comparison returns.
- Comparison values nested in arithmetic and assignments.
- Expressions used only for side effects.
- Faulting division and signed-overflow boundaries.

Exit gate:

- Straight-line optimization fixtures no longer pass through the lifter.
- Static and dynamic bounds remain within their test-owned ceilings.
- Invalid or faulting expressions match upstream behavior.

Benchmark note: Phase 2 direct coverage reaches only `Rnd` in Life. An
earlier run showed direct mode 4,104 dynamic instructions above upstream;
the cause was not division (the `sdiv` lowers to one native `div`) but a
lowerer coalescing gap for a value stored to a global and then reused,
which cost a temporary local plus a `copy` where classic writes the global
directly and reads it back. The lowerer's multi-use store fold now emits
classic's shape, direct mode matches upstream exactly on Life, and the
lifted production path improved by the same 4,104 instructions. The fix and
its guards are recorded in `REVIEW.md`.

### Phase 3: Generate Structured Control Flow Directly

- Translate condition context into LLVM branches rather than materialized
  Glulx booleans.
- Map `if`, `else`, loops, and source labels to LLVM blocks.
- Preserve short-circuit `&&`, `||`, and negation structurally.
- Generate condition-to-value expressions as LLVM phis or selects according to
  semantics, leaving profitability to lowering.
- Translate `switch` while preserving source comparison order.
- Represent `break`, `continue`, and routine returns explicitly.
- Keep block ordering neutral when branch frequency is unknown.

Test shapes:

- Nested short-circuit expressions with effects in each arm.
- Zero-trip and conditionally entered loops.
- Loop-carried local values and phi cycles.
- Early returns and shared return blocks.
- Switches with shared targets and default fallthrough.
- Conditional edges requiring lowerer phi-copy stubs.
- Conditional values with ordinary and constant arms.

Exit gate:

- The control-flow routines in the focused optimization fixture use direct IR.
- Loop and branch behavior matches upstream across positive and negative cases.
- No transformation relies on operand form as a likelihood heuristic.

### Phase 4: Add Calls, Memory, VM Effects, And Inline Assembly

The Glulx veneer and full library games rely on inline assembly (`@aload`,
`@binarysearch`, `@aloadbit`, ...), so call and memory coverage cannot be
gated separately from inline assembly; they land in one phase.

- Add ordinary calls, tail calls, veneer calls, and Glk calls.
- Preserve left-to-right operand decoding and stack-pop order.
- Add global and RAM reads/writes with centralized alias/effect metadata.
- Add object, property, array, and search operations.
- Add RNG and stream operations with callback-safe ordering.
- Preserve invalid-read, malformed-search, and callback behavior.
- Define how catch tokens and throw edges appear in IR, or reject them with an
  explicit direct-generation reason.

Inline assembly cannot be treated as an unstructured textual escape. Parse the
existing `assembly_instruction` operands, then translate each instruction into
one of these forms:

- A native LLVM operation with proven matching semantics.
- A typed opaque `i6.<opcode>` call with centralized effects.
- Explicit LLVM control flow for branches and returns.
- An explicit symbolic VM-stack operation when `sp` is named.
- A direct-generation rejection for layout-sensitive or unsupported behavior.

Custom opcodes need a stable opaque representation carrying opcode number,
operand rules, flags, and operands through lowering. Do not infer effects for a
custom opcode that lacks a trustworthy declaration.

Debug-file builds, asterisk-traced routines, and Infix currently bypass LLVM
capture. Define their direct-generation behavior in this phase. Prefer making
diagnostics observe direct generation rather than changing generated code; if a
mode must remain classic, make that a documented and tested product policy
rather than an accidental capture exclusion.

Exit gate:

- Full library games and veneer routines compile through direct IR at an
  explicitly asserted coverage level.
- Alias, callback, fault, RNG, and stack-argument tests pass.
- Lowering fallbacks and direct-generation bailouts are independently counted.
- `glulxercise` and project-specific inline assembly compile with expected
  direct coverage and exact known incompatibilities.
- Stack order, custom opcodes, faults, and non-returning operations have focused
  tests.
- Inline shift counts at zero, 31, 32, negative, and variable boundaries have
  focused tests.
- Computed code addresses follow the documented unsupported policy, and the
  compiler warning and compliance failure set remain guarded.
- Debug-file output, traced routines, and Infix have explicit direct or classic
  behavior with focused tests.

### Phase 4.1: Close The Library-Code Dynamic Gap

Phase 4's corpus gate measured what the transcript-only cloak test had
hidden: on a full library game both LLVM paths run more dynamic Glulx
instructions than upstream (upstream 164,995; direct 175,651, +6.5%;
lifted 179,729, +8.9%). Fallback routines replay classic code, so the
gap comes entirely from lowering shapes, not from coverage. The known
cost items are documented under "Instruction-Count Findings" in
`REVIEW.md`: materialized comparisons and selects, select-to-store not
folding, phi edge copies, and related boolean-value shapes that library
dispatch code hits constantly.

This phase spends bounded effort closing that gap before the lifter is
removed:

- Collect per-routine static deltas and opcode histograms for cloak's
  direct routines against classic, and rank the hot shapes.
- Attack the documented lowering costs generically: fold selects into
  their stores and returns, emit boolean values through branch forms
  when the consumer is a store or return, and coalesce phi edge copies,
  each guarded by focused fixtures.
- Where a value-form boolean exists only because direct generation joins
  paths that classic keeps as branches (strict guards, `has`/`in`
  results), prefer condition-context generation when the consumer
  branches — only where the transformation is generic and pathwise
  non-worsening.
- Re-measure cloak, Life, and the focused optimization fixture after
  each change; lower the test-owned cloak ceiling as it improves.
  Improvements land in the shared lowerer, so the lifted path benefits
  while it still exists.

Exit gate:

- The cloak direct dynamic count is at or below upstream, or the
  remaining gap is explained by a documented target-cost rationale with
  its ceiling pinned in the corpus test.

This gate is deliberately softer than the others. Direct generation
already beats the lifted path on every measured corpus point, so the gap
is not evidence against the migration, and maintaining two IR generation
schemes indefinitely is excessive ongoing cost. If the gap resists
closing after the documented findings are addressed, record the outcome
and the accepted ceiling in `REVIEW.md` and proceed to Phase 5 rather
than holding the lifter's removal hostage to parity.

Outcome: met on the primary arm, so the soft fallback was not needed.
Dynamic per-routine attribution (profiled glulxe joined against `$!asm`
addresses) replaced the earlier static ranking and located the gap in
four generic lowering shapes: return select chains, phi-to-parameter
slot adoption, cross-block direct global reads, and jump-only layout
straightening. Cloak now runs 162,002 direct and 163,569 lifted against
164,995 upstream — both LLVM paths beat classic — and every focused
fixture tightened; details in `REVIEW.md`.

### Phase 5: Make Direct IR The Default Glulx Path

- Change the normal LLVM mode to select direct generation.
- Retain the old lifter only as an explicitly selected diagnostic comparison.
- Assert direct-generation coverage in focused and corpus tests.
- Track direct build failures separately from lowerer failures.
- Compare direct IR against the old lifted IR for selected fixtures only where
  that helps explain changes; do not require structural identity.
- Revisit pass ordering using direct IR shapes rather than inherited lifted
  shapes.

Exit gate:

- All required corpus stories meet committed direct-coverage assertions.
- No correctness test depends on old-lifter fallback.
- Performance is non-worse on guarded dynamic metrics, or each accepted
  regression has a documented target-cost rationale.
- Repeated timings remain consistent with dynamic and opcode-mix results.

### Phase 6: Remove The Lifter And Shadow Emission

- Delete assembly-to-LLVM instruction lifting and symbolic stack reconstruction.
- Delete label-entry stack inference used only by the lifter.
- Stop capturing front-end Glulx assembly for directly generated routines.
- Route parse-visible reachability, sequence-point, and label-use bookkeeping
  through the backend-independent helpers established in Phase 1.
- Remove the temporary direct/lifter mode and fallback counters.
- Remove byte-identical Glulx capture/replay as a primary gate.
- Keep final lowered-output buffering because the lowerer still emits through
  the classic encoder.
- Remove classic Glulx generation from shared front-end paths only where doing
  so does not affect the Z-machine backend or required no-LLVM behavior.
- Update `README.md` architecture after the final path is stable.
- Delete this plan document; keep measured migration history and performance
  results in `REVIEW.md`.

Exit gate:

- No production Glulx compilation references the old lifter.
- The no-LLVM build policy is explicit: either Glulx remains available through
  classic generation, or LLVM is a declared requirement for optimized Glulx.
- Full tests and benchmarks pass from a clean checkout.

## Test Strategy

### Required Comparison Axes

- Pinned upstream Inform versus direct-IR fork.
- Behavioral transcript and completion marker.
- Interpreter and timeout status.
- Expected VM fault transcript where applicable.
- Direct-generation, optimization, and lowering coverage.
- Per-routine static Glulx instruction bounds.
- Focused dynamic instruction bounds.
- Whole-program dynamic totals and opcode histograms.
- Repeated wall-clock timings as a secondary validation.

### Coverage Rules

- Aggregate instruction counts must always be paired with routine coverage.
- A routine falling back must not make aggregate optimization totals appear to
  improve.
- Required test dependencies must be Nix inputs, not optional PATH probes.
- Each migration phase adds at least one positive direct-generation test and
  one semantic rejection or fault test.
- Tests should assert the backend used for named focused routines.
- Corpus tests should assert minimum direct counts and maximum categorized
  bailout counts.
- At least one test compiles erroneous source and asserts the direct builder
  abandons cleanly, without a crash or emitted routine code.
- Exact numeric expectations belong in test scripts, not this plan.

### Differential Diagnostics

Provide a machine-readable diagnostics mode before corpus migration. One record
per routine should include:

During migration this mode is selected with `I6_LLVM_DIAGNOSTICS=1`, separately
from the `$LLVM=4` direct-backend selection, so ordinary direct compiles do not
emit one record per routine.

- Routine identity.
- Selected backend: direct, lifted, or classic fallback.
- Direct-generation bailout reason.
- LLVM verification result.
- Lowering bailout reason.
- Input/source operation count where meaningful.
- Emitted Glulx instruction count.

This avoids parsing human-facing trace output and makes LLVM-version and corpus
comparisons reproducible.

## Correctness Checklist

- Integer arithmetic uses wrapping semantics unless Inform or Glulx specifies a
  fault.
- Division by zero and signed overflow behavior matches Glulx.
- Shift counts and rotate patterns match Glulx for all 32-bit counts.
- Unchosen conditional arms do not execute or introduce poison-dependent
  behavior.
- Potentially faulting reads cannot be deleted or hoisted onto untaken paths.
- Memory operations are not moved across possible aliases or callbacks.
- Filter I/O stream operations remain arbitrary VM callbacks.
- RNG operations preserve state order.
- Function and opcode arguments preserve Glulx stack-pop order.
- Tail calls leave no symbolic or physical stack residue.
- Backpatchable symbols never fold as ordinary integers.
- Source labels, catch tokens, and non-local control flow retain layout
  semantics.
- Debug sequence points remain associated with valid emitted locations.
- Malformed input that previously produced a VM fault or nontermination is not
  converted into successful execution.
- Source containing compile errors does not crash the direct builder or emit
  code from a partially built routine; the abandon path leaves no stale
  per-routine state.

## Performance Principles

- Dynamic interpreted instruction count remains the primary deterministic
  signal.
- Opcode mix and operand modes are considered when totals are close.
- Static expansion is acceptable only when a generic pathwise or loop-based
  argument supports it.
- Unknown branch frequency must not be guessed from constant versus computed
  operands.
- Source ordering is preserved unless a transformation is pathwise
  non-worsening or justified by a target cost model.
- Direct IR should remove reconstruction overhead and bailout limits, but
  compile-time improvement is secondary to generated-code correctness and cost.

## Main Risks

### Shared Z And Glulx Expression Generator

`expressc.c` serves both targets. Direct Glulx changes can accidentally alter Z
code generation. Keep target selection explicit and add a Z-machine baseline
before broad expression refactoring.

### Streaming Parser And Late Failure

The source cannot be replayed after a late unsupported construct. Shadow
assembly is required until direct support can be decided before emission or
until all required constructs are handled.

### Parser State Hidden In The Assembler

Instruction and label emission currently update state consumed by statement
parsing. Direct generation must preserve those updates independently of final
byte emission before shadow assembly can be removed.

### Computed Code Addresses

Programs that compute code addresses (`@jumpabs` with address arithmetic)
can change behavior under optimization. This is accepted, documented policy
rather than a hazard to engineer around: Inform does not guarantee the generated
instruction layout from which such addresses are derived. Glulx itself permits
arbitrary absolute branch targets. The compiler warns whenever optimization is
requested for `jumpabs`, and the compliance suite pins the known out-of-contract
failures so the accepted set cannot grow unnoticed.

### LLVM Semantic Mismatch

Producing IR earlier increases the amount of source behavior exposed to LLVM.
Every operation needs defined overflow, fault, memory, callback, and progress
semantics. LLVM attributes are part of correctness, not just optimization.

### Lowerer Shape Assumptions

The current lowerer is tested against shapes produced by the lifter and current
pass pipeline. Direct IR may produce different canonical forms. Extend the
lowerer only for generic shapes and guard each new form with focused tests.

### Debug And Trace Modes

Debug files, traced routines, and Infix currently avoid the capture pipeline.
They need an explicit direct-generation design and tests before direct IR can be
called the sole production Glulx path.

### Upstream Baseline Drift

Pin the upstream revision. Updating it is an explicit compatibility event, not
an incidental flake refresh.

### Permanent Dual Pipeline

Shadow assembly and the old lifter can make migration safe but also double
maintenance. Phase 5 coverage gates and Phase 6 deletion are required project
outcomes, not optional cleanup.

## First Work Slice

The first implementation slice should establish infrastructure without changing
optimization behavior:

1. Identify and pin the matching upstream Inform 6 source revision.
2. Package it as `inform6-upstream` in Nix.
3. Compile the existing classic Glulx fixtures with upstream.
4. Keep the fork's capture/replay test temporarily and add a Z-machine baseline.
5. Add backend-origin diagnostics for lifted and fallback routines.
6. Confirm the compliance suite still pins the computed-code-address policy
   failures exactly.
7. Factor parser-visible assembler bookkeeping into shared helpers.
8. Add direct routine lifecycle hooks that are inactive by default.
9. Directly build and lower one constant-return fixture behind an explicit
   migration mode.
10. Prove forced direct failure replays shadow assembly correctly.
11. Run the complete test suite and Life benchmark before expanding expression
   support.

This slice validates the baseline, lifecycle, fallback, diagnostics, and lowerer
integration before invasive changes to expression generation.

## Completion Criteria

The direct-IR migration is complete only when all of the following hold:

- Direct IR is the sole production path from normal Glulx source to optimized
  LLVM IR.
- Required inline assembly is directly represented, with computed code
  addresses governed by the documented unsupported policy.
- The old assembly-to-LLVM lifter is deleted.
- Front-end shadow assembly is deleted for direct routines.
- Tests use pinned upstream Inform for classic Glulx comparisons.
- Z-machine behavior remains guarded.
- Routine coverage and bailout diagnostics are machine-readable and gated.
- Correctness, focused optimization, compliance, and corpus tests pass.
- Dynamic benchmark and opcode-mix results satisfy the maintained regression
  policy.
- Documentation describes only the final architecture; migration findings and
  historical measurements remain in `REVIEW.md`, and this plan document is
  deleted.
