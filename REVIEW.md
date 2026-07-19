# LLVM Pipeline Review

This review treats dynamically dispatched Glulx instruction count as the
primary performance metric, but not the whole cost model. Every additional VM
instruction incurs a fetch/decode/dispatch cycle, and count has been the best
predictor of interpreter performance observed so far. Individual opcode
handlers, operand modes, memory behavior, Glk calls, host branch prediction,
and accelerated routines nevertheless have different costs. Instruction count
should therefore dominate optimization decisions without being treated as a
perfectly weighted runtime estimate.

The pipeline already reflects this model in several important places,
particularly stack fusion, direct global operands, phi coalescing, select-arm
sinking, connective-tree reconstruction, recurrence folding, block layout, and
the decision not to run `loop-rotate`. The focused regression gate protects
representative forms.

## Measured Performance Model

The patched `glulxe-counted` interpreter increments a 64-bit counter after an
opcode and its operands have been decoded and immediately before its handler is
dispatched. It counts every successfully decoded Glulx instruction executed in
called routines and across VM restarts. It does not count interpreter
bookkeeping, work within an opcode handler as separate operations, or native C
work performed by accelerated routines.

The Life benchmark measurement recorded after recurrence folding and block
layout was:

```text
classic: 56,177,197 instructions
LLVM:    54,609,633 instructions (-2.79%)
```

Before the recurrence-folding improvement described below, LLVM executed
57,579,709 instructions (`+2.50%`). After alternating execution order, one
five-run sample of that earlier build measured medians of
932 ms classic and 941 ms LLVM. This points in the expected direction, but the
timing delta is not exactly proportional to instruction count and individual
samples remain noisy. Deterministic dispatch totals are the preferred
regression gate; repeated timing medians remain an important secondary check
for changes in opcode mix or other costs which the unweighted counter misses.
After recurrence folding and block layout, one alternating nine-run sample
measured medians of 864 ms classic and 849 ms LLVM (ranges 849-910 ms and
824-932 ms respectively).

Static emitted counts are less predictive than dynamic counts because cold
blocks and hot loops are weighted equally. They remain useful for isolated
microfixtures and explaining where a transformation added bytecode.

## Opcode Cost Investigation

The initial priority was to explain the Life benchmark's additional 1,402,512
dispatches and determine how much the opcode mix affected runtime. Total
dynamic count showed that a regression existed, but not whether LLVM added
copies, branches, arithmetic, address calculations, or expensive VM operations.

`glulxe-counted` accepts `--opcode-histogram` and emits the total plus one
machine-readable count for every executed opcode. `tests/lifeBenchmark.nix`
validates that the histogram sums to the total and writes a temporary
classic/LLVM comparison during the benchmark run. Collection remains optional
so the simplest total-count path stays cheap and stable.

The first Life comparison attributes most of the changed mix to the hot
`Step` loop:

```text
opcode  operation  classic     LLVM        delta
0x040   copy          75,887   3,199,994   +3,124,107
0x020   jump       1,785,680   3,211,284   +1,425,604
0x025   jne        2,994,076   4,493,392   +1,499,316
0x028   jgt              501   1,565,547   +1,565,046
0x010   add       29,405,083  27,891,973   -1,513,110
0x011   sub        1,535,509           9   -1,535,500
0x023   jnz        1,561,007           0   -1,561,007
0x027   jge        1,611,844           0   -1,611,844
```

The branch changes are largely predicate and block-layout substitutions. The
copy increase is more directly actionable: about 3,120,000 executions are
explained by loop-carried values in `Step`, including two copies per cell from
an increment shared by a wraparound select and the loop phi. The lowerer's
edge-folding pass already targeted this shape, but its same-block restriction
did not accept the post-optimization CFG used by Life.

The edge-folding pass now accepts the narrow cross-block form where an `add` or
`sub` reads the destination phi and an integer constant. On Life this removes
3,072,000 copies, adds back 1,536,000 native additions, and moves 24,000 jumps
onto the rare wraparound path. The net reduction is exactly 1,536,000
dispatches, enough to put the LLVM build 133,488 instructions below classic.

The remaining excess jumps came from the `Step` cell-state diamond. LLVM placed
the live arm, dead arm, secondary `n == 2` test, and merge in that order, which
forced the common dead arm to jump over the secondary test. The lowerer now
recognizes this local four-block shape and emits the secondary test before the
dead arm. This is pathwise non-worsening: the live arm retains its merge jump,
the secondary test gains a fallthrough arm, and the dead arm falls through to
the merge after performing its phi copies. It removes another 1,434,076 Life
dispatches and leaves only 15,528 more LLVM `jump` executions than classic.

The histogram is also the prerequisite for a weighted interpreter cost model.
Per-opcode costs should be measured rather than guessed:

1. Construct controlled Glulx loops dominated by one opcode family.
2. Run enough iterations to separate handler cost from timer noise and fixed
   startup/loop overhead.
3. Measure important operand modes separately where decoding or memory access
   differs, such as constants, locals, globals, stack operands, and RAM
   dereferences.
4. Repeat across the interpreters used for project benchmarks; do not assume
   that one interpreter's opcode weights transfer to another.
5. Fit or validate a simple weighted sum against whole-program timings before
   using it for optimization decisions.

The expected outcome is a hierarchy of metrics:

- Dynamic total: deterministic primary regression signal.
- Opcode histogram: identifies what kind of interpreted work changed.
- Weighted dynamic cost: estimates runtime when opcode mixes differ.
- Repeated wall-clock timing: validates that the model predicts real execution.

After this source-level attribution, add routine or PC-range instrumentation to
identify where opcode differences and remaining locally excessive operations
occur. This requires routine boundaries from the compiler or story metadata,
but would turn whole-program differences into a ranked list of optimization
targets. This can remain deterministic and generic; it does not require PGO.

Benchmark output should eventually have a machine-readable JSON or TSV mode
recording compiler revision, LLVM and interpreter versions, optimized/captured
routine counts, static instructions, opcode histogram, weighted cost, dynamic
total, story size, and timing samples. This will make comparisons across
commits and LLVM upgrades reproducible.

### Pinned Interpreter Sources

`flake.lock` pins the interpreter sources used by this work:

- Glulxe `56ab8743bab565de307bd892c555d8d8897ed517`
- CheapGlk `14d8aaf6e4150669762bd4646a5368e75c1eeee6`

The Glulxe dispatch loop is in `exec.c`; process setup, normal exit, and fatal
exit handling are in `main.c`; shared declarations are in `glulxe.h`. The
counting patch applied to these files is
`patches/glulxe-instruction-count.patch`.

After a lock-file update, resolve the new source paths instead of searching the
store manually:

```sh
nix eval --raw .#glulxe.src
nix eval --raw .#cheapglk.src
nix eval --raw .#glulxe-counted.src
```

Built package paths should not be recorded here because they vary by system and
derivation changes; use `nix build .#glulxe-counted --print-out-paths` when a
binary path is needed.

## Optimization Tests

The initial review found useful behavioral coverage but almost no
optimization-quality regression coverage. The first test-suite work has since
addressed the most serious harness gaps:

- `tests/captureReplayTest.nix` proves that capture/replay remains byte-identical.
- `tests/optimizationTest.nix` uses a real LLVM build and asserts that a
  focused fixture lifts and lowers completely, and enforces aggregate,
  per-routine static, and dynamic instruction ceilings. It also compares
  classic and LLVM behavior for an unused faulting read.
- `tests/complianceTest.nix` compares classic and optimized interpreter transcripts.
- `tests/lifeBenchmark.nix` alternates execution order, reports timing median/min/max,
  and reports deterministic dynamic instruction totals.
- Interpreter and timeout statuses are checked explicitly.
- The `glulxercise` gate asserts the exact count of known layout-sensitive
  failures rather than accepting an unlimited substring match.
- Level 3 compiler output includes per-routine captured/emitted static counts.
- The real LLVM pipeline is not exercised by CI; the Windows build uses the
  no-LLVM stub.
- `tests/captureReplayTest.nix` exercises only Glulx; there is no Z-machine baseline for
  assembler changes shared with the capture seam.

The focused fixture currently requires 12 of 12 captured routines to lower,
with zero lift or lowering bailouts. It checks exactly 156 aggregate input
instructions, at most 175 emitted instructions, and exactly 396 classic dynamic
dispatches with an LLVM ceiling of 444. Its named static checks cover store
fusion, comparison and select returns, boolean trees, loop phis, recurrence
folding, four-block layout, switch order, and shared-target switch cases.
Broader fixtures still need coverage assertions so corpus routines cannot
silently stop optimizing.

The compiler reports aggregate and per-routine input/output counts. A future
dedicated machine-readable diagnostics mode would be less brittle than parsing
human-facing level 3 output, especially for corpus-scale result collection.

The aggregate counters include only successfully lowered routines. If a
routine which previously optimized starts falling back, both its input and
output instructions disappear from the total. Aggregate counts can therefore
appear to improve while optimization coverage regresses. Count assertions must
always be paired with lifted/lowered coverage assertions.

### Open Focused Coverage

`stories/optimization-regressions.inf` already covers store fusion, comparison
and non-immediate select returns, boolean trees, loop phis, recurrence folding
and loop-rotation sensitivity, four-block layout, and switch ordering. The most
useful remaining focused fixtures are:

- Phi and slot coalescing: loop induction values, phi chains, and parallel-copy
  cycles.
- VM-stack fusion: LIFO consumption, calls with stack arguments, and cases
  which must deliberately unfuse.
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

Static emitted count is useful for focused fixtures and per-change deltas, but
it is not a sufficient global profitability rule. The lowerer intentionally
accepts some static expansion to remove instructions from frequently executed
paths. Loop structure, pathwise-safe rules, target instruction costs, or
interpreter-level instrumentation is needed to estimate dynamic cost without
requiring application profiles.

The Life benchmark runs each version multiple times, compares medians, and
reports rather than gates its dynamic count.

## Correctness Findings

### `jumpabs` And Computed Code Addresses: Documented Policy

`jumpabs` is classified as non-returning at `src/asm.c:802` and is lifted as an
opaque call followed by `unreachable` at `src/llvm_codegen.c:743-755`. Glulx
allows it to jump into the interior of a routine, but LLVM may remove or
reorder that routine's instructions and change its local-frame layout.

DM4 §41 says that Inform branch labels are routine-local, but this restriction
does not define `jumpabs`: its operand is an ordinary computed value rather
than an Inform branch-label operand. The Glulx specification (§2.2 "Branches")
explicitly permits branching into another function and notes that a function
has no well-defined end. It defines `jumpabs` as an unconditional branch to an
absolute address.

What Inform does not guarantee is the generated instruction layout needed to
derive an interior address from a routine symbol. Glulxercise relies on that
layout with `pos = test_jumpabs_2+5`; optimization and compiler-version changes
may both invalidate the offset.

Policy: programs that compute code addresses are unsupported under LLVM
optimization, in the same way they are unsupported across upstream compiler
versions. Handling `jumpabs` routine-locally as an opaque non-returning
operation is sufficient; no story-wide preflight, whole-story buffering, or
layout-preserving mode is planned. `tests/complianceTest.nix` pins the known
out-of-contract glulxercise failures (one `jumpabs` check, ten catch-token
checks) exactly, so any change in the accepted failure set is still detected.
The compiler warns whenever LLVM optimization encounters `jumpabs`, because it
cannot prove whether the address depends on generated layout.

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
`src/llvm_codegen.c:188-237` control GVN, LICM, DCE, sinking, and the lowerer's
global-clobber analysis. A mistaken entry can reorder operations across a VM
callback or observable state change. Stream opcodes are correctly excluded
because filter I/O can invoke arbitrary code, but the classification needs
focused alias, callback, fault, and RNG-order tests rather than relying on
broad transcript coverage.

## Instruction-Count Findings

### Signed Division Can Expand Into Unsigned Emulation

Safe constant signed division is lifted directly at
`src/llvm_codegen.c:579-593`. InstCombine can prove that a dividend is
nonnegative and rewrite `sdiv` as `udiv`. Glulx has a native signed division
instruction but no native unsigned division instruction.

The resulting `udiv` is expanded into shifts, signed division, multiplication,
subtraction, comparison, and correction at `src/llvm_lower.c:2783-2837`.
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

During emission planning, a conditional target requiring a phi-copy stub causes
`plan_terminator()` to force a jump on the opposite edge at
`src/llvm_lower.c:3117-3129`. That edge is planned as `EDGE_DIRECT` rather than
`EDGE_FALLTHROUGH`, and `emit_terminator()` emits its explicit jump before
flushing the stub at `src/llvm_lower.c:3226-3232`.

This adds one dispatch whenever the non-stub fallthrough edge is taken. Stub
placement or a final `jump L; L:` peephole should remove it.

### Returned Selects Materialize Ordinary Values

`ret_select_pass()` accepts only selects whose arms are both immediate values
at `src/llvm_lower.c:1631-1661`. The fused return emitter already resolves
ordinary operands at `src/llvm_lower.c:3185-3205`.

For `return condition ? a : b`, the current lowering can materialize the select
in a slot and then return it. Direct conditional returns avoid one or two
dispatches per invocation. The immediate-only restriction appears
unnecessarily conservative for adjacent select/return sequences.

### Select Ordering Assumes Immediates Are Rare

`select_swapped()` at `src/llvm_lower.c:965-980` assumes that an immediate true
arm is a rare sentinel and orders emission to favor the computed false arm.
The choice affects `src/llvm_lower.c:2769-2779`.

For the common idiom `ok ? 0 : error`, this can add one dispatch to the common
success path. Operand representation is not branch-frequency evidence. The
heuristic should use structural evidence, source ordering, or a neutral rule
which does not claim one arm is more likely.

### Select-to-Store Is Not Folded

`store_fold_pass()` at `src/llvm_lower.c:1587-1615` requires
`stackable_def()`, which explicitly rejects selects at
`src/llvm_lower.c:1827-1850`. An adjacent sequence such as
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

Successfully lowered routines replace the captured stream unconditionally
through `src/llvm_codegen.c:1062-1076`; the lowerer resets and rewrites the
capture buffer at `src/llvm_lower.c:3444-3488`. The current aggregate statistic
records static counts but does not influence acceptance.

A blanket rule rejecting output whenever its static count exceeds the classic
count would be incorrect: additional cold-path instructions can remove work
from a hot loop. A useful generic per-routine model should instead estimate
cost from CFG loop structure, pathwise-safe transformations, and measured
target instruction costs. This does not require PGO. The original captured
stream is already available as a fallback and can be retained when the
estimated cost increases.

Until such a model exists, focused instruction-count tests are the most
important protection. They make each intended tradeoff explicit and prevent
LLVM version changes or lowerer refactoring from silently spending IR-level
gains on additional VM dispatches.

## Remaining Test Matrix

### Harness Integrity

- Run the real LLVM build and strict Nix test suite in Linux CI.
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
  expected incompatibilities rather than substring-filtered failures, per the
  computed-code-address policy above.

### Limits and Version Stability

- Symbolic stack and label-entry stack boundaries.
- Maximum pending fused values and readset size.
- Connective-tree depth.
- Edge-stub and local-slot pressure.
- An LLVM-version smoke test which verifies the expected post-pass shapes still
  lower successfully.
- Corpus-level tracking of optimized, lift-bailed, and lower-bailed routines.

## Documentation and Maintenance Notes

- The capture/replay identity test currently covers only Glulx.
- Corpus-scale dual compilation, behavioral comparison, code-size measurement,
  and dynamic-instruction reporting remain open.
- Compile-time `lookup()` scans remain quadratic in routine size. This does not
  affect interpreted instruction count, but should be measured if larger
  corpus testing exposes compiler-time problems.

## Recommended Order of Work

1. Resolved as policy: computed code addresses (including cross-routine
   `jumpabs` targets) are documented as unsupported under optimization; see
   the correctness finding above. No story-wide mechanism is planned.
2. Add focused fixtures for phi-stub fallthrough, unsigned-division
   canonicalization, and zero-trip or conditional LICM before changing those
   transformations.
3. Fix demonstrated instruction-count regressions, beginning with phi-stub
   fallthrough, and tighten the existing `Opt_SelectReturn` ceiling when
   ordinary-arm return fusion is implemented.
4. Put the real LLVM build and strict optimization suite in Linux CI.
5. Measure per-opcode and important operand-mode costs, then validate a
   weighted model against repeated whole-program timings.
6. Add routine or PC-range attribution for remaining Life opcode differences.
7. Develop a generic CFG profitability estimate based on loop structure and
   measured target costs before making routine replacement conditional on cost.
