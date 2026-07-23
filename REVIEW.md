# LLVM Pipeline Review

Planning document for the LLVM backend. The migration era is over:
every Glulx routine builds direct IR, the direct backend is the
parser-state writer by default, the whole language is representable
(box, raw code bytes, bare code blocks included), and the optimization
pipeline runs on every routine. Construct coverage is done — and it
bought correctness, not speed. This document is about what comes next:
the three things that keep LLVM's passes from paying off, and the
removal work that clears the road.

Companions: `EXPLAIN.md` (single-writer mechanics, backend mixing),
`SERIOUSLY-WANT-TO-ONLY-GENERATE-IR-AND-NOT-USE-CLASSIC-FALLBACK.md`
(the full inventory of replay legs, reject paths, and per-opcode IR
contracts).

## Where the pipeline stands

Parsing builds every routine as an LLVM function in one module shared
by the whole compile. Each function runs `mem2reg, instcombine,
simplifycfg, loop-mssa(licm), gvn, jump-threading, dce, simplifycfg`
plus our legalization rewrite, then the plain-translator lowerer
(`llvm_lower.c`) turns it into Glulx instructions replayed through the
classic encoder (branch shortening, backpatch). Classic generation
still *runs* during parse — its tree walk produces a discarded stream —
but it no longer writes parser state and, under the default, nothing
falls back to it.

And yet, measured by dynamic dispatch, direct output is currently
**behind** upstream's classic generator:

- optimization fixture: 601 dispatches direct vs 422 classic (pinned)
- cloak walk: 256,197 vs 164,995 upstream (+55%)
- Life: +24%; Advent: +61%

The passes are live but hemmed in on three sides. Those three issues
are the whole performance agenda.

## Issue 1 — the optimization horizon is one routine

Every source-level call lowers to an opaque `i6.callf*` with unknown
effects: a full barrier for globals and RAM, forced by Glulx semantics
as long as the callee is invisible. GVN and LICM stop dead at every
call, and IF code is call-dense — cloak's `Z__Region` alone is 17% of
all self-ops at ~9.5 ops per call, two of which are pure call/return
dispatch.

**The fix is devirtualization + inlining.** The one-module architecture
was built for exactly this: rewrite direct `i6.callf*(i6.sym)` call
sites into real IR calls between `i6fn.*` functions in the module and
let LLVM's inliner do the work. Inlining a callee dissolves its barrier
as a side effect — LLVM sees the actual global accesses instead of
assuming a full clobber — so this issue and Issue 2 partially collapse
into each other. Even without inlining, real IR calls let LLVM *infer*
function memory effects bottom-up, shrinking barriers at non-inlined
sites.

Plan:
1. Devirtualize `i6.callf*` sites whose callee is a known `i6.sym`
   routine marker (most calls in practice). Argument-count mismatch
   semantics (Glulx zero-fills missing args, drops extras) must be
   mirrored in the IR call's signature adaptation.
2. Switch the pass pipeline from per-function to module passes with the
   inliner enabled, gated by a cost model in Glulx dispatch terms —
   *not* LLVM's x86-flavored heuristics. Rank candidates by the dynamic
   attribution the benchmarks already print (calls × per-call
   overhead), starting with Z__Region-class helpers.
3. Recursion, `Replace`d routines, address-taken routines (stored to
   properties/globals) stay opaque calls — devirtualize only proven
   direct targets.

Lessons from the removed clone-and-link prototype are preserved in git
(`30f6990..a34522b`); the mistake to avoid repeating was hand-rolled
profitability with no measured cost model.

Prerequisite: a weighted per-opcode cost model (measured glulxe handler
costs fitted against whole-program timings), so the inliner trades
frame-setup savings against code growth in real units.

## Issue 2 — the effect ladder over-serializes

Everything LLVM cannot model natively is an opaque call, and its
declared memory effects are the entire license the optimizer has:

- pure: float/double math only
- readonly: `aload*`, searches, `gestalt`, `i6.deref`
- inaccessible-RW: RNG
- **everything else: full barrier** — all calls, all stream/glk output,
  `astore*`, stack ops, verbatims

The big lever here is Issue 1 (visible callees ⇒ inferred effects). The
independent improvements, in value order:

1. **Stream ops are full barriers only because of `@setiosys 1`**
   (filter mode routes every character through an arbitrary routine).
   A compile that can assume glk-only I/O could grade `streamchar`/
   `streamstr`/`streamnum` as "writes I/O state, never calls back into
   VM code", letting global/RAM accesses cross print statements — and
   print statements are everywhere. This needs a declared contract (a
   setting, or detection that the program never invokes iosys 1), not
   guesswork.
2. **`astore*` never calls back** — it could carry memory(readwrite)
   without the implied arbitrary-call clobber, ordering against loads
   and other stores but not against pure computation. Small but broad.
3. **The classification is correctness-critical and undertested.** A
   wrong entry reorders across a VM callback or an observable fault.
   Before widening any class, build focused tests: alias/callback
   traps, fault ordering, RNG-order.

Two pipeline-shape subitems live here as well:
- **Poison/undef discipline**: the lowerer resolves `undef`/`poison` to
  zero and treats `freeze` as a no-op — sound only if no pass lets
  LLVM-UB represent behavior Glulx defines. Division and shifts have
  explicit guards; prove the rest or reject poison-dependent values.
- **Unsigned-division canonicalization**: InstCombine rewrites `sdiv`
  to `udiv` for provably nonnegative dividends; Glulx has no unsigned
  divide, so the expansion costs several dispatches per site. Suppress
  the canonicalization at legalization.

## Issue 3 — the plain-translator lowering loses ground

Upstream's classic generator earns its numbers through emission
heuristics; our lowerer deliberately has none (settled policy: it
assigns slots, copies phi edges, fuses one shape — single-use same-block
compares into branches — and nothing else). The per-routine
`input → emitted` diagnostics show exactly where translation loses:

- `Opt_BooleanTree` 5 → 15: condition-as-value materializes as
  copy/branch/copy chains (Glulx has no setcc)
- `Opt_SwitchShared` 8 → 19: shared switch bodies duplicate edge copies
- `Opt_StoreFusion` 7 → 10, `Opt_InductionSelect` 13 → 23,
  `Opt_BranchLayout` 13 → 24: store targeting, select expansion, and
  block layout each pay copies classic avoids

The policy stands — profitability lives at the IR level or in
legalization, not in the lowerer — so recovery means: better IR shapes
in (a legalization pass that sinks compares to their consumers, orders
blocks for fallthrough, and avoids value-form booleans where a branch
form suffices), plus at most *shape-local* lowerer fusions in the
spirit of the existing compare-branch fusion (e.g. compare-into-select,
store-into-destination instead of slot-then-copy). Every candidate gets
ranked by dynamic attribution on the cloak/Advent walks, not by static
counts — that rule has already prevented one wasted phase.

Note the `input=` metric itself is dying: under the flipped writer it
measures classic's skewed discarded stream. When the classic tree walk
goes (below), replace it with pre-optimization IR instruction count,
which the diagnostics already dump.

## The road: removal work that clears these issues

### Next: remove the classic tree walk

Classic generation still walks every expression tree and statement,
emitting instructions that are counted, captured under the classic
writer, and otherwise thrown away. It writes nothing the parser needs
anymore. Removing it buys:

- roughly half the per-routine generation work at compile time
- the end of the dual-generation entanglements: the `random()` table
  handoff inverts (direct keeps its own table), the mid-statement
  reachability reads in `expressc.c` stop mattering, the `input=`
  metric is replaced
- a single, honest code path to maintain before optimization work
  starts touching call sites

Mechanics: for Glulx with the direct writer active, the generation
halves of `code_generate`/`generate_code_from` and the statement
handlers' `assembleg_*` calls are skipped; parsing, error reporting,
string/dictionary/data-area side effects, and the veneer-marking calls
must keep running. The dissection in the inventory doc's §1 is the
checklist: everything classic generation does *besides* emit
instructions has to be identified and kept. `$LLVM=0`, debug builds,
and Z-code keep the classic path — this is a removal from the Glulx
direct mode, not a deletion of the generator.

Risks to test for: parse-time side effects hiding inside emission code
(`compile_string` reachability gating, veneer_routine marking inside
generation branches, warnings emitted from generator guards), and the
error-recovery path (source errors mid-routine currently rely on the
classic stream when direct never started — that path keeps the walk).

### Later: remove the shadow stream and the classic-writer mode

After the tree walk is gone and a broad wild-code sweep (many titles,
`I6_LLVM_PARSER_WRITER` default) shows the reject rate is genuinely
zero-or-actionable, delete the capture seam: shadow events, replay of
captured streams, `I6_LLVM_PARSER_WRITER`/`I6_LLVM_SHADOW`, the
parser-state cross-check, and the classic-writer suspension dance. What
must stay is the *encoding* replay — the lowerer feeds
`assembleg_instruction` through the event buffer so the classic
encoder's branch shortening and backpatching keep working. That leg is
load-bearing for the IR path itself.

Sequencing note: the wild-code sweep should precede this deletion, not
follow it — every reject found while the classic-writer escape hatch
still exists is a cheap bug report; afterward it is someone's broken
build.

## Measurement (unchanged doctrine)

Dynamic dispatch count is the primary metric: every VM instruction pays
fetch/decode/dispatch. `glulxe-counted` counts; `--opcode-histogram`
breaks down; `tests/attrib.py` joins a profiled run against a `$!asm`
trace for per-routine attribution. Rank all optimization work by
measured dynamic attribution, never static emitted counts.

Caveats that remain true:
- Count assertions must pair with backend-coverage assertions (a new
  reject silently removes its counts from aggregates).
- Fixture transcripts must never print raw object/routine/dictionary
  addresses; RAM layout shifts with code size.
- Baselines only from a clean build.

## Standing invariants

- **`jumpabs` across routines is unsupported under optimization**;
  lowers routine-locally as opaque noreturn, with a warning.
  `tests/complianceTest.nix` pins the accepted glulxercise failure set
  (1 jumpabs + 10 catch-token frame-layout checks) exactly.
- **Remaining rejects** (compile errors under the default writer):
  custom two-store opcode shapes, degenerate Z-only statements, and
  malformed tree shapes. `I6_LLVM_PARSER_WRITER=classic` restores
  graceful fallback and the parser-state cross-check.
- **Debug-file builds bypass the pipeline entirely** (sequence points
  do not survive reordering); Z-code is untouched by all of this.
- **Deferred emission**: every parse-time consumer of a routine address
  must resolve through the routine symbol (SYMBOL_MV). When touching
  emission order, hunt for reads of `symbols[].value` or pc-derived
  values between parse and end of pass — the bug class behind this
  branch's audit miscompiles.
- **Frame-layout exposure is out of contract**: catch tokens and raw
  code-byte blobs that hardcode slot offsets observe layouts that
  lowering legitimately changes.
- Track compiler peak RSS and compile time on the largest corpus game
  as first-class benchmark outputs (all IR lives to end of pass).
