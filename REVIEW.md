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
layout was 54,609,633 LLVM instructions (`-2.79%`). The current measurement,
after the multi-use store fold described under the Phase 2 findings, is:

```text
classic: 56,177,197 instructions
LLVM:    54,605,529 instructions (-2.80%)
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

### Pinned Baseline Sources

`flake.lock` pins the compiler and interpreter sources used by this work:

- Inform 6 `d1066bc214a45ee0f600d2ae7f94ad0210606317`
- Glulxe `56ab8743bab565de307bd892c555d8d8897ed517`
- CheapGlk `14d8aaf6e4150669762bd4646a5368e75c1eeee6`

The Inform revision is the parent of this fork's initial source-layout commit;
the moved C sources are unchanged at that boundary. Nix packages it as
`inform6-upstream`. Behavioral, compliance, optimization, and benchmark
classic stories use that package. The temporary capture/replay test instead
compares the fork's `$LLVM=0` and `$LLVM=1` modes byte-for-byte so it continues
to isolate capture behavior.

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
nix eval --raw .#inform6-upstream.src
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
- `tests/directIrTest.nix` requires straight-line expressions, structural
  conditions, short-circuit values, branches, loops, nested switches,
  break/continue edges, terminal loops, and unreachable source to lower from
  direct IR. It
  compares behavior with upstream, checks exact direct/fallback diagnostics,
  and verifies byte-identical shadow replay after forced builder and lowering
  failures.
- `tests/zMachineTest.nix` requires byte-identical Z5 output from pinned
  upstream Inform and the fork for a focused shared-assembler fixture. It also
  extracts Z and Glulx debug sequence-point locations from fork and pinned
  upstream `-k` compiles of the same fixture and requires them to match, so
  locations cannot move unnoticed and no golden values need maintenance.
- `tests/lifeBenchmark.nix` alternates execution order, reports timing median/min/max,
  and reports deterministic dynamic instruction totals.
- Interpreter and timeout statuses are checked explicitly.
- The `glulxercise` gate asserts the exact count of known layout-sensitive
  failures rather than accepting an unlimited substring match.
- Level 3 compiler output includes per-routine captured/emitted static counts
  and `LLVM-BACKEND` TSV records identifying lifted output or categorized
  classic fallback. The focused test also forces the lowering limit to zero
  and asserts that every captured routine reports fallback.
- The real LLVM pipeline is not exercised by CI; the Windows build uses the
  no-LLVM stub.
- The Z-machine baseline is intentionally focused; it does not yet run a broad
  Z-machine corpus or interpreter-level compliance suite.

Direct IR remains an explicit migration mode at `$LLVM=4`; the production
default continues to use the lifter. Phase 2 directly generates all
straight-line source arithmetic, bitwise and signed comparison operators,
comparison-to-word conversion, comma sequencing, local/global pre/post updates,
symbolic constants, and value-producing assignments. Ordinary operands retain
Inform's right-to-left evaluation order; comma remains left-to-right.
Variadic comparison lists preserve Inform's less obvious rule: positive
predicates (`==`, `<`, `>`) combine with OR, while negated predicates (`~=`,
`<=`, `>=`) combine with AND.

Phase 3 directly represents condition context as LLVM branches. Logical `&&`,
`||`, and parser-normalized negation retain short-circuit structure; logical
expressions used as words join explicit zero and one arms with a phi. `if` and
`else`, `while`, `do`/`until`, all ordinary `for` layouts, `break`, `continue`,
source labels, and returns share the same opaque block API. Locals remain
allocas before optimization, so LLVM promotion forms loop-carried phis rather
than requiring the streaming parser to construct SSA.

Switch selectors are evaluated once. Cases, alternatives, and ranges become an
ordered comparison chain rather than an LLVM switch whose cases could be
reordered, preserving source comparison order without guessing branch
frequency from operand shape. Nested switches keep a selector stack, and a
switch inside a loop preserves the enclosing continue target. Parser-elided
forward exits are terminated as dead LLVM blocks without changing the current
insertion point; this lets terminal loops and all-return switches verify while
shadow assembly remains the sole writer of parser reachability and label-use
state.

The Phase 3 focused fixture matches pinned upstream behavior and guards direct
coverage, static instruction ceilings, dynamic dispatches, and forced
fallback. A separate manual clean build verifies the no-LLVM stub configuration.
Direct output uses fewer dynamic dispatches than upstream for that fixture.
Life coverage is unchanged because its remaining fallbacks require the calls,
memory, VM effects, and inline assembly assigned to Phase 4; direct mode
therefore remains instruction-identical to upstream on Life.

Division and remainder use the lifter's safety policy: a visibly safe constant
divisor uses native LLVM signed arithmetic, while zero, minus one, and variable
divisors remain opaque Glulx operations. When source runtime checks are enabled,
a potentially zero divisor first branches through the `RT__Err` veneer and
substitutes one exactly as classic generation does. This keeps division by zero
and `INT_MIN / -1` out of LLVM undefined behavior. Inform has no source shift or
cast operator; comparison `i1` to word conversion is the relevant Phase 2
conversion, while Glulx shifts enter through Phase 4 inline assembly.

The Phase 2 Life benchmark has low direct coverage: only `Rnd` builds directly,
while fourteen routines fall back. An earlier run measured direct mode at
56,181,301 instructions against upstream's 56,177,197, a 4,104-instruction
increase. The cause was not division: `Rnd`'s `sdiv` by `$10000` survives
optimization intact and lowers to one native `div`. The extra dispatch per call
was a lowerer coalescing gap for a value that is stored to a global and then
reused: classic writes the `add` result directly into `seed` and has the
division read the global back, while the lowerer materialized the value in a
temporary local plus a `copy` to the global. Both backends emitted the
identical six-instruction routine, so the lifted production path carried the
same +4,104, masked by its wins elsewhere; the `udiv` canonicalization hazard
documented below is real but was not implicated.

The lowerer's store fold now has a multi-use variant: a def stored to an i6
global immediately after it is computed (the single-use fold's emit-nothing
window) may keep its remaining same-block uses if the global is unclobbered
from the store to each read; the def then writes the global directly and the
reads use the GLOBALVAR operand, so the temporary local and the copy
disappear. Reads that later passes relocate are guarded by the same
`arm_globals_stable` span check that protects direct global-load operands,
and the focused fixture pins both the fold (`Opt_GlobalCoalesce`, 5 -> 5)
and the mandatory decline across a clobbering call (`Opt_CoalesceClobber`).
After the fold, direct mode matches upstream exactly on Life (56,177,197,
+0.00%) and the lifted production path improved by the predicted 4,104
instructions to 54,605,529 (-2.80%).

Phase 4 began with source-level function calls. Direct generation emits the
same opcode selection as classic: `i6.callf` through `i6.callfiii` for zero to
three arguments, and `i6.call.s` for wider calls, with stack-passed arguments
carried as explicit `.s` call operands in runtime pop order. Call declarations
stay unmarked in the effect scheme, so every call is a full optimization
barrier for globals and RAM. `indirect()` evaluates its first argument as the
function; `glk()` and `metaclass()` become calls to the `Glk__Wrap` and
`Metaclass_VR` veneer routines, matching classic. Other system functions
(`random`, the object-tree family) still reject with an explicit reason; they
need the Phase 4 memory operations. The parser's `push on stack` wrapper
around wide-call arguments unwraps to the argument value itself.

Adding calls exposed a latent direct-generation evaluation-order defect:
classic emits subtree code right to left, but a leaf variable operand is read
by the consuming instruction itself, after *all* subtree code. The direct
backend was loading leaf globals at tree-walk time, so `f() + g` read `g`
before `f` ran while upstream reads it after. Binary operators, variadic
comparison lists, and call argument lists now defer leaf variable loads until
after sibling subtree code, restoring classic read timing (the
`Direct_CallOrder` fixture pins this with a call that mutates the global).

The lowerer's stack fusion now understands `.s` calls: a contiguous deepest
tail of a call's argument push order that is already sitting on top of the
pending-stack simulation stays on the VM stack, and those explicit pushes are
elided. Because direct generation evaluates arguments right to left, a wide
call whose arguments are all fusable defs (for example five nested calls)
lowers to classic's exact shape with zero temporaries. This benefits the
lifted production path identically. Remaining known gap: constants interleaved
between effectful wide-call arguments cannot ride the stack because classic
interleaves their pushes between the argument computations while the lowerer
pushes at the call site; `Direct_CallWideMixed` pins the resulting +2
instructions as an accepted static ceiling until emission learns interleaved
pushes.

With calls supported, Life's `DrawCell` joined `Rnd` in direct coverage and
direct mode remained dynamically identical to upstream (56,177,197, +0.00%).

The next Phase 4 slice added memory and object operations. Byte and word
array reads, writes, and increments become opaque `aloadb`/`aload`/
`astoreb`/`astore` operations (reads readonly, writes full barriers, so a
computed store cannot reorder against Inform global accesses that share the
Glulx memory map). Strict mode mirrors classic's three checked shapes: a
compile-time-checked plain opcode for recognized arrays with constant
indices, an inline bounds check branching to `RT__Err` for recognized arrays
(reads yield zero on the failed arm via a phi), and `RT__ChLDW`-family
veneer calls for unrecognized addresses; `direct_array_bounds` mirrors the
classic bounds table without repeating its warnings. Property operators
(`.`, `.&`, `.#`, `.()`, their write and increment forms, `superclass`,
`ofclass`, `provides`) become the same veneer calls classic emits, in both
strict and unchecked modes. `has`/`hasnt` lower to `aloadbit`, `in`/`notin`
to a parent-field `aload` comparison, and `parent`/`child`/`sibling`/
`eldest`/`younger` to object-field loads — in unchecked or veneer mode;
their strict forms need classic's inline metaclass-check CFG and reject
with explicit `strict attribute check`/`strict object-tree check` reasons
until that is generated. Single-argument `random()` follows classic's three
shapes (constant range, constant seed, variable branch); multi-argument
`random()` rejects because its word array is a classic-generation side
effect. The `direct-ir-memory` fixture compiles strict and unchecked,
matches upstream transcripts (including the strict out-of-bounds
diagnostic), and pins per-mode coverage and dynamic ceilings; direct mode
beats upstream in both (1,056 vs 1,076 strict; 648 vs 683 unchecked).
`Meta__class` is the first veneer routine to compile through direct IR.

With memory operations, Life direct coverage reached 8 of 15 routines and
direct mode dropped below upstream for the first time: 54,632,155 dynamic
dispatches (-2.75%), within 0.05% of the lifted production path's
54,605,529 (-2.80%). The remaining fallbacks were print statements and
inline assembly.

The third Phase 4 slice added statements. Print items mirror classic's
per-item emissions as they parse: literal strings and `(string)` become
`streamstr`, `(char)` chooses `streamchar` or `streamunichar` by classic's
constant test, plain items become `streamnum`, `(object)` reads the
short-name field, and the article, `(name)`, `(number)`, `(property)`,
`(address)`, custom-routine, and strict-mode `RT__ChPrint*` forms all
become calls to the same routines classic calls. Stream operations stay
unmarked in the effect scheme, so filter I/O callbacks cannot reorder.
`print_ret` and string statements terminate with a direct `return 1`.
`new_line`, `give` (both the `astorebit` form and the strict `RT__ChG`
veneers), `move`, `remove`, `font`, `style`, `string`, and `quit` (an
opaque non-returning operation) generate directly; `box`, `read`,
`spaces`, and `objectloop` still reject — `spaces` and `objectloop` need
loop CFGs the statement layer does not build yet, and `box` shares
`random(...)`'s classic-side table effect. A new `llvm_direct_quantity`
hook evaluates statement operand trees before classic generation consumes
them.

The strict-mode object guard from `check_nonzero_at_runtime_g` is now
generated directly: zero and non-object values call `RT__Err` and either
substitute the `Object` class-object (quantity contexts such as
`parent()`) or force the condition false (`has`, `in`), exactly matching
classic's error labels; variable attribute numbers additionally range
check against `NUM_ATTR_BYTES` with the INVALIDATTR report. With that CFG
in place, `has`/`hasnt`, `in`/`notin`, and the object-tree functions
compile directly in strict mode too, and the strict memory fixture runs
all 32 focused routines through direct IR in both modes (1,409 vs
upstream's 1,420 strict, 798 vs 839 unchecked).

One fixture lesson recorded for later phases: transcripts must not print
raw object or routine addresses. RAM layout shifts across a 256-byte
alignment boundary whenever generated code size changes, so address-valued
results (`parent(o)`, bare `(the) o` without a library) must be compared
against known objects instead of printed.

After statements, Life direct coverage was 11 of 15 routines (adding
`PrintGrid`, `PrintElapsed`, `WaitKey`) at 54,616,168 dynamic dispatches
(-2.78%); the remaining fallbacks were `Main` and the inline-assembly
routines assigned to the next slice.

The fourth Phase 4 slice translates inline assembly. Parsed
`assembly_instruction` records translate under the lifter's rules — native
IR for arithmetic, safe divisions, constant shifts, sign extensions, and
copies; explicit control flow for the branch family, `@jump`, and
`@return`; typed opaque calls with graded effects for everything else,
non-returning opcodes ending their block. Operands naming `sp` ride a
parse-time symbolic stack of SSA values, consumed by `call`/`glk`
constant-count argument peeling and the `@push`/`@pull`/`@dload`/`@dstore`
macros. Because a symbolic value has no runtime stack presence, the stack
must be empty wherever control flow diverges or joins (source labels,
jumps, conditions, switch selectors, inline branches); violations reject.
Custom `@"..."` opcodes, `@catch`/`@throw`, explicit stack rearrangement
(`@stkswap` family), and raw `@ ->` code bytes reject with focused-fixture
reasons. With this, `Main` startup code, `glk` invocations, and most
veneer routines compile directly; the remaining fallbacks are
stack-argument routines (`Glk__Wrap`, `CA__Pr`, `Cl__Ms`), routines
containing directives, and the compiler-generated `Main__`/`Symb__Tab`
stubs.

Direct veneer coverage exposed a lowering cost: `simplifycfg` tail-merges
every `return` into one ret-phi block, and the planner emitted each
conditional edge into it as a stub plus a forced jump. Edge planning now
classifies return edges: a constant 0/1 return uses Glulx's rfalse/rtrue
branch encodings from either side of a conditional, any other return
prefers the goto side as an inline `return v`, and only the leftover cases
stub. Return blocks that lose their last reference drop their bodies, with
planning re-run to a fixpoint since each drop can create new fallthroughs.
`jump-threading` joined the pass pipeline: strict-mode guards join through
a boolean phi that the consuming condition immediately re-tests, and
threading restores classic's direct jumps to each outcome. These lowering
changes serve the lifted path equally: the focused optimization fixture
improved from 468 to 463 dynamic dispatches against classic's 422.

The strict memory fixture initially regressed from 1,409 to 1,597 dynamic
dispatches when its veneer routines began compiling directly; after the
edge-planning and threading work it stands at 1,412 against upstream's
1,420, with the unchecked mode at 811 against 839. On Life, direct mode
covers 12 of 15 routines (adding `Main`) and reaches 54,592,173 dynamic
dispatches (-2.82%), overtaking the lifted production path's -2.80% for
the first time. (The memory fixture's totals moved to 1,487 vs 1,519 and
886 vs 938 after the inline shift-boundary routines were added.)

Phase 4 closed with corpus and compilation-mode gates. `tests/
corpusTest.nix` compiles Cloak of Darkness with the full library through
direct IR and pins its coverage exactly (277 direct, 271 fallback; the
dominant bailout reasons are stack-argument routines, in-routine
directives, action statements, and top-level switch tables). Measuring the
corpus exposed a finding the transcript-only cloak test had hidden: on
cloak the LLVM paths are dynamically *worse* than upstream — lifted
179,729 and direct 175,651 against upstream's 164,995 (+8.9% and +6.5%).
Direct improves on the production lifted path but the select/branch
materialization costs documented under "Instruction-Count Findings"
dominate library dispatch code. The corpus test holds direct's ceiling;
closing the gap to upstream is follow-on lowering work, not a Phase 4
regression (direct is non-worse than the current production path).

Glulxercise under direct IR is pinned at 124 direct / 108 fallback with an
out-of-contract set of exactly one failure: the documented `jumpabs` case.
The lifted path's ten catch-token failures disappear because
`@catch`/`@throw` routines reject to classic generation — rejection is the
current documented representation for catch tokens and throw edges.

Compilation modes are now product policy with focused tests: debug-file
builds (`-k`) bypass the LLVM pipeline entirely and are byte-identical to
classic output (sequence points don't survive reordering); an
asterisk-traced routine compiles classically while its neighbors stay
direct and the trace transcript matches upstream; Infix does not exist for
Glulx (the compiler disables `-X` itself), so no direct-generation policy
is needed. The compliance suite now runs every behavioral story in
classic, lifted, and direct modes.

Per-routine `LLVM-BACKEND` records and direct-mode IR dumps require
`I6_LLVM_DIAGNOSTICS=1`; ordinary direct compiles emit only aggregate direct,
lifted, and fallback totals. Debug-file generation now uses allocated Unix
`realpath` output so long source paths do not trigger fortified-libc buffer
checks.

Phase 4.1 ranked all 277 direct Cloak routines by emitted-minus-input static
instructions before changing the lowerer. The largest expansions were
`GetGNAOfObject` (50 -> 132), `LanguageNumber` (114 -> 170), `ListEqual`
(136 -> 191), and `ObjectIsUntouchable` (162 -> 216). Their post-pass IR had,
respectively, 19/9/3/20 phis and 15/18/41/44 comparisons; only
`GetGNAOfObject` had many selects (7). Assembly comparison showed the dominant
shape was strict library guards becoming SSA condition values and phi edge
copies, not missing direct coverage. For example, classic
`GetGNAOfObject` updates its `case` and `gender` locals in place, while the
optimized SSA form carries their alternatives through select and phi chains.

Three generic lowerer changes survived the required cross-benchmark checks:

- A select whose sole consumer is a global or dereference store now writes
  each arm directly to that destination. `SetTime` guards the resulting 6 -> 7
  shape (formerly 6 -> 8).
- Multi-use comparisons of immutable operands are rematerialized at each
  branch/select consumer instead of paying a copy/branch/copy boolean
  materialization. Single-use comparisons also fuse through a no-op `freeze`.
- When exactly one side of a conditional needs ordinary phi copies, that side
  is emitted as the inline/goto edge and the copy-free side is the branch
  target. This removes the forced jump on the copy-free path and is pathwise
  non-worsening.

General fusion of returned selects with non-immediate arms was tested and
rejected: although it reduced static instructions, the focused lifted fixture
regressed from 463 to 474 dynamic dispatches. Keeping the immediate-only rule
preserves its branch layout.

The static ranking above turned out to be a poor guide to the dynamic gap:
those changes recovered only 208 of the 10,656 extra dispatches. Phase 4.1
then re-measured with dynamic attribution — a `VM_PROFILING` glulxe run of
the cloak walkthrough joined against `$!asm` routine addresses for
per-routine self-op deltas, plus `--opcode-histogram` diffs. That showed the
gap concentrated in a handful of hot routines with four generic causes, each
then fixed in the shared lowerer:

- `Unsigned__Compare` alone was +5,863 (2.9 ops/call upstream vs 6.1):
  if-conversion turns the branchy veneer comparison into a nest of selects
  feeding one return, which materialized each select through a slot. Fused
  return selects now chain: one arm may be another qualifying select (chains
  only, never both arms — a stack-riding condition operand of a second arm
  could pop out of LIFO order, while an untaken chain suffix is merely
  abandoned and discarded by the return). Conditions expand connective
  trees at the ret, so or-chains stay branch-form (`Opt_SwitchShared`
  improved 11 -> 9). `Unsigned__Compare` now lowers to three instructions
  and beats upstream by 140 dispatches on the walk.
- `RA__Pr`/`RL__Pr` were +3,263/+1,221: classic mutates the `obj`/`id`
  parameters in place, while the phi carrying their merged values paid two
  edge copies plus a jump on every call. `phi_param_slot()` now lets a phi
  adopt a parameter's slot whenever no read of the parameter can execute at
  or after any edge into the phi's block (a CFG reachability check),
  instead of demanding the phi be the parameter's only reader. Slot-sharing
  safety checks learned to see parameter reads, and one adopting phi per
  parameter slot is enforced.
- `NextWord`-style parser code was ~+1/call for `copy wn local` where
  classic reads the global operand directly at each use.
  `global_operand_pass()` now accepts cross-block uses via a forward
  "written since the load" dataflow (re-entering the load's block kills the
  taint), instead of requiring every use in the load's own block.
- The residual was almost entirely explicit `jump`s (+5,588 on the walk)
  from keeping LLVM's block order. `straighten_layout()` moves a block that
  nothing can fall into and that cannot fall out of its position to sit
  directly after a block that branches to it and currently falls into
  nothing. Each move enables one fallthrough and can lengthen no path; no
  branch-frequency guess is involved.

Cloak now measures 164,995 upstream, 162,002 direct (-1.81%), and 163,569
lifted (-0.86%): both LLVM paths beat classic codegen, and the direct
ceiling is pinned at 162,002. The focused optimization fixture improved to
444 dynamic dispatches (classic 422), the direct-IR fixture to 682
(upstream 756), and Life to 54,590,010 direct — still ahead of 54,605,034
lifted. Phase 4.1's exit gate is met on its primary arm; no accepted-gap
rationale is needed. Interference-aware coalescing beyond the parameter
case remains future work, but nothing measured still hangs on it.

The focused fixture currently requires 15 of 15 captured routines to lower,
with zero lift or lowering bailouts. It checks exactly 177 aggregate input
instructions, at most 184 emitted instructions, and exactly 422 classic dynamic
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
`src/llvm_codegen.c:601-615`. InstCombine can prove that a dividend is
nonnegative and rewrite `sdiv` as `udiv`. Glulx has a native signed division
instruction but no native unsigned division instruction.

The resulting `udiv` is expanded into shifts, signed division, multiplication,
subtraction, comparison, and correction in `emit_udiv_urem()` at
`src/llvm_lower.c:3066-3121`.
Patterns such as `(x & $7fffffff) / 3` can therefore replace one native
division dispatch with several dispatches. `Direct_NonnegativeDivide` now
guards this shape with a nine-instruction static ceiling; prevention of the
canonicalization or cost-aware recovery of signed division remains desirable.

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
`plan_terminator()` at `src/llvm_lower.c:3364-3466`. That edge is planned as
`EDGE_DIRECT` rather than `EDGE_FALLTHROUGH`, and `emit_terminator()` emits its
explicit jump before flushing the stub.

Conditional planning now avoids this cost when exactly one ordinary edge has
phi copies: the copy edge becomes the inline/goto side and the copy-free edge
the direct branch target. Cases where both edges require copies still need
stubs and remain candidates for interference-aware coalescing.

### Returned Selects Materialize Ordinary Values

`ret_select_pass()` accepts selects whose arms are immediate values or (as a
chain, one arm per level) further qualifying selects; `emit_ret_select()`
expands the chain as conditional returns, with connective-tree conditions
re-expanded at the ret. This restores the branch form of if-converted
boolean routines such as `Unsigned__Compare`.

Fusing arbitrary ordinary arms was tested in Phase 4.1, but it increased the
focused fixture from 463 to 474 dynamic dispatches despite reducing static
output, so arms beyond immediates and select chains still materialize.

### Select Ordering Assumes Immediates Are Rare

`select_swapped()` at `src/llvm_lower.c:1050-1055` assumes that an immediate
true arm is a rare sentinel and orders emission to favor the computed false
arm. The choice affects `emit_select()` at `src/llvm_lower.c:3004-3059`.

For the common idiom `ok ? 0 : error`, this can add one dispatch to the common
success path. Operand representation is not branch-frequency evidence. The
heuristic should use structural evidence, source ordering, or a neutral rule
which does not claim one arm is more likely.

### Select-to-Store Is Not Folded

`store_fold_pass()` now recognizes a select whose sole effective consumer is a
global or dereference store and emits each arm directly to that destination.
It uses the existing empty-window check and branches before either destination
write, so condition and arm reads cannot observe an early clobber. Cloak's
`SetTime` statically guards the fold.

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
capture buffer in `llvm_lower_routine()` at `src/llvm_lower.c:3744-3880`. The
current aggregate statistic
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
