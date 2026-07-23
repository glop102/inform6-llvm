# The Full Inventory: Replay, Fallback, and IR Restrictions

A survey of exactly (1) which parts of the compiler require the parse
stream to be captured and replayed, (2) every path that can still land a
routine in classic generation instead of IR, and (3) every IR shape the
direct backend can emit and the restriction attached to each one that
limits how LLVM may restructure it.

State as of this writing: the whole corpus (cloak, Advent, glulxercise,
fixtures) builds **zero fallbacks** — but the fallback *machinery* is all
still present and reachable by wild code. This document is the map of
that machinery.

---

## 1. Parts that require replay of the parse stream

"Replay" here means: the capture/replay seam in `src/asm.c` (the block
comment at `src/asm.c:1055` is the authoritative overview). Instruction
and label emission for every Glulx routine is suppressed during parsing
and stored as a stream of `llvm_event` records (`src/asm.c:1093`); at
emission time that buffer is fed back through `assembleg_instruction()`
by `llvm_replay_routine()` (`src/asm.c:1235`). There are two distinct
streams that ride this one mechanism, and they have different reasons to
exist.

### 1.1 The lowered stream — replay is the *encoding path*, not a fallback

Even a routine that never falls back is replayed. The lowerer
(`llvm_lower.c`) does not write bytes; it emits `assembly_instruction`
records into the same event buffer via `llvm_buffer_reset()` /
`llvm_buffer_append_instruction()` / `llvm_buffer_append_label()`
(`src/asm.c:1281-1296`), and asm.c replays that buffer through the
classic **encoder**. This is deliberate: the encoder owns

- **branch shortening** (`transfer_routine_g()`), which needs the whole
  routine's instruction stream before final offsets exist;
- **backpatch markers** (operand `marker`/`symindex` fields resolved at
  end of pass or link time);
- **operand size selection** (byte/short/long constant encoding).

Removing "the replay code" therefore cannot mean removing this leg
unless the lowerer grows its own encoder, branch-shortener, and
backpatcher. Classic *generation* is deletable in principle; classic
*encoding* is load-bearing for the IR path itself.

### 1.2 The shadow stream — replay is the fallback

The parser is streaming; there is no AST and a routine cannot be
re-parsed. If the direct build or the lowerer rejects at the *end* of a
routine, the source is already consumed. The only way to still emit the
routine is the classic stream captured at parse time. Requiring parts:

- **Capture stubs in the assembler** — `assembleg_instruction()` at
  `src/asm.c:1785` and `assemble_label_no()` at `src/asm.c:2107`
  suppress encoding and (when `cur_emit.shadow_store`) store the event.
- **Fallback replay** — `assemble_routine_end()` (`src/asm.c:2723`,
  eager path) and `emit_deferred_routines()` (`src/asm.c:2346`,
  deferred path): if `llvm_pipeline_routine()` /
  `llvm_lower_retained_routine()` returns FALSE, the shadow buffer is
  replayed verbatim. With `I6_LLVM_SHADOW=0` this becomes a compile
  error instead.
- **Error recovery** — a routine begun with source errors never builds
  IR at all, and its captured stream is its *only* output (forced
  `shadow_store = TRUE` at `src/asm.c:2573`); errors appearing
  mid-routine abandon the direct attempt at `assemble_routine_end()`
  (`src/asm.c:2701-2708`).

### 1.3 Parser bookkeeping — the reason classic generation *runs at parse time at all*

The parser reads assembler globals while still parsing the rest of the
routine, so *something* must write them as each statement parses:

- `sequence_point_follows` — one-shot, consumed per instruction.
- `execution_never_reaches_here` — drives implicit-return emission,
  unreachable-statement warnings, statement skipping.
- `labeluse[]` — per-label forward-branch counts;
  `assemble_forward_label_no()` materializes labels from it and
  `transfer_routine_g()` *decrements* it during branch shortening, so
  counts must be exact.

`shadow_note_instruction()` (`src/asm.c:1224`) is the single writer of
this state at parse time, through the backend-independent
`asm_parser_*` helpers. It runs for every suppressed instruction
**whether or not the event is stored** — that is what `I6_LLVM_SHADOW=0`
proves (parsing does not depend on the stored stream, only on the
bookkeeping). Direct IR hooks read this state (`direct_can_emit()`
checks `execution_never_reaches_here`) but never write it.

Consequence for cleanup: to stop running classic generation entirely,
the direct backend must *become* the single writer by calling the same
`asm_parser_*` helpers. EXPLAIN.md ("Is single-writer a blocker") calls
this flip mechanical — the Phase 6 seam was built for it — but it has
not been done.

### 1.4 Deferred lowering — replay out of parse order

Under deferred lowering (the default for Glulx + LLVM;
`src/asm.c:2150-2372`), nothing is emitted during parsing. Every
routine's captured stream is deep-copied into a `deferred_routine`
stash (`stash_deferred_routine()`, `src/asm.c:2235`) together with its
IR handle, label high-water mark, and shadow counters; at end of pass
`emit_deferred_routines()` restores that per-routine context, assigns
the address, lowers the retained IR (or falls back), replays, and
transfers. Requiring parts:

- routine addresses assigned only at end of pass — every reference
  resolves through the routine symbol (`SYMBOL_MV`), including the
  `Replace X Y` attachment dance (`defer_replace_original()`,
  `src/asm.c:2277`);
- per-routine label state (`next_label`) restored so lowering can
  allocate labels above the parsed count;
- the routine header emitted at end of pass so
  `llvm_patch_routine_locals()` (`src/asm.c:1313`) can rewrite the
  local count when lowering needs more slots than the source declared.

### 1.5 Small but real replay dependencies

- **Custom `@"..."` opcodes** — the assembler keeps only the most
  recently parsed custom descriptor in the `custom_opcode_g` static, so
  every captured event snapshots it (`llvm_capture_instruction()`,
  `src/asm.c:1197`) and replay restores it (`src/asm.c:1255`); the
  lowered path restores it via `glulx_set_custom_opcode()` from the
  name-encoded descriptor.
- **Label events** — capture clears `EXECSTATE_ENTIRE` so the
  strip-label check doesn't fire twice (`src/asm.c:1217`); replay
  preserves the label's symbol association across
  `assemble_label_no()` (`src/asm.c:1248`).
- **Asterisk-trace preamble** — `assembleg_traced()` (`src/asm.c:2379`)
  feeds each preamble instruction to *both* generators so the direct IR
  isn't missing the preamble; it is ordinary captured code.
- **Data-area handoff for multi-arg `random()`** — the direct build
  creates the constants array and queues its address
  (`llvm_direct_random_array_set/take`, `src/llvm_codegen.c:986`);
  classic generation of the same expression tree *takes* the address
  instead of building a duplicate (`src/expressc.c:2878`). This only
  works because classic generation still walks the same parse right
  after the direct build — a dependency on the dual-generation
  arrangement itself.

### 1.6 What the replay is *not* needed for

`I6_LLVM_SHADOW=0` is byte-identical on any zero-fallback story: the
stored shadow stream is write-only with respect to the IR and never
constrains it. Its retention costs ~5% compile time. The *reduction*
opportunity is therefore: (a) stop storing shadow events (keep the
bookkeeping), accepting that any fallback becomes a compile error; and
(b) eventually flip the bookkeeping writer to the direct backend and
stop running classic generation. The encoding replay (1.1) stays either
way.

---

## 2. Parts that fall back to classic code generation

Fallback granularity is the **routine, winner-take-all**: either the
entire body is lowered IR, or the entire body is the replayed shadow
stream. There are no classic islands inside an IR routine. Failure is
reported per stage: `direct-build` (rejected while parsing),
`direct-finish` (builder not finalized), `direct-verify`,
`direct-optimize`, `direct-post-verify`, `direct-lower`
(`process_direct_routine()`, `src/llvm_codegen.c:1949`).

### 2.1 Whole-compile gates (no IR is ever attempted)

- **Z-code mode** — the seam is Glulx-only.
- **`$LLVM=0`** — the miscompile-bisection escape hatch.
- **No-LLVM builds** — `llvm_stub.c`'s `llvm_codegen_available()`
  returns FALSE; pure classic compile.
- **Debug-file (`-g`) and Infix (`-X`) builds** — capture never starts
  (`src/asm.c:2554`): sequence points don't survive instruction
  reordering. This is a design problem (debug info under `-O2`), not a
  codegen gap.
- **`track_unused_routines` (`-u`)** disables *deferral* only (it
  records emission PCs at parse time); capture and eager per-routine
  lowering still run (`deferred_lowering_wanted()`, `src/asm.c:2176`).
- **Mid-source `Switches` conflict** with the latched deferral decision
  is fatal (`deferred_lowering_latch_conflict()`, `src/asm.c:2192`).

### 2.2 Per-routine abandonment (source errors)

- Routine begins while `no_errors`/`no_compiler_errors` are nonzero:
  no IR attempt; shadow is primary output (`src/asm.c:2568-2574`).
- Errors appear during the routine: `llvm_direct_routine_abandon()` at
  routine end (`src/asm.c:2702`).

### 2.3 Build-time rejects — `llvm_direct_reject()` call sites

Every reject reason string, grouped by module. Each one is a latent
classic fallback for whatever wild code triggers it.

**Statements (`src/states.c`, `src/llvm_codegen.c:1023`)**

- `"code block"` (`states.c:1926`) — a bare `{ ... }` braces block used
  as a statement.
- `"unsupported statement"` — `llvm_direct_note_statement()` allowlist.
  Allowed: return/rtrue/rfalse, jump, if, break, continue, do, for,
  while, switch, print, print_ret, new_line, give, move, remove, font,
  style, string, quit, objectloop, spaces, inversion. Rejected in
  practice: **`box`** (its text table is compiled into the data area by
  classic's statement handler during parsing — an ownership conflict;
  the `random()` handoff is the template if it ever matters), plus the
  degenerate/Z-only codes (sdefault, read, save, restore, stray
  else/until).

**Inline assembly (`src/asm.c`, `src/llvm_codegen.c:1257-1892`)**

- `"raw code bytes"` (`asm.c:4434`) — `@ -> ...` / `@ --> ...` code
  byte arrays: arbitrary bytes with no instruction-level meaning.
  Excluded by design, permanently.
- `"unsupported inline operand"` / `"unsupported inline store operand"`
  — an operand type outside constant/local/global/sp/dereference.
- `"inline local out of range"`, `"inline operand count mismatch"`.
- `"unsupported verbatim opcode shape"` — a verbatim (custom/raw)
  opcode that is two-store, or branch+store combined.
- `"inline opcode with two stores"` — a `.ss2` opcode that also has
  stack-passed extras, or >16 sources.
- `"inline opcode with too many operands"` (>16 — unreachable in
  practice: `assembly_instruction` itself holds at most 8 operands).
- `"VM stack value carried across control flow"` — pending symbolic-sp
  values at a control-flow point in a terminated block
  (`direct_symstack_spill()`, `src/llvm_codegen.c:154`). The symbolic
  stack itself grows on demand (no depth cap).
- `"sp read with empty symbolic stack"`, `"verbatim sp operand"`,
  `"explicit stack-manipulation opcode"`,
  `"untrackable computed call"` — each only fires when real-stack
  **escalation** itself fails (only possible in a terminated block);
  normally these constructs escalate the routine to real-stack mode and
  proceed (`direct_stack_escalate()`, `src/llvm_codegen.c:113`).
- `"unsupported assembly macro"` — a macro other than
  @pull/@push/@dload/@dstore.
- `"invalid source label"`, `"operation after terminator"` (emitting
  into a terminated block while the parser believes code is reachable —
  an announcement bug, by construction).
- `"unknown direct opcode"`, `"unsupported direct opcode shape"`,
  `"direct opcode arity mismatch"` — via `llvm_direct_glulx_op()`
  (store2/branch shapes must come through the inline-assembly path, not
  the expression path).

**Expressions (`src/expressc.c:3278-4576`)**

- `"unsupported expression operand"`, `"unsupported assignment
  destination"` — operand kinds outside the modeled set.
- `"invalid call tree"`, `"invalid indirect call"`,
  `"unsupported call arity"` (only a negative child count — a malformed
  tree; argument lists themselves are unbounded).
- System functions: `"invalid metaclass call"`, `"unsupported random
  arity"`, `"non-constant random alternative"` (multi-arg `random()`
  with variable alternatives), `"invalid object-tree call"` (children/
  parent/sibling shapes), `"unsupported system function"` (anything
  outside the handled set).
- `"unknown checked array"` — strict-mode array bound checks against an
  array the direct path can't identify.
- Tree-shape guards (all of the form "the expression tree isn't the
  shape the generator expects"): `"invalid expression tree"`,
  `"unsupported assignment expression"`, `"invalid logical
  expression"`, `"invalid comma expression"`, `"invalid push
  expression"`, `"invalid unary expression"`, `"unsupported increment
  destination"`, `"invalid zero comparison"`, `"invalid comparison
  arity"`, `"invalid binary expression"`, `"invalid array
  read/assignment/update"`, `"invalid attribute test"`, `"invalid
  containment test"`, `"invalid class test"`, `"invalid property
  expression/assignment/call"`, `"unsupported expression operator"`,
  `"invalid condition tree"`, `"invalid logical condition"`,
  `"unsupported expression statement"`.

**Routine finalization (`src/llvm_codegen.c:572-607`)**

- `"invalid fallthrough block"` — front end says fallthrough reachable
  but the builder's block is terminated/absent.
- `"empty direct body"` — a single empty entry block, meaning the body
  was raw-assembled behind the builder's back without announcement;
  rejected so a missed announcement can't silently drop a routine.

**Expression-level operator guards (`src/llvm_codegen.c`)**

- `"unsupported unary/binary expression operator"`, `"unsupported
  comparison operator"`, `"invalid division operator"`, `"invalid
  local/global expression/assignment/return"`, `"invalid adjusted
  variable"`.

### 2.4 Pipeline-stage failures (after a successful build)

`process_direct_routine()` (`src/llvm_codegen.c:1949`):

- **`direct-verify`** — `LLVMVerifyFunction` fails (e.g. a phi left
  with missing edges after a soft failure; deliberate safety net).
- **`direct-optimize`** — `LLVMRunPassesOnFunction` returns an error
  (LLVM's own breakage; LLVM 21's instcombine fixpoint verifier
  hard-abort was hit during this project).
- **`direct-post-verify`** — verification after the
  `unmerge_pointer_selects` legalization.
- **`direct-lower`** — the lowerer refuses (next section).

### 2.5 Lower-time failures — `lfail()` sites in `src/llvm_lower.c`

Phase A (classify/validate/slots) is side-effect-free precisely so that
a refusal here can still fall back to shadow replay:

- Width limits: `"narrow arithmetic"`, `"narrow logic op"`, `"narrow
  compare"`, `"signed compare of i1"` (0/1 slot representation vs
  LLVM's true=-1), `"narrow select"`, `"narrow phi"`, `"narrow unsigned
  division"`, `"zext/sext from unhandled width"`, `"trunc to/from
  unhandled width"`, `"trunc escapes"`, `"narrow trunc with non-extend
  user"`, `"narrow extend not fed by trunc"`.
- Unsigned division: `"unsigned division by non-constant"` (Glulx has
  no unsigned divide; only the constant-divisor halving trick is
  lowered), `"unsigned division by zero"`.
- Memory model: `"memory access not to an i6 global"`, `"memory access
  to unknown global"` — the operand model has no indirect global
  access; the unmerge legalization exists to keep instcombine/
  jump-threading from producing pointer selects/phis, and anything it
  misses (non-i6.g leaves; the networks themselves grow without
  bound) lands here.
- Calls: `"indirect call"`, `"unhandled intrinsic"` (anything beyond
  assume/lifetime/dbg/donothing, smax/smin/umax/umin/abs.i32, and
  constant-count fshl/fshr.i32), `"funnel shift by non-constant"`,
  `"call to unknown function"`, `"unknown opcode call"`, `"unknown
  verbatim opcode"`, `"malformed verbatim call"`, `"verbatim name too
  long"`, `"opcode name too long"`, arg-count mismatches for
  stkpush/stkpop/catchtok/catchflag/verbatim/opcode calls, `"two-store
  opcode"` / `"branch opcode call"` (shape suffix disagrees with opcode
  flags), `"catchflag without token slot"`.
- Operands: `"unlowerable operand/…"` family — any operand that isn't a
  constant, undef/poison, i6.sym call, i6.g global, parameter, or a
  VK_SLOT value; `"computed deref address"` (i6.deref addresses must be
  encodable as `DEREFERENCE_OT`: plain or marked constants only);
  `"void return"`.
- Resources: `"too many local slots"` (the Glulx format ceiling of
  16384 — a local operand's two-byte frame offset — not an
  implementation choice; frames past 250 slots emit a multi-pair
  locals format),
  `"routine header not patchable"` (something landed in the holding
  area after the header), `"empty function"`, `"alloca survived
  optimization"` (mem2reg failed), `"param count mismatch"`
  (compiler bug guard).

### 2.6 Summary of the *irreducible* fallback set

Per EXPLAIN.md, everything else having been closed, what remains is:

1. **By design**: raw code-byte arrays; `box` (and the residual
   unsupported statements) — no IR representation, nothing in the
   corpus needs one.
2. **Design problems**: debug/INFIX builds (sequence points vs
   reordering) — a whole-compile gate, not a per-routine fallback.
3. **Limits**: all dynamic since 2026-07-22 (symbolic stack, call
   arity, switch nesting, unmerge networks, random-array queue grow on
   demand; `stories/direct-ir-limits.inf` pins every old cap exceeded).
   The one that remains is the Glulx frame format itself: 16384 local
   slots, the two-byte local-offset addressing ceiling.
4. **LLVM itself**: pass-pipeline errors and shape drift across LLVM
   versions on arbitrary wild code.

---

## 3. IR code points and their restructuring restrictions

Everything is `i32` (`datalayout "e-p:32:32-i32:32-n32"`), one function
per routine, all functions in **one shared module** — but routines never
reference each other's functions (calls go through `i6.sym` +
`i6.callf*`), so there is deliberately no IPO/inlining and function
deletion is always safe. The pass pipeline is fixed and function-scoped:
`mem2reg, instcombine, simplifycfg, loop-mssa(licm), gvn,
jump-threading, dce, simplifycfg` (`src/llvm_codegen.c:51`), followed by
the `unmerge_pointer_selects` legalization.

### 3.1 Native IR (fully restructurable)

| Shape | Emitted for | Restriction |
|---|---|---|
| `add/sub/mul/and/or/xor`, `neg/not` | arithmetic ops, `@add` etc. | none — free to fold, reassociate, CSE |
| `sdiv/srem` | `/`, `%`, `@div/@mod` | **only** when the divisor is a visible constant ≠ 0, ≠ −1; otherwise opaque `i6.div/i6.mod` — Glulx defines faults/overflow where LLVM has UB |
| `shl/lshr/ashr` | `@shiftl/@ushiftr/@sshiftr` | only constant counts < 32 (Glulx defines counts ≥ 32; LLVM gives poison); count ≥ 32 folds to 0 / sign; variable counts stay opaque |
| `icmp` + `zext` | conditions, comparisons | lowered natively; fused into its branch/select when single-use, same-block |
| `select`, `phi` | conditionals, loop values | must be i32 or i1 by lowering time |
| `load/store` on `i6.g<n>` module globals | I6 global variables | the only native memory access; must resolve to a *direct* global by lowering time (unmerge de-pointerizes select/phi-of-global networks) |
| `alloca` + `load/store` | locals (parameters stored at entry) | must be fully eliminated by mem2reg |
| `br` / `condbr` / `switch` / `ret` / `unreachable` | all control flow | CFG is free to restructure — this is where simplifycfg/jump-threading pay off |
| `trunc`/`sext`/`zext` (i8/i16/i1) | `@sexs/@sexb`, masking idioms | only the sext/zext(trunc x) sandwich shapes; a narrow value must never escape |
| `llvm.smax/smin/umax/umin/abs.i32`, `llvm.fshl/fshr.i32` (const count) | canonicalized by instcombine | lowered open-coded; anything else intrinsic ⇒ lower fail |

### 3.2 `i6.sym` — symbolic constants (the backpatch boundary)

`i6.sym(marker, value, symindex)` (`src/llvm_codegen.c:675`) carries any
operand the backpatcher will fix up (routine addresses, string/array
addresses, veneer refs). Marked `memory(none), nounwind, willreturn,
speculatable` — identical calls merge, dead ones vanish, they are never
barriers. **The restriction is that the value can never constant-fold**:
no arithmetic on a routine address, no branch folding on an address
comparison. This is the price of end-of-pass address assignment and is
inherent, not incidental. Internal-routine refs are re-deferred through
`SYMBOL_MV` at resolve time (`src/llvm_lower.c:969`) so end-of-pass
address assignment still works.

### 3.3 Opaque opcode calls — `i6.<opcode>` — and the effect ladder

Any Glulx operation LLVM can't model natively becomes a typed opaque
call; the declared memory effects are the *entire* license LLVM has to
move it (`mark_opaque_fn_attrs`, `src/llvm_codegen.c:263`):

1. **Pure** (`memory(none)`, speculatable): all float/double math
   (`numtof … atan2, dtonumz, dtonumn, dtof`). Free to CSE, hoist,
   sink, delete, speculate.
2. **Readonly** (`memory(read)`, *not* nounwind/willreturn): `aload,
   aloads, aloadb, aloadbit, linearsearch, binarysearch, linkedsearch,
   gestalt`, and `i6.deref`. May CSE/delete, but may **not** be
   speculated or reordered past stores — invalid addresses fault
   observably, searches over malformed data may not terminate.
3. **Inaccessible-RW** (`memory(inaccessiblemem: readwrite)`):
   `random, setrandom`. Stay ordered with respect to each other and
   never merge, but RAM/global loads and stores move freely across.
4. **Full barrier** (unmarked, unknown effects): *everything else* —
   all stream/glk output (`streamchar`, `streamstr`, `glk`…), all
   calls, `astore*`, `i6.deref.store`, stack ops, verbatims. Nothing
   crosses. Stream ops are deliberately here because under
   `@setiosys 1` (filter) every character can invoke arbitrary VM code.

**This ladder is the central optimization impediment.** Every
source-level call (`i6.callf/callfi/callfii/callfiii/call.s`) is a full
clobber of all globals and RAM — forced by Glulx semantics (any callee
can touch anything), so GVN/LICM stop at every call. Inlining
(devirtualizing `i6.callf*(i6.sym)` into real IR calls) is the known
route to dissolving the barrier for visible callees.

Suffix contracts on opaque calls (parsed back by the lowerer,
`src/llvm_lower.c:522-570`):

- **`i6.<op>.s`** — trailing args are stack-passed, listed in runtime
  pop order; the lowerer pushes them in reverse to rebuild the stack.
- **`i6.<op>.br`** — a branch opcode materialized as its taken/not-taken
  0/1 answer; re-emitted as the real branch into a 0/1 slot (or a
  converging label when the answer is unused). Cost: a real branch +
  two copies when the value is live.
- **`i6.<op>.ss2`** — a two-store opcode with both stores redirected to
  sp; the two `i6.stkpop`s that follow route the values to the real
  destinations.

### 3.4 The VM stack — symbolic window, spills, and real-stack mode

Inline `sp` operands ride a parse-time **symbolic stack** of SSA values
(grown on demand, `src/llvm_codegen.c`). Restrictions this imposes:

- The symbolic stack must be **empty at every control-flow divergence
  or join** — a symbolic value has no runtime presence, and classic's
  real stack could carry a value along only one path. Pending values
  are spilled as `i6.stkpush` calls at the same source points classic
  pushed (`direct_symstack_spill`), and statement-level control flow
  triggers spills via `llvm_direct_note_control_flow()`.
- `i6.stkpush`/`i6.stkpop` carry **full side effects** — their order is
  frozen exactly as classic executed it. In real-stack mode every sp
  operand is one of these, so a C0 routine's stack traffic is entirely
  opaque to optimization.
- **Escalation is one-way and mid-build**: at the first untrackable
  construct (@stkcopy and friends, computed-count calls, verbatim sp
  operands, popping past everything the routine pushed) the routine
  spills and flips to real-stack mode permanently
  (`direct_stack_escalate`). Correct because elided push/pop pairs were
  net zero, so after the spill the real stack provably matches
  classic's.
- Returns discard the symbolic stack (values die with the frame).

### 3.5 Verbatim calls — `i6.custom.<code>.<flags>.<forms>` / `i6.raw.<name>.<forms>`

For custom `@"..."` opcodes (operand roles unknowable) and the sub-word
copies `@copys`/`@copyb` (memory operands accessed at narrow widths).
Per-operand forms are name-encoded: `v` value, `g` global passed **by
pointer** (the instruction touches the global's memory directly — the
only place global pointers appear as call args), `p` sp kept as a real
stack access (escalates the routine), `r` declared store returned as
the call result; `.br` marks a branch-label final operand.

Restrictions:

- Operands keep their original addressing forms — no substitution.
- **No generated instruction may land between consecutive verbatim
  instructions**: custom jump forms may branch by raw byte offsets
  computed against the emitted layout, so pending spills are
  materialized *before* the verbatim (`src/llvm_codegen.c:1490`).
- Full-barrier effects; `g`-form global pointers additionally pin the
  global against surrounding load/store motion.
- Two-store and branch+store verbatim shapes are rejected outright.

### 3.6 `@catch` / `@throw` — the setjmp pair

`i6.catchtok` (no args, yields the token) paired with
`i6.catchflag(tok)` (1 on the branch path, 0 on the throw-resume path)
(`src/llvm_codegen.c:1789`). The lowerer emits **one real
`catch S ?L`** whose store operand is catchtok's slot, so a throw
rewrites exactly the value the IR reads (`src/llvm_lower.c:1538`).
Restrictions: the token must live in a slot (fails if optimized into
anything else); and catch tokens inherently **expose frame layout**,
which optimization legitimately changes — glulxercise's ten catch-token
failures are the story hardcoding classic frame depths (pinned as
accepted; the catch *values* all pass). `@throw` is noreturn and ends
the block.

### 3.7 `i6.deref` / `i6.deref.store`

Inline dereference operands (`DEREFERENCE_OT`). Readonly / full-barrier
respectively; the address must remain a plain or marked constant — a
**computed** deref address has no encodable operand form and fails
lowering.

### 3.8 Lowerer-side representation constraints (back-pressure on the IR)

The lowerer is a plain translator; its accepted-shape envelope is
effectively a contract the pass pipeline must not exceed. These are
restrictions not on single instructions but on what optimized IR may
look like:

- Every live SSA value must be representable in a Glulx local slot:
  i32 everywhere, i1 only in the 0/1 slot convention (which is why
  *signed* i1 compares and i1 arithmetic are rejected — instcombine
  canonicalizes the latter away).
- ≤ 16384 slots after pooling (the Glulx two-byte local-offset
  ceiling; >250 slots emits a multi-pair locals format); phis and
  cross-block
  values get dedicated slots, block-local values share by linear scan;
  parallel phi copies may need staging scratch slots.
- No indirect global access, no pointer values at all beyond the
  `i6.g` leaves the unmerge pass de-pointerizes.
- No intrinsics beyond the small allowlist; no module-level passes; no
  inlining (would create IR calls the lowerer has no representation
  for — devirtualization must come with lowering support).
- Adding any new pass to `LLVM_PASS_PIPELINE` means auditing every
  shape it can produce against `classify()`/`validate()`; the stated
  policy (REVIEW.md) is that new shape normalization belongs in
  legalization, never in the lowerer.

### 3.9 Cross-cutting invariants that constrain restructuring work

- **Single-writer rule**: direct hooks may read
  `execution_never_reaches_here` / label state but never write parser
  bookkeeping while classic generation runs. Any change that makes the
  direct backend emit parser-visible side effects must first flip the
  writer wholesale.
- **Optimization horizon is strictly intra-routine** by design; every
  call is a full barrier regardless of backend. A fallback routine
  costs exactly its own body and poisons no neighbor.
- **`jumpabs` across routines is unsupported under optimization**
  (settled policy) — computed code addresses lower routine-locally as
  an opaque non-returning op, with a warning and a pinned
  compliance-failure set.
- **Suspend/resume** (`llvm_direct_suspend`, used by states.c around
  unreachable-code parsing) means the builder deliberately ignores
  stretches the parser knows are dead — emitters must announce raw
  assembly or the empty-body/after-terminator guards reject.

---

## Appendix: where to look

| Concern | Location |
|---|---|
| Capture seam overview | `src/asm.c:1055` block comment |
| Event record / capture / replay | `src/asm.c:1093-1270` |
| Lowered-stream buffer API | `src/asm.c:1281-1342` |
| Deferred lowering | `src/asm.c:2150-2372` |
| Routine end / fallback decision | `src/asm.c:2668-2743` |
| Direct builder + effect model + legalization | `src/llvm_codegen.c` |
| Symbolic stack / escalation | `src/llvm_codegen.c:76-172` |
| Inline-assembly translation | `src/llvm_codegen.c:1257-1892` |
| Pipeline + retain/defer | `src/llvm_codegen.c:1949-2078` |
| Lowerer classify/validate (the shape contract) | `src/llvm_lower.c:269-743` |
| Slot assignment / emission | `src/llvm_lower.c:851-1822` |
| Statement allowlist | `src/llvm_codegen.c:1023` |
| Expression-tree rejects | `src/expressc.c:3278-4576` |
| Design rationale | `EXPLAIN.md`, `REVIEW.md` |
