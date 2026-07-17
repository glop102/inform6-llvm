# LLVM Pipeline Review

This review treats dynamically dispatched Glulx instruction count as the
primary performance metric. Benchmarking indicates that runtime across the
interpreters tested tracks instruction count approximately 1:1, so native-code
assumptions such as preferring simpler instructions, speculative execution, or
register-like copies do not apply. Every additional VM instruction incurs a
fetch/decode/dispatch cycle.

The pipeline already reflects this model in several important places,
particularly stack fusion, direct global operands, phi coalescing, select-arm
sinking, connective-tree reconstruction, and the decision not to run
`loop-rotate`. The largest immediate weakness is not a missing optimization,
but the lack of tests which prove that these transformations continue to
reduce dispatched instructions.

## First Priority: Optimization Tests

The current tests provide useful behavioral coverage but almost no
optimization-quality regression coverage.

- `tests/run-m1.sh` proves that capture/replay remains byte-identical.
- `tests/run-m3.sh` compares classic and optimized interpreter transcripts.
- `tests/run-life.sh` provides one useful wall-clock benchmark.
- Compiler optimization statistics are redirected to `/dev/null` by the test
  scripts.
- No test asserts that a routine was successfully lifted or lowered.
- No test asserts an instruction-count ceiling for a specific transformation.
- The Life benchmark reports one timing and applies no performance threshold.
- The ordinary transcript tests do not check the exit status from `timeout` or
  the interpreter, so matching crashes or timeouts can potentially pass.
- The real LLVM pipeline is not exercised by CI; the Windows build uses the
  no-LLVM stub.
- Missing `glulxe` causes the behavioral and benchmark scripts to exit
  successfully without testing anything. Missing Inform library files also
  turn the Cloak checks into successful skips.
- `tests/run-m1.sh` is described in `DESIGN.md` as a gate for both targets, but
  all of its fixtures are compiled with `-G`, so it exercises only Glulx.
- The `glulxercise` gate filters every failure line containing `token=` or
  `jumpabs test=` rather than asserting an exact expected set. A new failure
  containing either substring could be hidden.

A regression which causes every routine to fall back to classic codegen could
therefore pass most tests. A regression which adds one instruction to a hot
loop can also pass while materially reducing performance.

### Recommended Test Gate

Add a small optimization-specific suite which compiles known-liftable routines
with LLVM diagnostics enabled and checks all of the following:

1. Compilation succeeds with the real LLVM implementation, not
   `src/llvm_stub.c`.
2. The expected routines are lifted and lowered rather than silently falling
   back.
3. Classic and LLVM outputs have identical behavior.
4. Interpreter and `timeout` exit statuses are successful.
5. Each fixture remains below a narrow emitted-instruction ceiling.
6. Hot-loop fixtures remain below a weighted or measured dynamic instruction
   ceiling, not merely a whole-routine static count.

The compiler currently reports only aggregate input and output counts at
`src/llvm_codegen.c:1082-1089`. Tests would be more useful if level 3 output
also included per-routine captured and emitted counts in a stable,
machine-readable format.

The aggregate counters include only successfully lowered routines. If a
routine which previously optimized starts falling back, both its input and
output instructions disappear from the total. Aggregate counts can therefore
appear to improve while optimization coverage regresses. Count assertions must
always be paired with lifted/lowered coverage assertions.

### Initial Microbenchmarks

The first fixtures should isolate the transformations most likely to regress
dynamic instruction count:

- Store fusion: `Glob = Glob + 1` and `array-->i = array-->i + 1` should emit
  directly to their destination.
- Return fusion: comparison returns, `phi; ret`, and
  `return condition ? a : b`.
- Boolean trees: nested `&&`, `||`, and negation should retain short-circuit
  behavior and avoid materialized booleans.
- Phi and slot coalescing: loop induction values, phi chains, and parallel-copy
  cycles.
- VM-stack fusion: LIFO consumption, calls with stack arguments, and cases
  which must deliberately unfuse.
- Switches: early, middle, late, and default cases with a fallthrough target.
- Conditional phi edges: ensure a natural fallthrough does not gain an
  unnecessary jump because the opposite edge needs a stub.
- Division: source patterns which InstCombine converts from `sdiv` to `udiv`.
- LICM: zero-trip loops and conditionally executed invariant operations.
- Global and RAM aliasing: loads around direct global stores, opaque calls,
  possible RAM aliases, readonly operations, and stream operations under
  filter I/O which call back into arbitrary VM code.
- Dereference motion: repeated reads without a write should merge, reads
  around a possible write must not merge, and a faulting read on an untaken
  path must not be hoisted or deleted.
- RNG ordering: deterministic sequences with intervening RAM operations and
  repeated `setrandom` calls must preserve RNG ordering without unnecessarily
  blocking RAM optimization.
- Unsigned division and remainder boundaries: dividends around `0x7fffffff`
  and `0x80000000`, and divisors `1`, small even and odd values,
  `0x7fffffff`, `0x80000000`, and `0xffffffff`.
- Funnel shifts and rotates: `llvm.fshl` and `llvm.fshr` patterns with counts
  0, 1, 31, and values requiring masking by 31.
- Select-of-pointer repair: nested conditional global reads, including trees
  deeper than the current repair depth.
- Tail calls with stack arguments: verify pop order, an empty symbolic stack,
  and successful lowering rather than fallback.
- Cross-block comparison fusion: record the current excess count and guard a
  future implementation which extends operand liveness across blocks.
- Loop rotation: retain a fixture representing the Life induction/select shape
  so `loop-rotate` cannot be reintroduced without demonstrating that it no
  longer adds dispatches.

Static emitted count is useful for focused fixtures and per-change deltas, but
it is not a sufficient global profitability rule. The lowerer intentionally
accepts some static expansion to remove instructions from frequently executed
paths. Loop depth, branch likelihood, or interpreter-level instruction
instrumentation is needed to estimate dynamic cost.

For the Life benchmark, run each version multiple times and compare medians.
Record both elapsed time and interpreter instruction count where the
interpreter exposes it. A performance gate should tolerate normal timing noise
but fail a sustained instruction-count regression.

## Correctness Findings

### `jumpabs` Is Unsafe Under Per-Routine Optimization

`jumpabs` is classified as non-returning at `src/asm.c:802` and is lifted as an
opaque call followed by `unreachable` at `src/llvm_codegen.c:747-756`. Glulx
allows it to jump into the interior of a routine, but LLVM may remove or
reorder that routine's instructions and change its local-frame layout.

The compliance suite already observes this mismatch and suppresses it at
`tests/run-m3.sh:48-53` and `tests/run-m3.sh:75-80`. Programs using `jumpabs`
can therefore silently change behavior under the default LLVM level. A routine
containing `jumpabs` should not be optimized. Because one routine can jump into
another, a conservative implementation may need to disable routine rewriting
for the entire story when `jumpabs` is present.

### Potentially Faulting Reads Can Be Removed

`mark_fn_as_readonly()` applies `memory(read)`, `nounwind`, and `willreturn` at
`src/llvm_codegen.c:160-169`. It is used for dereferences and readonly Glulx
operations listed at `src/llvm_codegen.c:204-210`.

An unused read can consequently be removed even when an invalid address should
produce an observable VM fault. Inline assembly makes the assumption that all
addresses are compiler-generated and valid unsafe. `willreturn` is also not
necessarily true for malformed cyclic data passed to `linkedsearch`.

### Poison, Undef, and Freeze Semantics Need Proof

The lowerer resolves LLVM `undef` and `poison` values as zero and treats
`freeze` as a no-op. This is safe only if the lifter and pass pipeline never
allow LLVM undefined behavior or poison to represent behavior which Glulx
defines. Division and shift lifting already contain explicit guards because
the two semantic models differ, but there is no focused test or documented
invariant covering all other ways poison can arise.

Tests should exercise shift boundaries, signed overflow-sensitive
transformations, narrowed values, and selects whose unchosen arm could become
poison. If the invariant cannot be proven for arbitrary optimized IR, lowering
should reject poison-dependent values instead of silently choosing zero.

### Opcode Effect Classification Is Correctness-Critical

The pure, readonly, and inaccessible-memory opcode lists at
`src/llvm_codegen.c:192-240` control GVN, LICM, DCE, sinking, and the lowerer's
global-clobber analysis. A mistaken entry can reorder operations across a VM
callback or observable state change. Stream opcodes are correctly excluded
because filter I/O can invoke arbitrary code, but the classification needs
focused alias, callback, fault, and RNG-order tests rather than relying on
broad transcript coverage.

## Instruction-Count Findings

### Switch Fallthrough Can Penalize a Hot Case

At `src/llvm_lower.c:3080-3103`, the first switch case targeting the next block
is moved to the end of the comparison chain so that it can fall through. This
saves a jump on that path but moves the case behind every other comparison.

For a 63-case switch, a common first case can change from one comparison to 63
comparisons. Without branch-frequency information, the safer minimal choice is
to move only the last eligible case, which cannot increase the number of
comparisons needed by preceding cases.

### Signed Division Can Expand Into Unsigned Emulation

Safe constant signed division is lifted directly at
`src/llvm_codegen.c:583-596`. InstCombine can prove that a dividend is
nonnegative and rewrite `sdiv` as `udiv`. Glulx has a native signed division
instruction but no native unsigned division instruction.

The resulting `udiv` is expanded into shifts, signed division, multiplication,
subtraction, comparison, and correction at `src/llvm_lower.c:2723-2743`.
Patterns such as `(x & $7fffffff) / 3` can therefore replace one native
division dispatch with approximately seven dispatches. This needs a focused
regression fixture and either prevention of the canonicalization or a
cost-aware recovery of signed division when nonnegativity is known.

### LICM Can Add Work to Zero-Trip or Conditional Paths

LICM runs unconditionally at `src/llvm_codegen.c:67-81`, and pure opaque
operations are marked `speculatable` at `src/llvm_codegen.c:152-158`. This can
hoist an invariant operation out of a conditional or zero-trip loop, causing it
to execute once when it previously executed zero times.

LICM may be a large win when an operation would otherwise execute on every
iteration, but it is not always the "pure dynamic win" described in the
comment. Tests should cover both profitable and unprofitable loop shapes.

### Phi Stubs Defeat Natural Fallthrough

When one conditional edge needs a phi-copy stub,
`src/llvm_lower.c:3056-3069` passes `NULL` as the next block for the opposite
edge. `emit_goto()` then emits an explicit jump at
`src/llvm_lower.c:2550-2552`, even if that successor is physically next.

This adds one dispatch whenever the non-stub fallthrough edge is taken. Stub
placement or a final `jump L; L:` peephole should remove it.

### Returned Selects Materialize Ordinary Values

`ret_select_pass()` accepts only selects whose arms are both immediate values
at `src/llvm_lower.c:1611-1641`. The fused return emitter already resolves
ordinary operands at `src/llvm_lower.c:3000-3020`.

For `return condition ? a : b`, the current lowering can materialize the select
in a slot and then return it. Direct conditional returns avoid one or two
dispatches per invocation. The immediate-only restriction appears
unnecessarily conservative for adjacent select/return sequences.

### Select Ordering Assumes Immediates Are Rare

`select_swapped()` at `src/llvm_lower.c:965-980` assumes that an immediate true
arm is a rare sentinel and orders emission to favor the computed false arm.
The choice affects `src/llvm_lower.c:2676-2686`.

For the common idiom `ok ? 0 : error`, this can add one dispatch to the common
success path. Operand representation is not branch-frequency evidence. The
heuristic should use profile information, source ordering, or a neutral rule
which does not claim one arm is more likely.

### Select-to-Store Is Not Folded

Store folding at `src/llvm_lower.c:1567-1595` requires `stackable_def()`, which
excludes selects. An adjacent sequence such as
`Glob = condition ? a : b` therefore materializes a local select result and
then copies it to the destination.

Writing each select arm directly to the final store target would save one
dispatch on every path. It needs the same source-before-destination and
clobber checks used by existing store folding.

### Fixed Limits Can Hide Coverage Regressions

The lifter and lowerer use fixed limits for symbolic stacks, entry stacks,
pending fused values, connective-tree depth, edge stubs, reads, and local
slots. Exceeding most of these limits safely falls back to classic output, but
the normal test suite does not assert optimization coverage and will not
notice the loss.

Tests should include fixtures near each practical limit and assert either
successful lowering or the exact expected bailout reason. Aggregate bailout
counts should be tracked for larger corpus tests so an LLVM upgrade cannot
silently reduce coverage.

## Profitability Model

Successfully lowered routines replace the original stream unconditionally at
`src/llvm_codegen.c:1064-1073` and `src/llvm_lower.c:3208-3247`. The current
aggregate statistic records static counts but does not influence acceptance.

A blanket rule rejecting output whenever its static count exceeds the classic
count would be incorrect: additional cold-path instructions can remove work
from a hot loop. A useful per-routine model should instead estimate dynamic
dispatches from the CFG, with at least loop-depth weighting and conservative
branch probabilities. The original captured stream is already available as a
fallback and can be retained when the estimated dynamic cost increases.

Until such a model exists, focused instruction-count tests are the most
important protection. They make each intended tradeoff explicit and prevent
LLVM version changes or lowerer refactoring from silently spending IR-level
gains on additional VM dispatches.

## Detailed Test Matrix

### Harness Integrity

- Fail when the compiler is built with `src/llvm_stub.c` instead of the real
  LLVM pipeline.
- Fail when a required interpreter is absent in CI; retain optional local
  skips only behind an explicit environment setting.
- Check compile, interpreter, and `timeout` exit statuses independently.
- Require a completion marker as well as transcript equality.
- Assert exact expected `glulxercise` failures and their count.
- Capture compiler diagnostics instead of discarding them.
- Assert expected minimum lifted/lowered routine counts and maximum bailout
  counts.
- Keep one Z-machine baseline test to protect assembler changes shared with
  the Glulx capture seam.

### Representation and Fusion

- Direct global operands with and without intervening clobbers.
- Store fusion to globals and dereferences, including self-updates.
- Select-to-store fusion and cases where an intervening emission must prevent
  it.
- Stack fusion in valid LIFO order, invalid pop order, around stack-argument
  calls, and at the pending-value limit.
- Slot reuse where the previous tenant dies before the new definition, and a
  negative case where it remains live.
- Phi-to-parameter and phi-to-phi coalescing.
- Parallel-copy cycles which require scratch slots.
- Loop edge-folding with and without a select rematerialization use.

### Control Flow

- Comparison returns and select returns with constant, parameter, local, and
  global arms.
- Lone return blocks, return phis, and return blocks with multiple
  predecessors.
- Nested short-circuit `&&`, `||`, and negation, including side effects which
  prove an unused arm is not executed.
- Branch-tree movement across pure operations and rejection across writes.
- Switch fallthrough with common cases in early, middle, and late positions.
- Phi stubs on either branch while the other branch is the physical
  fallthrough.
- Cross-block single-use comparisons.

### VM and LLVM Semantics

- Signed division and modulo fault cases, unsigned canonicalization, and all
  constant-lowering ranges.
- Shift counts 0, 31, 32, negative values interpreted as unsigned, and rotate
  idioms.
- Repeated and conditionally executed dereferences, including invalid
  addresses where inline assembly permits them.
- Global/RAM aliasing around stores and opaque calls.
- Filter-I/O callbacks which mutate globals.
- Readonly search operations over valid and malformed structures.
- RNG state ordering independent of RAM motion.
- Poison-producing or poison-sensitive LLVM transformations.
- Tail calls, ordinary calls, and Glk calls with stack arguments.
- Nested pointer selects around global loads.
- `jumpabs`, catch tokens, and other layout-sensitive behavior as explicit
  expected incompatibilities rather than substring-filtered failures.

### Limits and Version Stability

- Symbolic stack and label-entry stack boundaries.
- Maximum pending fused values and readset size.
- Connective-tree depth.
- Edge-stub and local-slot pressure.
- An LLVM-version smoke test which verifies the expected post-pass shapes still
  lower successfully.
- Corpus-level tracking of optimized, lift-bailed, and lower-bailed routines.

## Documentation and Maintenance Notes

- `DESIGN.md` says the M1 identity gate covers both targets, while the script
  currently tests only Glulx.
- The design's earlier recommendation that optimization remain off by default
  conflicts with the current default LLVM level of 2.
- The `glulxercise` catch-token explanation still says there is one local slot
  per SSA value, predating slot reuse.
- README M5 and corpus-validation work remains open, and the TODO wording still
  refers to M4 coverage after M4 is marked complete.
- Compile-time `lookup()` scans remain quadratic in routine size. This does not
  affect interpreted instruction count, but should be measured if larger
  corpus testing exposes compiler-time problems.

The main implementation history relevant to regression baselines is:

- `ad7b8555`: initial LLVM capture and lifting pipeline.
- `e01041b`: full lift/optimize/lower round trip.
- `3b1e0b8`: veneer and `glulxercise` compliance coverage.
- `fb817d9`: Game of Life benchmark.
- `16b0eb1`: M4 lowering quality, stack phis, fusion, and slot reuse.
- `9227ad1`: post-M4 instruction-count reduction series.

The latter two commits establish the most useful baseline for per-feature
instruction-count tests because they introduced most of the transformations
whose performance behavior is currently unguarded.

## Recommended Order of Work

1. Add an LLVM-required optimization test suite with per-routine count output,
   successful-lowering assertions, and interpreter exit checks.
2. Add focused regression fixtures for switches, phi stubs, select returns,
   division canonicalization, and LICM.
3. Put the real LLVM build and optimization suite in Linux CI.
4. Disable unsafe optimization in the presence of `jumpabs` and correct the
   removable-read attributes.
5. Fix the demonstrated instruction-count regressions, beginning with switch
   ordering and phi-stub fallthrough.
6. Develop a CFG-weighted dynamic profitability estimate before making
   routine replacement conditional on cost.

At the time of review, `make test` passed all existing tests. One `make bench`
run reported 902 ms for classic output and 854 ms for LLVM output. These values
are useful as a smoke test but are not statistically sufficient to establish a
performance threshold.
