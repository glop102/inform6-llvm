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

## Branch `inlining-deferred-lowering`: Review Findings

The branch (commits d63c4f0..9cf10c1) implements deferred lowering —
no routine is lowered/encoded/addressed during parse; all are retained
and emitted in one end-of-pass pass after `parse_program` +
`compile_veneer` — and, on top of it, selective procedure inlining behind
`I6_LLVM_INLINE` (clone the callee module, link into the caller, rewrite
the opaque `i6.callf*` as a real call marked alwaysinline, run the
always-inline pass). A full skeptical audit (2026-07-21, all findings
independently reproduced) follows. **The four blocking defects it found
are now fixed** (see that section); the inlining design gaps and
secondary defects remain open.

### Verified claims (adversarially checked, hold)

- Byte-identical output with inlining off, master vs branch, across the
  full story corpus in both `$LLVM=0` and `$LLVM=4`, from a clean
  worktree baseline. All nine test gates pass.
- Cloak with inlining on: 153,588 → 143,916 dispatches (−6.30%),
  transcript identical. Microbenchmark (helper called 1,000× in a loop):
  −28.5%.
- The revert-on-fallback guard is implemented and works: inlining runs on
  a throwaway clone and the un-inlined module lowers instead if the
  inlined form fails to optimize or lower. Measured essential: without
  it, `Parser__parse` bloats past the local-slot limit, falls back to
  classic, and erases most of the program-wide gain (−1.39% vs −6.30%).
  Under the guard no routine changes backend.
- The SYMBOL_MV address-decoupling argument is sound end-to-end: marked
  operands are forced to 4-byte `CONSTANT_OT` (width invariant), and
  `bpatch.c`'s ROUTINE_T `SYMBOL_MV` case applies the identical
  stripped-address + `code_offset` computation as `IROUTINE_MV`. Veneer
  overrides, GV2-only grammar, and Z-code isolation all check out.
- The deferred event-stream deep copy is genuine (POD operands, `ai.text`
  nulled at capture, memlist-registered buffer); the failed-inline retry
  cannot leave partial output (every lowering failure exit precedes
  emission and the buffer resets at emission start); effect-attribute
  loss across module linking is impossible in the unsafe direction
  (missing attributes default to full-barrier); no double-frees in the
  retained-module lifetime.

### Blocking defects (all reproduced; ALL FIXED 2026-07-21)

Resolutions, in order: (1+3) the deferral decision is latched once in
`asm_begin_pass` (`deferred_lowering_wanted()` no longer consults
`trace_fns_setting` at all), and asterisk-traced / `-g` routines are now
captured and deferred as classic-policy routines — the capture block in
`assemble_routine_header` moved above the trace preamble so the preamble
is ordinary captured code; nothing emits eagerly under an active gate, so
`Main__` is always first and a mid-source `Switches g` compiles
correctly (a directive change that *would* alter the latched decision is
a `fatalerror` via `deferred_lowering_latch_conflict()`, as a backstop —
upstream already blocks `-k` in `Switches`). (2) `defer_replace_original`
attaches Y to the just-stashed first definition of X (and gives Y its
`ROUTINE_T` placeholder identity immediately, since the `Replace`
directive had defined it as a zero constant); `emit_deferred_routines`
assigns Y alongside the routine's own symbol. (4) the two stubs were
added to `llvm_stub.c`. All fixed outputs are byte-identical to master's
eager output (asterisk, `Replace`, `-g` cases verified byte-for-byte),
the full corpus sweep and all nine gates stay green, and a combined
regression fixture (`direct-ir-deferred-timing` in
`tests/directIrTest.nix`: `Switches g` + `[ Traced * ]` + two-symbol
`Replace`, compared behaviorally against upstream) now guards all three
miscompiles. The original findings, kept for the record:

1. **An asterisked routine hijacks program startup.** `[ Foo * ...; ]`
   routines emit eagerly (uncapturable), and under deferral nothing else
   has been emitted during parse — so the eager routine lands at
   code-area offset 0, where the Glulx header's start-function field is
   hardcoded to point (`src/files.c:723` bakes the code-area start, not
   the `Main__` symbol). The story boots into the traced routine and
   never runs Main. Also falsifies the commit's "reproduces the eager
   layout exactly" claim. Fix: defer-or-refuse asterisked routines, or
   emit `Main__` eagerly first, or resolve the header start function
   through the symbol.
2. **`Replace X Y` resolves the preserved original to `Main__`.**
   `src/syntax.c:222` copies X's value into Y at parse time, when every
   deferred routine's value is the placeholder 0; Y is never a stashed
   `routine_symbol`, so it backpatches to `0 + code_offset` = Main.
   Calling the preserved original recurses into Main (reproduced to a
   stack fault). This is the standard library-customization idiom. Fix:
   re-resolve `rep_symbol` at end of pass.
3. **The deferral gate is a live predicate, not a latched decision.**
   `deferred_lowering_active()` re-reads `trace_fns_setting` at every
   call site; a mid-source `Switches g;` flips it after `Main__` is
   stashed — later routines emit eagerly from pc 0 and the stash
   (including Main) is silently never emitted. Fix: latch the decision
   once at `begin_pass` and make `Switches` error if it would change a
   gate input after the first stash.
4. **`make WITH_LLVM=0` no longer links** — `llvm_stub.c` is missing
   stubs for `llvm_retain_direct_routine` / `llvm_lower_retained_routine`
   (unconditional call sites in `asm.c`).

The root cause of 1-3 is systematic: the commit enumerated the *table*
consumers of parse-time addresses (action table, veneer table, GV2
grammar tokens — all correctly refreshed) but missed the *scalar*
consumers (`Replace`'s value copy, the header's implicit "Main is first"
invariant) and treated a mutable predicate as a constant. All three sit
in gaps the corpus never exercises, which is why "byte-identical across
the corpus" held anyway. Any further emission-order work should hunt for
the same class: reads of `symbols[].value` or pc-derived values between
parse and end-of-pass.

### Design gaps in the inlining prototype

- **The profitability gate is not implemented — only the lower-failure
  revert exists.** The inlined form is used unconditionally whenever it
  lowers (`process_direct_routine`); nothing compares it against the
  un-inlined lowering, and there is no heat ranking. Measured
  consequences: on cloak 264 routines get strictly larger static output
  (+8,718 instructions; 11 shrink), stories grow +32-42%
  (cloak 152,576 → 201,216 bytes), and **Life regresses** by +1,634
  dispatches because cold sites inline as eagerly as hot ones. The
  needed gate, unchanged from the original design: rank candidate sites
  by measured `calls × per-call overhead` from dynamic attribution,
  estimate post-inline cost from CFG/loop structure, and keep the
  pre-inline form when the estimate regresses. Default-on is
  unjustifiable before this exists.
- **Retained callee modules are optimized in place by their own
  lowering**, so what a caller inlines depends on emission order (a
  caller emitted before its callee clones pre-optimization IR; after,
  post-optimization IR that has also been through
  `unmerge_pointer_selects`). Not unsound — both forms are verified
  equivalents — but order-dependent output under a flag violates the
  design's own snapshot invariant and will confound tuning. Snapshot or
  clone before the callee's own lowering mutates it.
- **All retained modules persist to end of compile even with inlining
  off**: +21% peak compiler RSS on glulxercise (49.9 → 60.6 MB). Dispose
  each module after its routine lowers when inlining is disabled (or
  after its last inline use when enabled). The
  "should have been consumed" comment in `llvm_codegen_free` is stale.
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

### Secondary defects

- **AUTOGEN embedded-routine symbols are name-keyed with no collision
  guard.** The mangled `<object>.<prop>` names include
  `nameless_obj__N`, a legal user identifier; `assign_symbol` silently
  overwrites on name match, so a collision aliases two embedded routines
  to one address (latent silent wrong-code). Detect a non-UNKNOWN
  existing symbol at the mint sites (`objects.c`) and uniquify.
- **The promised AUTOGEN post-backpatch cleanup pass was never written**;
  the comment at `objects.c:1582` claims it exists. Either implement the
  discard (DISCARDED/UNHASHED machinery) or fix the comment. Until then
  AUTOGEN symbols persist in the table (and keep the collision surface).
- **`find_the_actions()` runs twice under deferral**
  (`inform.c:1040`, `:1053` — the first for USED_SFLAG before unused
  warnings, the second to refresh addresses); its error paths are not
  idempotent, so a missing `...Sub` reports twice and doubles the error
  count. Make the second pass silent.
- The deferred branch in `properties_segment_z` (`objects.c`) is
  unreachable (deferral requires Glulx) and sets Glulx `CONSTANT_OT`
  inside the Z path — wrong if ever enabled. Copy-paste residue; delete
  or fix.
- `verbs.c` registers ATTRIBUTE_T grammar tokens in the routine-address
  fixup list; currently a harmless no-op rewrite, but semantically wrong
  and a trap if either mechanism is extended. Limit to the routine token
  cases.
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
- `I6_LLVM_SHADOW=0` now means "do not retain shadows" — any routine
  needing a fallback becomes a compile error, evaluated at end of pass.

### Remaining inlining work (beyond the fixes above)

- Tune `INLINE_MAX_BLOCKS`/`INLINE_MAX_INSTS` (currently 16/70 on
  pre-optimization IR — a crude proxy) *after* the profitability gate
  exists; decide default-on only on cross-corpus evidence (cloak −6.30%,
  Life must not regress, size growth bounded).
- Phase 3 landing: inline `Z__Region` and hot siblings by default, pin
  the new cloak baseline in `tests/corpusTest.nix`.
- Focused fixtures: a hot-loop helper that must inline (assert the
  call/return dispatch pair disappears), a forward-defined helper
  (proving deferral bought the forward case), a recursive callee that
  must not inline, an indirect call that must not, a varargs callee
  excluded, and an ordering fixture whose opaque stream op's barrier
  must survive inlining.
- Diagnostics: per-routine inlined-callee counts in
  `I6_LLVM_DIAGNOSTICS=1`; `I6_LLVM_INLINE=0` already works for
  bisection.
- Track the deferred-model costs as first-class benchmark outputs: peak
  compiler RSS and compile time (current data: glulxercise RSS
  49.9 → 60.6 MB inline-off, ×4 compile time inline-on) on the largest
  corpus game.
- Open design questions: snapshot point for the inline source (decide
  pre- vs post-optimize deliberately — currently accidental, see design
  gaps); transitive inlining (flatten at snapshot vs caller time, depth
  budget vs recursion guard).

### Recommended fix order for the branch

1. ~~Blocking defects 1-4~~ — done, with the `direct-ir-deferred-timing`
   regression fixture (see above).
2. Snapshot callee modules before their own lowering; dispose retained
   modules when inlining is off (fixes the RSS regression together).
3. Fix the secondary defects (AUTOGEN collision guard + comment, silent
   second `find_the_actions`, dead Z branch, attribute fixup).
4. Then, and only then, build the profitability gate and re-tune.

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

Successfully lowered routines replace the shadow stream unconditionally,
and (on the branch) successfully lowered *inlined* forms replace the
un-inlined form unconditionally. A blanket static-count rule would be
wrong — cold static growth can buy hot dynamic wins — so the needed model
estimates cost from CFG loop structure, pathwise-safe rules, and measured
target instruction costs (no PGO). The retained shadow (and the retained
un-inlined module) are the ready-made fallbacks when an estimate
regresses. Until the model exists, focused instruction-count tests are
the protection that keeps LLVM upgrades and lowerer refactors from
silently spending IR-level gains on VM dispatches.

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

1. Fix the branch's blocking defects and secondary defects (order above)
   with regression fixtures; land deferred lowering byte-identical.
2. Build the inlining profitability gate; only then tune caps, land
   `Z__Region`-class inlining, and pin new baselines.
3. Put the real LLVM build and strict optimization suite in Linux CI.
4. Measure per-opcode/operand-mode costs; validate a weighted dynamic
   cost model against repeated timings.
5. Develop the generic CFG profitability estimate and make both routine
   replacement and inline acceptance conditional on it.
6. Work the open correctness items (poison proof, effect-classification
   tests) and the remaining test matrix opportunistically alongside.
