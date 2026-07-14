# inform6-llvm: LLVM-based code generation for Inform 6

## Goal

Route Inform 6 code generation through LLVM IR so routines benefit from LLVM's
optimization passes (mem2reg, instcombine, GVN, simplifycfg, DCE, ...), then
lower the optimized IR back to VM bytecode. The output is still a normal story
file; LLVM is used purely as a mid-level optimizer, not as a native backend.

## Upstream codegen, as it actually works

The upstream compiler (in `./inform6/`, working copy in `./src/`) is a
single-pass compiler:

- `expressp.c` parses expressions into `expression_tree_node` trees;
  `expressc.c` walks them and emits `assembly_instruction` structs.
  `states.c` emits statement-level control flow directly (labels + branches).
- `asm.c` is the assembler. `assembleg_instruction()` (Glulx) /
  `assemblez_instruction()` (Z-machine) immediately encode each instruction
  into a per-routine **holding area** (`zcode_holding_area`, with a parallel
  `zcode_markers` byte array recording backpatch markers per byte).
- Within a routine, branch targets are **label numbers**; at
  `assemble_routine_end()`, `transfer_routine_g/z()` resolves labels to
  offsets, shortens branch forms, and dumps the routine into the code area.
- Cross-routine references (routine addresses, strings, arrays, globals,
  dict words...) are `?_MV` marker values on operands, resolved after layout
  by `bpatch.c`.

Key data types (header.h): `assembly_operand {type, value, symindex, marker}`,
`assembly_instruction {internal_number, operand_count, operand[8], ...}`.

In Glulx mode: `LOCALVAR_OT` value 0 is the VM stack ("sp" addressing mode),
values 1..n are the routine's locals (args arrive in locals 1..k).
`GLOBALVAR_OT` are global variables. Everything is a 32-bit word.

## Architecture: per-routine round-trip through LLVM

**Target: Glulx only.** Glulx is a 32-bit orthogonal VM that maps naturally
onto LLVM `i32` operations. The Z-machine (16-bit words, packed addresses,
result-variable encoding) stays on the classic path permanently.

Interception happens at the assembler boundary — the narrowest waist in the
compiler, through which *all* code (expressions, statements, veneer) flows:

```
expressc.c / states.c / veneer.c
        │  assembleg_instruction(AI), assemble_label_no(n)
        ▼
[capture buffer]           per routine: list of {INSTR ai | LABEL n} events
        ▼ assemble_routine_end()
   ┌── liftable? ──no──────────────► replay buffer through classic encoder
   ▼ yes
LLVM IR function  ──passes──►  optimized IR  ──lower──►  assembleg_* calls
                                     │ (unsupported construct? discard IR,
                                     ▼  replay original buffer instead)
                              classic encoder → holding area → transfer_routine_g
```

**Graceful per-routine fallback is the core principle.** Any routine the
lifter or lowerer cannot handle is emitted exactly as upstream would have.
The compiler is never wrong, only sometimes unoptimized. This makes the
project incrementally shippable.

### Lifting rules (Glulx instruction → LLVM IR)

| Glulx construct | LLVM IR |
|---|---|
| routine with n locals, k args | `define i32 @name(i32 %a1..%ak)`; every local an `alloca i32` (mem2reg promotes) |
| labels / branches | basic blocks; `jump` → `br`; `jeq/jne/jlt/...` → `icmp` + `br` |
| add/sub/mul/neg/bitand/... | wrapping `add/sub/mul/...` (no nsw/nuw — Glulx wraps) |
| shifts | Glulx shifts with count ≥ 32 produce 0; guard or lower via select |
| div/mod | opaque call unless divisor is a nonzero constant (Glulx /0 is a VM fault, LLVM UB — semantics must not leak) |
| return | `ret i32` |
| stack-mode operands (sp) | symbolic per-block value stack during lifting; values crossing branches become phis at the target label (every edge must arrive at the same depth, else fallback) |
| global variables | external `@glob_i = global i32`; load/store (calls conservatively clobber them — correct, since called routines can write globals) |
| call/callf/glk/print/IO/save etc. | opaque external function calls (side effects pin ordering) |
| loadw/storew/loadb/aload... | opaque calls initially; later, typed memory model for known arrays |
| operands with backpatch markers | symbolic constants that must survive optimization intact — either `ptrtoint` of named external globals (one per (marker,value) pair) or opaque pure calls; decided by experiment (risk: LLVM folding pointer comparisons of distinct externals) |

Anything not in the table ⇒ fallback for that routine.

### Optimization

`LLVMRunPasses()` (new pass manager via the C API) with an explicit pipeline,
roughly: `function(mem2reg,instcombine,simplifycfg,reassociate,gvn,dce,simplifycfg)`.
Explicitly **not** inlining or IPO initially — one IR function per routine,
module-at-a-time comes later.

### Lowering (IR → Glulx)

- Reverse instruction selection: each IR instruction maps to 1–2 Glulx ops;
  `icmp`+`br` pairs fuse into conditional branch opcodes.
- Value allocation: trivially assign each live SSA value a Glulx local slot
  (Glulx allows plenty of locals); reuse slots naively at first. Single-use
  values whose def immediately precedes their use can ride the VM stack.
- Emission goes through the existing `assembleg_*` API so
  `transfer_routine_g` and `bpatch.c` machinery is reused unchanged —
  labels, branch shortening, and markers all keep working.

### Implementation notes

- New module `src/llvm_codegen.c` (+ header), C11 like the rest of the code,
  using the **LLVM C API** (`llvm-c/Core.h`, `llvm-c/Transforms/PassBuilder.h`).
  Hooks in `asm.c` are a handful of lines.
- Enabled by a compiler option (e.g. `$LLVM=1` / `-B`-style switch), off by
  default until mature. Disabled when `debugfile_switch` is set (sequence
  points would be scrambled by optimization).
- Devshell (flake.nix) provides LLVM 21.1.8, clang, lldb, make.

## Milestones

- **M0** — baseline build of unmodified compiler from `src/`. ✅
- **M1** — capture + replay: buffer every routine's instruction stream, then
  replay it through the classic encoder. Output must be **byte-identical**
  to baseline for both targets. Proves the interception seam. ✅
  (`tests/run-m1.sh`)
- **M2** — lifting + IR dump: `$LLVM=2` writes each routine's verified IR to
  `inform6-llvm-dump.ll` and reports bails. No output change. ✅
  Lift rate on a full library game (cloak.inf): 366/548 routines. Known
  remaining bail causes: values live on the VM stack across branches
  (condition-to-value idioms; needs block-entry stack modeling with phis,
  ~50 routines) and stack-vararg functions (`_vararg_count`, mostly the
  infglk wrappers, ~130 routines — genuinely unmodelable, permanent
  fallback, no optimization value anyway).
- **M3** — full round-trip for simple routines (arith, locals, branches,
  calls), fallback for the rest. Story files must run correctly. ✅
  (`tests/run-m3.sh`; transcript-identical on cloak.inf with 360/548
  routines optimized.) Implementation notes:
  - `$LLVM` levels were re-cut: 0 classic, 1 capture/replay only (the M1
    byte-identity gate), 2 full pipeline (default), 3 = 2 + IR dump.
  - The lowerer (`src/llvm_lower.c`) runs in two phases: classify/validate
    (no side effects, clean per-routine bail) then emit. Emission rewrites
    the capture buffer through `llvm_buffer_*` and asm.c replays it, so
    branch shortening and backpatching are reused unchanged.
  - Every live SSA value gets its own fresh local slot (naive but correct);
    the routine header's locals count is patched after the fact
    (`llvm_patch_routine_locals`). Phis are lowered with per-edge copies
    (staged when >1 phi to keep parallel-copy semantics), fused
    icmp+br pairs become native compare-branches, materialized i1 values
    live in slots as 0/1, and `smax/smin/umax/umin/abs` intrinsics (which
    instcombine likes to form) lower to compare+copy sequences.
  - Semantics guards in the lifter: div/mod only lift with a constant
    divisor (nonzero, not -1 — Glulx faults/overflows are not LLVM UB to
    exploit); shifts only with constant counts (counts >= 32 fold to the
    defined Glulx result); `@copyb`/`@copys` bail because byte/short
    copies read memory-mode operands at their own width, which the
    word-based `i6.deref` abstraction would misread.
  - `i6.sym` is marked `memory(none) nounwind willreturn speculatable`,
    so identical symbolic constants merge and dead ones vanish without
    ever folding their values.
  - Remaining lower-bail causes on cloak.inf (5 routines, all M4 work):
    "too many local slots" ×3 (WriteAfterEntry, Locale, EmptyTSub — the
    one-slot-per-SSA-value allocator overflows the 118-slot cap; needs
    slot reuse/liveness), "unsigned division" ×1 (LanguageNumber —
    instcombine turns a signed div/mod chain into udiv/urem, which have
    no Glulx opcode; lower as a divu helper or block the transform), and
    "narrow select" ×1 (InformLibrary.play — a select on an i8/i16 value
    from narrowed arithmetic; extend lowering to narrow widths or reject
    narrowing earlier).
- **M4** — lowering quality and coverage. ✅
  The motivating measurement (`tests/life.inf`, `make bench` — Game of
  Life, 64x48, 500 generations) showed the optimized build ~30% *slower*
  than classic, and the cause was instruction count: an interpreter pays
  a fixed fetch/decode/dispatch cost per instruction, so instruction
  count is effectively the whole cost model — "more but simpler ops"
  only wins on hardware with pipelining. The naive one-slot-per-SSA-value
  lowering spent LLVM's IR-level wins back on copies (a full dispatch
  cycle each) that on hardware would be near-free register moves.
  `make bench` is now at parity (means within noise, ~875 ms both ways;
  the hot routine lowers to 63 instructions vs 61 classic). What it took,
  all in the lowerer's representation-assignment phase (`llvm_lower.c`):
  - **Direct operands** (`VK_GLOBAL`): a load of an i6 global none of
    whose uses can observe an intervening write (stores to that global,
    opaque calls) resolves as a `GLOBALVAR_OT` operand at each use — no
    copy, no slot.
  - **Operand fusion** (`VK_STACK`): a value produced by one emitted
    instruction and consumed exactly once, unconditionally, later in the
    same block rides the VM stack (`stackptr` operands). A per-block
    simulation of the pending stack (driven by per-instruction "readset"
    models of what each emission pops/reads) verifies LIFO pairing and
    unfuses anything that doesn't line up.
  - **Slot reuse**: values whose live range sits inside one block share
    a linear-scan pool; only phis and block-crossing values get dedicated
    slots. Reuse requires the previous occupant to die strictly before
    the new def, so multi-instruction emissions that write before reading
    can't clobber their own operands.
  - **Phi coalescing**: a value (or merge phi) whose only use feeds a phi
    takes over the phi's slot when nothing still reads the slot's old
    tenant, eliding the edge copy (`add x 1 -> x` loop latches). Slot
    tenancy is checked, not value identity, so chained coalescing stays
    sound. Phi-copy staging is only emitted on edges that actually have
    a parallel-copy hazard.
  - **Select-arm sinking**: instcombine/simplifycfg speculate cheap arm
    computations into selects — right for hardware, wrong here. A pure
    single-use arm is emitted inside the select's branch diamond, on the
    path that needs it (reproducing classic codegen's diamonds); with
    both a phi use and a select-arm use, the arm gets a rematerialized
    clone and the original folds into the phi's edge copy, undoing GVN's
    merge of loop increments with wraparound selects.
  - **Branch shape**: conditional branches invert to fall through when
    the then-block is next in layout; lone `ret 0`/`ret 1` blocks are
    reached via the Glulx -3/-4 "branch means return" encodings (their
    bodies aren't even emitted) and other constant returns are inlined
    into gotos; a switch case targeting the fallthrough block is tested
    last, inverted.
  Coverage, same milestone:
  - The lifter models **VM-stack values crossing branches** (Inform's
    condition-to-value idioms): each label records its entry-stack shape;
    the first edge fixes the depth and creates phis, later edges must
    match or the routine bails. `@tailcall` peels its stack arguments
    like `call`/`glk` (glulxercise caught the miscompile when the old
    "empty stack at noreturn" bail was lifted).
  - All five M3 lower-bail causes are gone, plus two found since:
    slot reuse fixed "too many local slots" (and `MAX_LOWER_SLOTS` is
    250 now — the encodings go much higher); `udiv`/`urem` by constants
    lower via the halving trick (safe signed division on halved values
    plus one unsigned-compare correction); loads of select-of-pointers
    (simplifycfg's conditional-load merge) are rewritten back into
    selects of loads before lowering; `llvm.fshl/fshr` (rotates) lower
    as shift/shift/or; and the module datalayout says `n32` so
    instcombine stops narrowing i32 arithmetic to widths Glulx can't
    express.
  - Cloak of Darkness: **412 of 548 routines optimized** (was 360), zero
    lower-bails; 126 of the 136 remaining lift-bails are the stack-vararg
    infglk wrappers (permanent fallback, no optimization value anyway).
  Not done, deliberately: glk dispatch semantics and a typed memory model
  for `loadw`/`storew` etc. stay opaque calls (they pin ordering and are
  correct; making them transparent is an optimization-scope question for
  after M5), and `i6.sym` hardening stayed a non-issue — symbolic
  constants are opaque pure calls, which cannot fold.
- **M5** — validation: compile a real corpus both ways (e.g. library demos),
  run under an interpreter (glulxe), compare transcripts; measure code size
  and instruction counts.

## Verification

- M1 gate: `cmp` of story files (classic vs capture-replay) — must be equal.
  (`tests/run-m1.sh`, using `$LLVM=1`.)
- M3+ gate: interpreter transcript equality on a test corpus, since
  optimized code is intentionally different bytes. (`tests/run-m3.sh`,
  running both builds under glulxe/cheapglk from the devshell.)
- `tests/` holds `.inf` sources; larger corpus TBD (Inform library + example
  games) once the round-trip lands.
- Compliance tests: `tests/veneer.inf` (every veneer routine the Glulx
  compiler can emit, layout-independent output) and `tests/glulxercise.inf`
  (Plotkin's Glulx unit test; self-checking, gated on its own pass/fail
  output since some checks print heap addresses/memory sizes). Known
  benign glulxercise failures under optimization: `@catch` token checks
  (tokens are stack addresses; optimized frames are larger) and the
  `jumpabs` test (jumps to `routine+5`, executing another routine's body
  in its own frame — unsound under any per-routine optimization).
- Custom `@"..."` opcodes (opcode-by-number inline assembly, used heavily
  by glulxercise) live in a single static in `asm.c` that each parse
  overwrites; the capture buffer snapshots the opcode's fields per event
  and restores them at replay (found by glulxercise, which otherwise
  miscompiled under `$LLVM>=1`).
- Debugging aid: `I6_LLVM_LIMIT=<n>` lowers only the first n lifted
  routines, for bisecting a misbehaving routine in a big game.
