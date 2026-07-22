# LLVM Pipeline Review

Working document for the LLVM backend: architecture, settled policies,
and open work.

## Architecture

Parsing builds every routine as an LLVM function in **one module shared
by the whole compile**, while the classic generator's event stream is
captured per routine as the fallback shadow. Nothing is emitted during
the parse. At end of pass, each routine in parse order is: assigned its
address, given its header, then verified, optimized (LLVM function
passes), legalized, and lowered into Glulx instructions — or, if any
stage refuses, its shadow stream is replayed byte-exactly through the
classic encoder.

The division of labor is strict:

- **All optimization happens at the IR level** — LLVM's pass pipeline
  plus our legalization rewrites (IR-to-IR transforms that turn shapes
  LLVM likes into shapes the lowerer accepts, e.g. de-pointerizing
  select-of-globals). Future shape or performance work belongs here.
- **The lowerer (`llvm_lower.c`) is a plain translator.** It assigns
  representations (every value in a local slot, block-local values
  pooled, dedicated slots for phis and cross-block values), emits phi
  edge copies with parallel-copy staging, fuses a single-use same-block
  comparison into its conditional branch or select, and expands the few
  constructs Glulx lacks (unsigned division, funnel shifts,
  min/max/abs). It makes no profitability decisions.

Diagnostics: `I6_LLVM_DIAGNOSTICS=1` emits per-routine `LLVM-BACKEND`
TSV records plus IR dumps to `inform6-llvm-dump.ll`; ordinary compiles
print only the aggregate `LLVM: backends` counter lines, which the
tests pin. `I6_LLVM_SHADOW=0` disables shadow retention to prove
parsing never depends on the stored stream.

## Cost model (for future optimization work)

Dynamically dispatched Glulx instruction count is the primary
performance metric: every VM instruction pays a fetch/decode/dispatch
cycle. The `glulxe-counted` interpreter counts dispatches;
`--opcode-histogram` breaks them down; `tests/attrib.py` joins a
profiled run against a `$!asm` trace for per-routine attribution. Rank
optimization targets by measured dynamic attribution, never by static
emitted counts. A weighted per-opcode cost model (measured handler
costs fitted against whole-program timings) remains open work and is a
prerequisite for any future inlining decision-making.

Measurement caveats that remain true:

- Aggregate counters include only successfully lowered routines, so a
  new fallback *removes* its counts; count assertions must always be
  paired with backend coverage assertions (the suite does this).
- Fixture transcripts must never print raw object/routine/dictionary
  addresses: RAM layout shifts whenever code size changes.
- Capture baselines only from a clean build.

## Settled policies

- **Computed code addresses (`jumpabs` across routines) are unsupported
  under optimization**, as across upstream compiler versions generally.
  It lowers routine-locally as an opaque non-returning operation and
  the compiler warns. `tests/complianceTest.nix` pins the accepted
  glulxercise failure set exactly.
- **Classic-by-policy no longer exists**: every Glulx routine builds
  direct IR. C0 stack-argument routines run in real-stack mode (every
  `sp` operand is an ordered opaque `i6.stkpush`/`i6.stkpop`, explicit
  stack opcodes and computed-count calls emit verbatim), and the
  asterisk-trace preamble feeds the IR builder alongside the classic
  stream. The tests assert zero `backend=classic` records.
- **Shadow retention is permanent architecture.** The parser is
  streaming — a routine cannot be re-parsed — so the classic stream
  captured at parse time is the only way to emit a routine the
  pipeline rejects (catch/throw, stack manipulation against the
  symbolic stack, custom opcodes, multi-arg `random()`).
- **Classic generation stays.** It backs the fallback, debug/INFIX
  builds, and `$LLVM=0` bisection. The single-writer rule: direct
  hooks read parser state, classic generation is the sole writer (see
  EXPLAIN.md).
- **Debug-file (`-k`) builds bypass the LLVM pipeline entirely**;
  Z-code is untouched by all of this.
- Deferred emission means every parse-time consumer of a routine
  address must resolve through the routine's symbol (SYMBOL_MV).
  When touching emission order, hunt for reads of `symbols[].value`
  or pc-derived values between parse and end of pass — the class of
  bug that produced this branch's audit miscompiles (Replace's value
  copy, grammar tokens, the veneer table, "Main is first").

## Open work

- **Inlining, done properly this time.** The one-module architecture
  is the intended substrate: devirtualize direct `i6.callf*(i6.sym)`
  sites into real IR calls between functions in the module and let
  LLVM's own inliner (with a cost model grounded in the measured
  per-opcode weights above) do the work, instead of the removed
  clone-and-link prototype with its hand-rolled profitability gate.
  Git history preserves that prototype and its lessons
  (`30f6990..a34522b`).
- **Poison/undef/freeze needs proof.** The lowerer resolves
  `undef`/`poison` as zero and treats `freeze` as a no-op — safe only
  if the pass pipeline never lets LLVM UB/poison represent behavior
  Glulx defines. Division and shifts have explicit guards; nothing
  else does. If the invariant can't be proven, reject
  poison-dependent values instead of silently choosing zero.
- **Opcode effect classification is correctness-critical.** The
  pure/readonly/inaccessible-memory lists in `src/llvm_codegen.c`
  drive GVN, LICM, DCE, and sinking; a mistaken entry reorders across
  a VM callback or observable state change. Needs focused alias,
  callback, fault, and RNG-order tests.
- **Unsigned-division canonicalization**: InstCombine rewrites `sdiv`
  as `udiv` for provably nonnegative dividends; Glulx has no native
  unsigned divide, so the expansion costs several dispatches.
  Preventing the canonicalization (a legalization concern) remains
  desirable.
- Track compiler peak RSS and compile time on the largest corpus game
  as first-class benchmark outputs (all IR now lives to end of pass).
