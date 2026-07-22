# LLVM Pipeline Review

This is the working document for the LLVM backend: pinned measurements,
settled policies, open defects, and remaining work. The direct-IR
migration history (Phases 0-6) lives in git history and the test suite's
pinned gates; it is no longer narrated here.

## Cost Model

Dynamically dispatched Glulx instruction count is the primary performance
metric: every VM instruction pays a fetch/decode/dispatch cycle, and count
has been the best predictor of interpreter performance observed so far.
It is not a perfectly weighted runtime estimate — opcode handlers, operand
modes, memory behavior, Glk calls, and accelerated routines have different
costs — so count dominates optimization decisions without being treated as
exact. Static emitted counts are less predictive (cold blocks and hot
loops weigh equally) and are used only for focused microfixtures and
explaining where a transformation added bytecode.

The `glulxe-counted` interpreter increments a 64-bit counter after decode,
immediately before handler dispatch. `--opcode-histogram` breaks the total
down by opcode. Per-routine dynamic attribution (a `VM_PROFILING` glulxe
run joined against `$!asm` routine addresses — `tests/attrib.py`) runs by
default in the benchmarks; **rank optimization targets by measured dynamic
attribution, never by static emitted-minus-input** (a static ranking
misdirected an entire Phase 4.1 attempt).

The desired metric hierarchy, top two in place:

- Dynamic total: deterministic primary regression signal.
- Opcode histogram: identifies what kind of interpreted work changed.
- Weighted dynamic cost (open): measure per-opcode / per-operand-mode
  handler costs with controlled loops, fit against whole-program timings.
- Repeated wall-clock timing: secondary validation (medians over
  alternating runs; individual samples are noisy).

## Current Measurements and Pinned Baselines

Direct IR is the sole production Glulx path (`$LLVM=4` default; every
nonzero value selects it; `$LLVM=0` is the classic escape hatch). Current
corpus points, all enforced by pinned gates in `tests/`:

```text
cloak walk:  upstream 164,995   direct 153,588  (-6.9%)
             coverage: 416 direct / 132 policy-classic C0 / 0 fallbacks
Life:        upstream 56,177,197  direct 54,589,516  (-2.83%)
glulxercise: 135 direct / 79 fallback (correctness gate; pinned
             out-of-contract set: one jumpabs check, ten catch-token checks)
Advent:      -4.88% (second real-game benchmark, --rngseed 1)
focused optimization fixture: classic 422, direct ceiling 444
```

`flake.lock` pins Inform 6 `d1066bc2…`, Glulxe `56ab8743…`, CheapGlk
`14d8aaf6…`. After a lock update resolve sources with
`nix eval --raw .#glulxe.src` (etc.); don't record built store paths here.
The counting patch is `patches/glulxe-instruction-count.patch`.

Two standing measurement caveats:

- The aggregate instruction counters include only successfully lowered
  routines, so a routine falling back *removes* its counts and can make
  aggregates look better while coverage regresses. Count assertions must
  always be paired with backend coverage assertions (the suite does this;
  keep it true for new tests).
- Fixture transcripts must never print raw object/routine/dictionary
  addresses: RAM layout shifts across 256-byte alignment whenever code
  size changes. Compare against known objects instead.
- Capture performance baselines only from a clean build (`make clean` or a
  fresh worktree of the baseline commit); a stale incremental binary
  produced phantom regressions once already.

## Settled Policies

Recorded so they are not relitigated without new evidence.

- **Computed code addresses (`jumpabs` across routines) are unsupported
  under optimization**, as they are unsupported across upstream compiler
  versions. The Glulx spec permits interior jumps, but Inform never
  guaranteed the generated layout needed to derive an interior address
  from a routine symbol (glulxercise's `test_jumpabs_2+5` relies on it).
  `jumpabs` lowers routine-locally as an opaque non-returning operation
  and the compiler warns when optimization encounters it. No story-wide
  preflight or layout-preserving mode is planned.
  `tests/complianceTest.nix` pins the accepted glulxercise failure set
  exactly.
- **C0 stack-argument routines stay classic by measured policy.** The
  entire 132-routine cloak set is 8.35% of self-ops, 96% of it `CA__Pr`,
  and the measured direct-IR gain on its inline-assembly-heavy veneer
  siblings (0-8.4%) projects the recoverable win under half a percent —
  which does not pay for modeling varargs entry state. Enforced as a
  closed set: every `backend=classic` diagnostic record must carry
  `reason=stack-argument routine`. Revisiting requires new attribution
  evidence, not a coverage argument.
- **Shadow retention is permanent architecture, not migration residue.**
  Fallback is load-bearing (glulxercise: 79/214 routines) for constructs
  direct generation deliberately rejects (catch/throw, explicit stack
  manipulation, custom opcodes, multi-arg `random()`); the cost is ~5% of
  a half-second compile and zero output bytes. Parse-visible bookkeeping
  (`shadow_note_instruction`) is separated from storage;
  `I6_LLVM_SHADOW=0` disables retention to prove parsing never depends on
  the stored stream (byte-identical cloak, clean error — never a silently
  empty routine — where a fallback would be needed).
- **Classic generation stays.** It backs the fallback valve, the C0
  policy, debug/trace/INFIX builds (sequence points and trace preambles
  do not survive reordering), `$LLVM=0` bisection, and resilience against
  LLVM itself (LLVM 21's instcombine fixpoint verifier has hard-aborted
  on library code). The single-writer rule — direct hooks read parser
  state, classic generation is the sole writer — hides only wins that are
  dynamically ~0 on everything measured (multi-arg `random()`, `box`).
  If a specific win is wanted later, relax the rule per construct with
  attribution evidence first.
- **Debug-file (`-k`) builds bypass the LLVM pipeline entirely** and are
  byte-identical to classic output; asterisk-traced routines compile
  classically among direct neighbors; Glulx has no INFIX.

Diagnostics: `I6_LLVM_DIAGNOSTICS=1` emits per-routine `LLVM-BACKEND` TSV
records (backend, stage, counts, bailout reason) plus IR dumps to
`inform6-llvm-dump.ll`; ordinary compiles print only the aggregate
`LLVM: backends` / `LLVM: direct fallbacks` counter lines, which the
corpus tests pin.

## Branch `inlining-deferred-lowering`: Remaining Work

The branch defers all routine lowering/addressing to one end-of-pass
pass and, on top of that, selectively inlines small direct callees
behind `I6_LLVM_INLINE`, gated greedily per (callee, depth) site group
against a loop-weighted cost estimate. Status reference (not pinned;
inlining is opt-in): inline-off is byte-identical to master everywhere;
inline-on measures cloak −4.36% (146,886) at +10.4% story size, Life −6
dispatches, transcripts identical, no backend changes; compile time
inline-on is ~1.1 s on glulxercise and ~12 s on cloak. Everything below
is open work.

### Lesson for further emission-order work

The audit's blocking miscompiles shared one systematic root cause: the
deferral commit enumerated the *table* consumers of parse-time addresses
(action table, veneer table, GV2 grammar tokens) but missed the *scalar*
consumers (`Replace`'s value copy, the header's implicit "Main is first"
invariant) and treated a mutable predicate as a constant — all in gaps
the corpus never exercises, which is why "byte-identical across the
corpus" held anyway. Any further emission-order work should hunt for the
same class: reads of `symbols[].value` or pc-derived values between
parse and end-of-pass.

### Rethink the inlining cost model

The current gate's estimator (instructions weighted 8^depth, depth
capped at 3, inferred from backward branches) has hit its precision
limit, and the limits were bought with measured regressions — respect
them until the model itself is replaced:

- **The per-level iteration guess cannot price a loop.** It accepted an
  inlined `PrintGrid` (2,048 real cells per call against a guessed 64)
  as a win that measured +1,987 dispatches on Life; that is why only
  loop-free callee bodies are eligible today, which forfeits the
  loopy-callee wins the ungated form showed (~1.5pp of cloak's −6.02%).
  Recovering them needs real cost evidence — the measured per-opcode /
  weighted dynamic cost model from the metric hierarchy, and iteration
  evidence (bounded-loop analysis of constant trip counts, or measured
  attribution) — not a bigger guess.
- **Any charge term must apply the exact weighting the inlined form's
  own scoring uses** (same bins, same cap). Two asymmetries were caught
  manufacturing phantom wins: flat weight×cost products exceeding the
  cap, and IR-layout depth disagreeing with stream-measured depth. A
  rethought model must keep this both-sides-identical property by
  construction.
- **Estimation noise around zero is real regression risk** — near-tie
  accepts measurably regressed; the noise margin (baseline/128) is a
  patch, not a model. A rethought model should quantify its own
  uncertainty per decision instead.
- **Trial cost is the binding constraint on model richness**: every
  candidate group costs a full caller lowering (cloak ~12 s inline-on).
  Prune groups whose maximum possible win is under the margin, make
  trial evaluation incremental, or move the model to something cheap
  enough to score without lowering.

### Design gaps in the inlining prototype

- **The same callee is cloned + linked once per call site**
  (`i6.inl.<caller>.<i>`), not once per caller; compile time with
  inlining on is ~4× (glulxercise 0.13 s → 0.55 s). Cache one clone per
  (caller, callee).
- Minor: the >16-param eligibility check runs after the clone is already
  linked (harmless — the unused internal function is deleted — but the
  check should precede linking); the 128-call-site collection cap is
  silent; the eligibility doc's "inline stack by symindex" recursion
  guard is not implemented (moot today only because inlining is strictly
  single-level — revisit if transitive inlining is added).

### Secondary defects (open)

- Errors raised inside `emit_deferred_routines` are attributed to the
  end-of-file lexer position, not the routine's source line (diagnostic
  modes only).
- `llvm_replay_routine`'s save/restore of `labels[].symbol` preserves
  stale cross-routine leftovers during batched replay; nothing reads it
  after replay today except an internal-error path that would print a
  wrong label name. Undocumented cross-routine state dependency.
- `expressp.c`'s SYSTEM_SFLAG marker-clearing now bakes a symbol index
  (previously a raw address — equally garbage); unreachable today
  (`Main__`/`Symb__Tab` are never referenced by name through expressp)
  but deserves an assert.
- Glulxercise under inlining: the catch-token diffs are the documented
  layout-sensitive checks (value-only; they already fail against
  upstream expectations on master's direct path). The jumpabs check does
  not merely fail — execution escapes the test without returning
  (layout UB per the settled policy; compile-time warning already
  covers it). Acceptable while inlining is opt-in; keep the pinned
  failure set honest if it ever defaults on.

### Correctness invariants for the remaining branch work

- Single-writer/shadow rule untouched: deferral moves consumption of the
  shadow stream, never its production; every routine's shadow is retained
  until that routine lowers so a late fallback stays byte-exact.
- Byte-identical output with inlining off is the standing gate for all
  deferral fixes.
- Effect attributes must survive cloning/linking (currently guaranteed
  conservatively); default-return convention per `embedded_flag`;
  address-assignment order must match emission order; an inlined body
  must not create dependence on a routine's generated address.
- The inline source is the callee's pristine pre-optimization module
  (settled): retained modules are never mutated, so what a caller
  inlines is independent of emission order. Future work must not
  reintroduce in-place mutation of a retained module.
- Module lifetime (settled): with inlining off, routines lower at stash
  time and their modules are disposed immediately (only header emission
  and address assignment stay deferred); with inlining on, modules
  persist to end of compile as inline snapshots. Future work touching
  either path keeps the other's lifetime intact.
- `I6_LLVM_SHADOW=0` now means "do not retain shadows" — any routine
  needing a fallback becomes a compile error, evaluated at end of pass.

### Remaining inlining work

- Measure Advent inline-on (second real game; still unmeasured), and
  decide default-on only on cross-corpus evidence: dispatch wins with no
  regressions, bounded size growth, and acceptable compile time —
  compile time currently fails that bar (cloak ~12 s).
- Tune `INLINE_MAX_BLOCKS`/`INLINE_MAX_INSTS` (currently 16/70 on
  pre-optimization IR — a crude proxy) once the cost model is settled.
- Phase 3 landing: inline `Z__Region` and hot siblings by default, pin
  the new cloak baseline in `tests/corpusTest.nix`.
- Focused fixtures: a hot-loop helper that must inline (assert the
  call/return dispatch pair disappears), a forward-defined helper
  (proving deferral bought the forward case), a recursive callee that
  must not inline, an indirect call that must not, a varargs callee
  excluded, and an ordering fixture whose opaque stream op's barrier
  must survive inlining.
- Track the deferred-model costs as first-class benchmark outputs (peak
  compiler RSS, compile time) on the largest corpus game.
- Open design question: transitive inlining (flatten at snapshot vs
  caller time, depth budget vs recursion guard).

### Recommended fix order for the branch

1. Clear the open secondary defects above (all minor).
2. Rethink the cost model (section above); bring compile time down.
3. Measure cross-corpus (including Advent); then decide default-on and
   pin baselines.

## Open Correctness Work

- **Poison/undef/freeze needs proof.** The lowerer resolves `undef`/
  `poison` as zero and treats `freeze` as a no-op — safe only if the
  builder and pass pipeline never let LLVM UB/poison represent behavior
  Glulx defines. Division and shifts have explicit guards; nothing else
  does. Exercise shift boundaries, signed-overflow-sensitive transforms,
  narrowed values, and selects whose unchosen arm could become poison;
  if the invariant can't be proven for arbitrary optimized IR, reject
  poison-dependent values instead of silently choosing zero.
- **Opcode effect classification is correctness-critical.** The
  pure/readonly/inaccessible-memory lists in `src/llvm_codegen.c` drive
  GVN, LICM, DCE, sinking, and global-clobber analysis; a mistaken entry
  reorders across a VM callback or observable state change. Needs focused
  alias, callback, fault, and RNG-order tests rather than broad
  transcript coverage.

## Open Instruction-Count Work

- **Unsigned-division canonicalization**: InstCombine can rewrite `sdiv`
  as `udiv` for provably nonnegative dividends; Glulx has no native
  unsigned divide, so `emit_udiv_urem` expands it into several
  dispatches. `Direct_NonnegativeDivide` pins the shape at a
  nine-instruction ceiling; preventing the canonicalization or
  cost-aware recovery of signed division remains desirable.
- **LICM on zero-trip/conditional paths**: pure opaque ops are
  `speculatable`, so an invariant op can be hoisted from a loop that
  would have executed it zero times. Fixtures for profitable and
  unprofitable shapes remain open.
- **Select ordering heuristic**: `select_swapped()` assumes an immediate
  true arm is a rare sentinel; for `ok ? 0 : error` that taxes the
  common path. Operand representation is not branch-frequency evidence —
  replace with structural/source-order evidence or a neutral rule.
- **Both-edges phi copies still stub**: the one-copy-edge case is solved
  (copy edge inlines, copy-free edge branches), but conditionals where
  both edges need copies still pay stubs; interference-aware coalescing
  beyond the phi-to-parameter case is the known follow-on. (Ordinary-arm
  return-select fusion was measured and rejected: static win, dynamic
  loss.)
- **Fixed limits can hide coverage regressions**: symbolic stack, pending
  fusions, connective-tree depth, edge stubs, reads, local slots all
  fall back safely, but a limit hit surfaces as an unexplained total
  change. Add fixtures near each practical limit asserting either
  successful lowering or the exact bailout reason.

## Profitability Model

Two profitability decisions still need a real cost model (the shared
requirements — measured per-opcode costs, iteration evidence, symmetric
weighting, quantified uncertainty — are in the inlining cost-model
rethink section above):

- **Routine replacement is unconditional**: a direct lowering that
  estimates worse than the shadow stream still replaces it. Make
  replacement conditional on the model, with the retained shadow as the
  ready-made fallback.
- **Inline acceptance** should move from the current guessed-weight
  estimator to the same model.

Until then, focused instruction-count tests are the protection that
keeps LLVM upgrades and lowerer refactors from silently spending
IR-level gains on VM dispatches.

## Remaining Test Matrix

Focused fixtures worth adding, beyond what
`stories/optimization-regressions.inf` already covers (store fusion,
comparison/select returns, boolean trees, loop phis, recurrence folding,
four-block layout, switch ordering, global coalescing + clobber decline):

- Representation/fusion: phi and slot coalescing (induction values, phi
  chains, parallel-copy cycles needing scratch); stack fusion in valid
  LIFO order, invalid pop order, around stack-argument calls, at the
  pending-value limit; slot reuse liveness positive and negative cases;
  loop edge-folding with and without a rematerialization use.
- Control flow: return shapes (lone blocks, phis, multi-predecessor);
  nested short-circuit with side effects proving unexecuted arms;
  branch-tree movement across pure ops and rejection across writes;
  cross-block single-use comparisons (record current excess, guard a
  future implementation).
- VM/LLVM semantics: division/modulo fault cases and constant ranges;
  shifts 0/31/32/negative and rotate idioms (`llvm.fshl`/`fshr` with
  counts 0, 1, 31, mask-required values); unsigned boundaries around
  `0x7fffffff`/`0x80000000`; repeated and conditional dereferences;
  global/RAM aliasing around stores and opaque calls; filter-I/O
  callbacks mutating globals; faulting reads on untaken paths neither
  hoisted nor deleted; RNG ordering with intervening RAM ops and repeated
  `setrandom`; nested pointer selects beyond current repair depth; tail
  calls with stack arguments (pop order, empty symbolic stack).
- Harness: run the real LLVM build and strict Nix suite in Linux CI;
  keep the Z-machine baseline protecting the shared assembler; an
  LLVM-version smoke test that expected post-pass shapes still lower.

## Maintenance Notes

- Machine-readable benchmark output (JSON/TSV: compiler revision,
  LLVM/interpreter versions, coverage counts, histogram, dynamic total,
  story size, timings) remains open; it would make cross-commit and
  cross-LLVM-upgrade comparisons reproducible.
- Compile-time `lookup()` scans remain quadratic in routine size; measure
  if larger corpora expose compile-time problems.
- Code-size measurement across the corpus remains open (newly relevant:
  inlining trades size for dispatches).
- Pre-existing upstream bug (present on master and this fork): a long
  *output-file* path aborts the compiler with a fortified-libc buffer
  overflow (the debug-file path was already fixed to use allocated
  `realpath`; the output-filename path was not).
- Glulxercise's `random` self-check flakes at ~1 in 59 runs (1.70%) by
  design of the test, not the RNG: its `lobit`/`hibit` `[100..140]`
  window is only ±2.6σ for a Binomial(240, ½) counter. On glulxe builds
  that clock-seed (`@setrandom 0` → `time(NULL)`), failure is a pure
  function of the epoch second, so specific seconds reproduce it
  deterministically. Full analysis and an exact simulator:
  `glulxercise-random-flake-report.md` / `glulxercise-random-sim.c`.
  Re-run before suspecting a compiler regression; CI that runs
  glulxercise should tolerate or pin-seed this test.

## Recommended Order of Work

1. Finish the branch (order above): clear the minor open defects; land
   deferred lowering byte-identical.
2. Measure per-opcode/operand-mode costs; validate a weighted dynamic
   cost model against repeated timings.
3. Rethink the inlining cost model on that evidence; bring inline-on
   compile time down; then land `Z__Region`-class inlining by default on
   cross-corpus measurements (including Advent) and pin new baselines.
4. Put the real LLVM build and strict optimization suite in Linux CI.
5. Make routine replacement (as well as inline acceptance) conditional
   on the cost model.
6. Work the open correctness items (poison proof, effect-classification
   tests) and the remaining test matrix opportunistically alongside.
