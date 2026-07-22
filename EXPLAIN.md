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

## Why the rule causes some rejects ("bails")

The rule generalizes to a broader invariant: **classic generation owns
every parse-time side effect; direct must neither duplicate one nor
reach into one.** A few constructs violate that structurally:

- **Multi-argument `random(a, b, c)`** — classic's statement handler,
  as a side effect of generating code, compiles a word array of the
  choices into the story's data area; the emitted code indexes it. If
  direct generated this itself it would either create a *second* array
  (duplicated side effect, shifted addresses) or have to consume the
  array classic just created (direct depending on classic's mutation of
  shared compiler state). It rejects instead
  (`unsupported random arity`).
- **`box`** — same shape; its text table is compiled by classic's
  handler during parsing.

These rejects are *not* representation gaps — an array load is trivial
IR. They are ownership conflicts, and the pragmatic choice was to
reject. Contrast `objectloop`, `spaces`, and `children()`, which needed
nontrivial generation but went direct: their complexity lives in
codegen-local state (LLVM blocks and phis the direct backend owns), not
in shared parser or data-area state.

The fix, if ever worth it, is to relax ownership per construct: hoist
the word-array creation out of classic's statement handler into a shared
helper that runs once regardless of backend, with both backends
referencing the result. Measured payoff today is ~zero: cloak compiles
with zero fallbacks, so real library games never hit these constructs —
they live in glulxercise and test fixtures.

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
classic runs alongside for every routine, so classic must be the writer.
If classic Glulx generation stopped running, the direct backend would
simply *become* the single writer, calling the same `asm_parser_*`
helpers the capture stubs call now. The Phase 6 separation built exactly
that seam; flipping the writer is mechanical once classic stops running.

The real blockers to IR-only are the constructs with no direct
representation, graded roughly:

**Tractable engineering**
- Custom `@"..."` opcodes: representable as opaque calls with
  conservative full-barrier effects.
- Explicit stack opcodes (`@stkswap`, `@stkroll`, `@stkcopy`) in
  ordinary routines: C0 real-stack mode already emits them verbatim as
  ordered opaque calls; against the symbolic stack their effects are
  untrackable, so the remaining work is spill-then-verbatim with
  honest depth accounting.
- Multi-arg `random()` / `box`: the shared-helper ownership relaxation
  above.

**Genuinely awkward**
- `@catch`/`@throw`: `@catch` is setjmp-shaped — a call that
  effectively returns twice — which LLVM IR has no comfortable encoding
  for, and catch tokens expose frame layout, which optimization
  legitimately changes (glulxercise's ten catch-token failures are the
  story hardcoding classic frame depths; the catch *values* all pass).

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

The hypothesis considered: glulxercise's 79 fallbacks might be reducing
medium-to-long-horizon optimization, and larger games might hit the same
thing. The hypothesis mostly does not hold — but the instinct that a
longer-horizon win exists is correct; it lives elsewhere.

**Fallbacks poison nothing beyond themselves.** The optimization horizon
is strictly intra-routine by design: one LLVM module per routine, no
inlining, no IPO, and every call is a full optimization barrier for
globals and RAM. The barrier is not a fallback concession — Glulx
semantics force it regardless, because any callee can touch any global
or RAM address, whether it was compiled direct or classic. A fallback
routine therefore costs exactly its own body's optimization and degrades
no neighbor by a single dispatch. Glulxercise is also the worst corpus
to extrapolate from: it is a VM conformance exerciser whose purpose is
to poke the weird opcodes (`@stkroll`, `@catch` chains, custom opcodes)
— hence 34% fallback there vs 0% on cloak.

**Real-game exposure is near zero.** The entire Inform library — the
parser, verb dispatch, everything — compiles with zero fallbacks.
Larger games are the library plus more ordinary Inform source, plus
occasionally a few inline-assembly routines from extensions, most of
which use `@glk` and covered opcodes. A rare `@catch`/`@throw` or
stack-trick routine compiles classic and costs its self-ops only. If one
is ever *hot* in a real game, the targeted fix is that one construct's
representation, justified by attribution evidence.

**Removing the shadow stream buys no code quality.** Proven, not
guessed: `I6_LLVM_SHADOW=0` is byte-identical on any zero-fallback
story. The shadow stream is write-only and never constrains the IR.
Removal buys ~5% compile time. Meanwhile the safety valve has an
underrated second job: it gracefully absorbs *limit* overflows
(symbolic-stack depth, slot pressure, connective depth) and
LLVM-version shape drift on arbitrary wild code. IR-only converts those
from silent classic degradation into compile failures of someone's
game; dynamic limits and a much broader fuzz corpus should precede any
such move.

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
routine, `CA__Pr` included, is already direct. The work is elsewhere: revisiting the
one-module-per-routine structure and being careful with the
call-barrier effect model once a callee's body is visible to the
caller's optimizer. Notably, inlining also *dissolves* the barrier
problem for inlined callees — LLVM sees the callee's actual global
accesses instead of assuming a full clobber, which may unlock secondary
wins (GVN/LICM across what used to be an opaque call).

**Recommended order if pursuing wins:**

1. Selective inlining of small hot routines, ranked by the per-routine
   attribution the benchmarks already print by default (calls x
   per-call overhead is the sort key, not routine size).
2. Construct-by-construct representation work only when a real game's
   profile demands it (`@catch`/`@throw` likely first to surface, and
   also the hardest).
3. Shadow removal last — it becomes nearly free once coverage is
   empirically total, and is pure downside before then.

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
