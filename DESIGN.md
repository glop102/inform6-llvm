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
| stack-mode operands (sp) | symbolic per-block value stack during lifting; must balance at block boundaries, else fallback |
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
  calls), fallback for the rest. Story files must run correctly.
- **M4** — coverage: stack-balanced expression temporaries, glk dispatch,
  memory ops, symbolic-constant hardening.
- **M5** — validation: compile a real corpus both ways (e.g. library demos),
  run under an interpreter (glulxe), compare transcripts; measure code size
  and instruction counts.

## Verification

- M1 gate: `cmp` of story files (classic vs capture-replay) — must be equal.
- M3+ gate: interpreter transcript equality on a test corpus, since
  optimized code is intentionally different bytes.
- `tests/` holds `.inf` sources; larger corpus TBD (Inform library + example
  games) once the round-trip lands.
