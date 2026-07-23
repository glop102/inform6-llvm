# The Single-Writer Rule, Backend Mixing, and the Path to IR-Only

Notes from a design discussion at the close of the direct-IR migration
(2026-07-20). Companion to the "Why Classic Generation Stays" section in
`REVIEW.md`; this file goes deeper on the mechanics.

## Background: generation is entangled with parsing

The compiler's parser is streaming. There is no AST that gets walked
twice — as each statement parses, code generation happens immediately.
In Glulx mode today, *two* generators watch the same parse side by side:
classic generation emits `assembly_instruction` events (suppressed and
optionally captured by the seam in `src/asm.c`), and the direct backend
builds LLVM IR through the `llvm_direct_*` hooks.

This also means a routine cannot be re-parsed. If direct generation
discovers an unsupported construct at the *end* of a routine, the source
is already consumed — the only way to still emit the routine is to have
kept classic's shadow stream. That is the entire reason the shadow
capture exists.

## The parse-visible state

The assembler maintains globals that the **parser reads to make
decisions while still parsing the rest of the routine**:

- `sequence_point_follows` — one-shot flag: "the next instruction
  emitted gets a debug sequence point." Emitting an instruction
  consumes it.
- `execution_never_reaches_here` — reachability. The parser uses it to
  decide whether to emit the implicit `return` at routine end, whether
  to warn about unreachable statements, and whether whole statements can
  be skipped.
- `labeluse[]` — per-label count of forward branches.
  `assemble_forward_label_no()` only materializes a label if the count
  says someone branched to it, and `transfer_routine_g()` *decrements*
  counts as branch shortening deletes branches — so the counts must be
  exact, not merely nonzero.

## The single-writer rule

**Exactly one writer of that state per compile.** While shadow
generation runs, the classic path (via the capture stubs, which call the
backend-independent `asm_parser_*` helpers through
`shadow_note_instruction()`) is the writer. Direct hooks may read parser
state freely — they need reachability and label information to build a
correct CFG — but must never write it.

Why the rule exists:

1. **Double-application corrupts parsing far from the fault.** Both
   backends process every instruction. Two writers means every update
   lands twice: the sequence-point flag consumed twice, a forward branch
   counted twice in `labeluse[]`. The parser keeps reading these globals
   as it goes, so a double-count at line 10 changes a decision at line
   200 — a label materializes that shouldn't exist, an implicit return
   goes missing. The failure appears nowhere near the cause.
2. **It keeps the fallback sound.** The shadow stream must be exactly
   what a pure classic compile would produce, so that replaying it on
   rejection yields correct bytes. If direct hooks mutated parser state
   mid-routine, the captured shadow would be a stream no real classic
   compile generates.

Since the Phase 6 separation work, bookkeeping is fully factored out of
both encoding and event storage: the `asm_parser_*` helpers run for
every suppressed instruction whether or not its event is retained, and
`I6_LLVM_SHADOW=0` proves parsing does not depend on the stored stream
(byte-identical output on any story with zero fallbacks; pinned for
cloak and the memory fixture).

## Data-area side effects: ownership conflicts and the handoff pattern

The rule generalizes to a broader invariant: **every piece of shared
state has exactly one writer per compile.** For parser bookkeeping that
writer is classic generation, always. For data-area side effects the
ownership is per construct:

- **Multi-argument `random(a, b, c)`** — the choices live in a word
  array compiled into the story's data area, indexed by the emitted
  code. This used to reject as an ownership conflict (two generators
  would mean two arrays, or direct depending on classic's mutation).
  It is now handled by an aligned handoff: the direct build creates
  the array and queues its address
  (`llvm_direct_random_array_set/take`); classic generation of the
  same expression tree takes the address instead of building its own.
  Direct build strictly precedes classic generation per statement, and
  an aborted direct build stops before pushing, so consumption never
  misaligns — exactly one array exists whichever backend's stream is
  kept, and each array still has exactly one writer.
- **`box`** — the same shape (its text table is compiled by classic's
  statement handler during parsing) and still a reject
  (`unsupported statement`), because nothing in the corpus uses it.
  The random() handoff is the template if it ever matters.

These were never representation gaps — an array load is trivial IR.
Contrast `objectloop`, `spaces`, and `children()`, which needed
nontrivial generation but went direct without any ownership question:
their complexity lives in codegen-local state (LLVM blocks and phis the
direct backend owns), not in shared parser or data-area state. The
zero-fallback conversions (branch opcodes, two-store opcodes,
catch/throw, custom opcodes, sub-word copies) all follow that same
pattern — the new complexity is carried in name-encoded opaque-call
contracts between builder and lowerer, precisely so that no new shared
mutable state exists between the two generators.

## Mental model correction: how the backends mix (they don't)

**Granularity is the routine, winner-take-all.** For each routine,
either direct IR produces the *entire* body (lowered stream through the
classic encoder), or the routine rejects and the shadow stream is
replayed for the entire body. There are no classic islands inside an IR
routine. A fallback routine is a completely classic routine sitting next
to optimized ones in the code area. Since there is deliberately no
inlining or IPO, and every call is a full optimization barrier, nothing
is ever "moved around" a classic routine — the two kinds only interact
through calls, which neither backend reorders across.

**Inside a direct routine, the opaque-drop intuition is accurate — but
the things being dropped are `i6.*` opaque calls, not classic output.**
When the direct backend hits a Glulx operation LLVM cannot model
natively (`glk`, stream writes, `aload`/`astore`, searches, RNG), it
emits a typed opaque call (e.g. `i6.aload`) carrying a centralized
effect classification. LLVM may CSE, hoist, sink, or delete those only
as far as the declared effects permit — stream operations are full
barriers because filter I/O can call back into arbitrary VM code — and
the lowerer then emits the real Glulx instruction. That is the design
working as intended. Classic generation contributes zero bytes to a
direct routine's output; its stream is captured and discarded (or with
`I6_LLVM_SHADOW=0`, never stored).

## Is single-writer a blocker for forcing everything through IR?

No — it constrains the *transition state*, not the destination. The
rule is not "direct may never write"; it is "exactly one writer." Today
classic runs alongside for every routine, so classic is the default
writer. **The flip itself is built and proven** (2026-07-22): the direct
backend maintains its own parser-state model — reachability mirrored
from its build events (returns, jumps, noreturn opcodes close; label and
continuation binds open, the latter only when the block is classically
entered, so folded-away branches stay unreachable) and a
classically-entered block set answering the labeluse[] boolean. An
always-on cross-check compares the model against classic's writes at
every parser decision point (statement dispatch, forward-label
resolution, the routine-end fallthrough decision) and prints a
`parser-crosscheck` line on mismatch; the corpus was pinned clean
before the flip became the default. **The direct writer is now the
default** (2026-07-22, after `box` and raw code-byte arrays gained
representations): classic's bookkeeping (shadow_note_instruction and
asm_parser_note_label) is disabled while the direct build is active and
the model writes the globals — proven byte-identical on the whole
corpus before defaulting. The default forces shadow retention off
(event snapshots assume classic-timed writes), so a routine that
rejects direct IR is a compile error; `I6_LLVM_PARSER_WRITER=classic`
restores the classic writer, the shadow stream, graceful fallback, and
the cross-check. One measurement artifact: classic generation under the
flip reads direct-timed reachability mid-statement and emits fewer
instructions into its (discarded) stream, so the per-routine `input=`
diagnostic shrinks for condition-materialization shapes — output bytes
are unaffected.

The subtleties the cross-check flushed out, for the record: classic
folds constant conditions in `assembleg_1_branch` (a plain-constant
leaf becomes @jump or nothing — and front-end folding wraps constant
comparisons in "expression used as condition" nodes, so the model looks
through NONZERO_OP/ZERO_OP wrappers), and the if/else join label is
materialized only when branched (states.c now uses the same
forward-resolve idiom for it that loops always used).

The representation gaps that once made this list are all closed:
custom `@"..."` opcodes and sub-word copies emit verbatim with
per-operand forms, explicit stack opcodes escalate the routine to
real-stack mode mid-build (spill first, then the real stack provably
matches classic's), multi-arg `random()` uses the array handoff above,
and `@catch`'s setjmp shape is carried by the `i6.catchtok` /
`i6.catchflag` pair — one real `catch S ?L` whose store operand is the
token value's slot, so a throw rewrites exactly the value the IR reads
(catch tokens still expose frame layout, which optimization
legitimately changes; glulxercise's ten catch-token failures are the
story hardcoding classic frame depths, and the catch *values* all
pass). What remains excluded:

**By design**
- Custom two-store opcode shapes and the residual degenerate
  statements: the only rejects left (bare code blocks turned out to
  need nothing at all -- their statements parse through the normal
  dispatch, so removing the day-one reject was the whole fix). `box` now emits its
  veneer call directly (its text table was always built once by the
  statement handler, so there was never a real ownership conflict),
  and raw code-byte arrays ride the IR as verbatim blob anchors
  (`i6.codebytes.<n>`, full barrier, real-stack escalation) that the
  lowerer re-emits in place -- frame-offset assumptions inside the
  bytes stay out of contract, as with catch tokens.

**Design problems, not codegen problems**
- Debug-file builds: sequence points must survive instruction
  reordering — the same problem as debug info under `-O2` anywhere.

**Product decisions an IR-only world forces**
- `$LLVM=0` disappears as the miscompile-bisection escape hatch.
- No-LLVM builds (the stub) stop producing Glulx at all; LLVM becomes a
  declared hard requirement — including surviving LLVM's own breakage
  (LLVM 21's instcombine fixpoint verifier hard-aborting on library
  code was hit during this project).
- Un-represented constructs become compile errors until each gets an IR
  representation.

So "remove fallback" really means declaring a language subset and then
shrinking the excluded set construct by construct. That is a defensible
end state, but the sane route to it is per-construct representation work
(each justified by attribution evidence), not deleting the safety valve
first. Note also that classic *generation* is not classic *encoding*:
the encoder, branch shortening, and backpatching stay regardless,
because the lowerer emits through them — the deletable surface is
smaller than it appears.

## Would full IR coverage unlock optimization wins? (honest assessment)

Full corpus coverage has since been reached — every routine in cloak,
Advent, glulxercise, and the fixtures builds direct IR — and the
assessment below predicted the outcome correctly: coverage was a
correctness and completeness win, not a performance one (Advent moved
+0.02%).

**Fallbacks poison nothing beyond themselves.** The optimization horizon
is strictly intra-routine by design: one shared module, no inlining, no
IPO, and every call is a full optimization barrier for globals and RAM.
The barrier is not a fallback concession — Glulx semantics force it
regardless, because any callee can touch any global or RAM address,
whichever backend compiled it. A fallback routine therefore costs
exactly its own body's optimization and degrades no neighbor by a
single dispatch.

**Removing the shadow stream buys no code quality.** Proven, not
guessed: `I6_LLVM_SHADOW=0` is byte-identical on any zero-fallback
story. The shadow stream is write-only and never constrains the IR.
Removal buys ~5% compile time. The safety valve used to have a second
job — absorbing fixed-limit overflows — but the direct path's limits
are dynamic as of 2026-07-22: the symbolic stack, call arity, switch
nesting, unmerge networks, and the random-array queue all grow on
demand, and lowered frames are bounded only by Glulx's own two-byte
local-offset addressing (16384 slots, emitted as a multi-pair locals
format; `stories/direct-ir-limits.inf` pins all of this past every old
cap). What the valve still absorbs is LLVM-version shape drift on
arbitrary wild code and the genuinely unrepresented constructs; a much
broader fuzz corpus should precede dropping it.

**The real medium/long-horizon prize is selective inlining.** Cloak's
own profile makes the case: `Z__Region` is 17% of all self-ops at 2,776
calls of ~9.5 ops each — two of those ops per call are pure call/return
dispatch, plus argument setup on top. Inlining Z__Region-class helpers
into their callers could plausibly be worth several percent of a whole
game, an order of magnitude more than full construct coverage. Larger
games that employ their own small helper functions would benefit the
same way — helper-heavy code is exactly where per-call overhead
compounds. Crucially, **fallback does not block this project**: inlining
would be direct-into-direct only, and with classic-by-policy gone
(C0 routines now build direct IR in real-stack mode) every hot cloak
routine, `CA__Pr` included, is already direct. The one shared IR module
is the intended substrate; the work is devirtualizing
`i6.callf*(i6.sym)` sites into real IR calls and being careful with the
call-barrier effect model once a callee's body is visible to the
caller's optimizer. Notably, inlining also *dissolves* the barrier
problem for inlined callees — LLVM sees the callee's actual global
accesses instead of assuming a full clobber, which may unlock secondary
wins (GVN/LICM across what used to be an opaque call).

**Recommended order if pursuing wins:**

1. Selective inlining of small hot routines, ranked by the per-routine
   attribution the benchmarks already print by default (calls x
   per-call overhead is the sort key, not routine size).
2. Recovering the plain-translator dispatch gap at the IR level.
   Construct coverage is done and bought no dispatches — the wins are
   in optimization, not representation.
3. Shadow removal not at all: coverage is empirically total on the
   corpus and retention still costs only ~5% compile time while
   absorbing limit overflows and LLVM shape drift on wild code.

## Current measured context (for calibration)

Re-measured 2026-07-21, after the plain-translator lowerer rewrite and
the removal of classic-by-policy (C0 and asterisk-traced routines now
build direct IR). The dispatch regressions versus upstream are the
plain translator's known quality gap, to be recovered at the IR level.

- cloak: 548 direct / 0 fallbacks; walk 256,197 dispatches vs upstream
  164,995 (+55.3%).
- Life: 69,752,750 vs 56,177,197 (+24.2%); all 15 routines direct.
- glulxercise: 153 direct / 79 fallback — the fallback population that
  makes the safety valve load-bearing.
- Shadow retention cost: ~5% of a half-second cloak compile (547ms vs
  510ms interleaved), zero output bytes.
