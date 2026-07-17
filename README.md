# inform6-llvm

An Inform 6 compiler with an LLVM-based code generator for the Glulx target.
Instead of encoding bytecode directly as it parses, routines are lifted to
LLVM IR, run through LLVM's optimization passes, and lowered back to Glulx
bytecode. LLVM is used as a mid-level optimizer; the result remains a normal
Glulx story file interpreted by existing VMs.

A fork of the upstream
[Inform 6 compiler](https://github.com/DavidKinder/Inform6), with the
sources moved under `./src/` and the LLVM modules added alongside them.

## Building

A Nix devshell provides the toolchain (LLVM 21, clang, make):

```
nix develop
make
```

The devshell also provides `glulxe` for behavioral tests and
`glulxe-counted`, a separately patched reference interpreter which reports
`GLULXE_INSTRUCTION_COUNT=<n>` on stderr. Pass `--opcode-histogram` to also
emit `GLULXE_OPCODE_COUNT_0x<opcode>=<n>` records for every executed opcode.
`make bench` uses it to report deterministic classic-versus-LLVM dynamic
instruction totals and writes their opcode comparison to
`tests/life.opcodes.tsv` alongside timing medians. Set `BENCH_RUNS` to change
the default five timing runs. Older counted interpreters which do not support
histograms still provide total counts but skip the opcode TSV.

LLVM is optional: the Makefile detects it via `llvm-config` and, when it
isn't found (or with `make WITH_LLVM=0`), builds `src/llvm_stub.c` in
place of the pipeline. A stub build compiles everything classically —
output is byte-identical to `$LLVM=0` — and prints a note when
optimization is requested. The Visual Studio project
(`visual_studio/inform6.vcxproj`, built by the Windows CI workflow)
always uses the stub.

## Usage

Same as upstream Inform 6. The LLVM pipeline is controlled by the `$LLVM`
option (Glulx only) and is **on by default**:

```
./inform6-llvm -G game.inf game.ulx              # LLVM pipeline (default)
./inform6-llvm -G '$LLVM=0' game.inf game.ulx    # classic upstream codegen
./inform6-llvm -G '$LLVM=1' game.inf game.ulx    # capture/replay only (byte-identical)
./inform6-llvm -G '$LLVM=3' game.inf game.ulx    # + dump IR to inform6-llvm-dump.ll
```

With `$LLVM=0` eligible code uses the classic path. `$LLVM=1` routes each
eligible Glulx routine through the capture buffer but replays it without
optimizing; debug-file builds, asterisk-traced routines, and Infix builds
bypass capture. Captured output stays byte-identical to `$LLVM=0`. The default,
`$LLVM=2`, is the full pipeline; `$LLVM=3` additionally dumps each routine's IR
before and after optimization and reports routines the pipeline could not
handle.

## Architecture

Inform's front end emits `assembly_instruction` records directly to the
classic assembler. In Glulx mode this fork intercepts that assembler boundary
and records each routine as instruction and label events. At routine end:

1. `src/llvm_codegen.c` lifts the captured stream into one LLVM `i32` function.
2. LLVM runs `mem2reg`, `instcombine`, `simplifycfg`, `reassociate`, LICM, GVN,
   DCE, and a final CFG simplification.
3. `src/llvm_lower.c` validates the optimized shape, assigns Glulx
   representations, chooses block layout, and constructs a block/edge emission
   plan before lowering it back into the capture buffer.
4. `src/asm.c` replays either the optimized or original stream through the
   classic encoder, preserving branch shortening, labels, and backpatching.

Unsupported routines fall back independently to classic code generation. The
Z-machine target always uses the classic path.

Locals begin as LLVM allocas and are promoted by `mem2reg`; Glulx branches form
the CFG, VM-stack values crossing joins become phis, globals become external
`i32` globals, and backpatchable symbols remain opaque `i6.sym` calls. Calls,
Glk, stream operations, and most memory operations remain opaque where LLVM
cannot safely model VM effects. Division and shifts are lifted directly only
when their LLVM semantics match Glulx.

The lowerer is designed around interpreter costs rather than native register
allocation. Values can resolve directly as global operands, ride the VM stack
when consumed once in LIFO order, or occupy reusable local slots. It also
coalesces phis, sinks speculated select arms, reconstructs short-circuit
conditions, folds stores and returns, and plans fallthroughs, inline returns,
and phi-copy stubs before emitting branches. `loop-rotate` is deliberately
excluded because it increased dynamic work in benchmarks.

## Status

The full lift/optimize/lower pipeline is enabled by default and has focused
behavioral, compliance, static instruction, and dynamic instruction-count
tests. Unsupported instructions, fixed resource limits, debug output, traced
routines, or unfamiliar post-LLVM shapes can cause per-routine fallback.

Dynamic dispatch count is the primary performance signal, but it is not a
complete cost model. Opcode handlers, operand modes, memory behavior, Glk work,
host branch prediction, and accelerated routines have different costs.
Deterministic dispatch totals are used as the main regression signal and are
reported alongside repeated wall-clock timings. See [REVIEW.md](REVIEW.md) for
current measurements, detailed findings, known limitations, and the
optimization roadmap.

## Tests

```
make test         # build and run the test suite (Glulx only)
make bench        # run the Game of Life benchmark (tests/run-life.sh)
make clean-tests  # remove test artifacts (.ulx, logs, IR dumps)
```

`run-m1.sh` checks that `$LLVM=1` capture/replay output is byte-identical to
classic Glulx output. `run-opt.sh` requires complete lowering of a focused
fixture and enforces aggregate, per-routine static, and optional dynamic
instruction ceilings. Set `REQUIRE_LLVM=1`, `REQUIRE_GLULXE=1`, and
`REQUIRE_GLULXE_COUNTED=1` for a strict environment. `run-m3.sh` compiles each
test both ways, runs both stories under `glulxe`, and requires identical
behavior.

Two compliance tests exercise the API surface beyond ordinary game code:

- `veneer.inf` calls every veneer routine the Glulx compiler can emit
  (property/class machinery, strict-mode checks, print rules, class
  messages, `glk()`, dynamic strings, actions) with layout-independent
  output, compared transcript-for-transcript.
- `glulxercise.inf` is Andrew Plotkin's Glulx interpreter unit test
  (public domain, from <https://eblong.com/zarf/glulx/>), driven by
  `glulxercise.walk`. It is self-checking, so instead of a transcript
  diff the classic build must pass every check and the LLVM build may fail only
  the exact known layout-sensitive checks documented in `run-m3.sh` and
  [REVIEW.md](REVIEW.md).

`life.inf` is a Game of Life benchmark: 500 generations on a 64x48
torus, self-timed via `glk_current_time`. On an interpreter with Glk
graphics (Gargoyle, WinGlulxe, ...) it draws each cell as a filled square in a
graphics window; under the headless CheapGlk glulxe it falls back to text
rendering. `run-life.sh` (also `make bench`) compiles it both ways, requires
identical simulation output, alternates execution order, and reports timing
median, minimum, maximum, and dynamic instruction totals. It runs each version
five times by default; set `BENCH_RUNS` to change this.

`cloak.inf` (a full library game) needs the Inform 6 standard library.
Inside the devshell it is provided automatically (the `inform6lib-src`
flake input, exported as `INFORM6_LIB`); outside, the scripts fall back
to a clone at `tests/lib`:

```
git clone --depth 1 https://gitlab.com/DavidGriffith/inform6lib.git tests/lib
```

(The test scripts pass `+language_name=english` because the compiler's
default language include is capitalized "English", while the library ships
`english.h` — which matters on a case-sensitive filesystem.)

## TODO

- Rework the test corpus around
  [erkyrath/inform6-testing](https://github.com/erkyrath/inform6-testing)
  (the upstream compiler's regression suite): compile its tests both ways
  and gate on behavior, replacing the bespoke `cloak.inf` + `tests/lib`
  setup. The small local tests (`hello`, `torture`, `m3`, `veneer`,
  `glulxercise`) stay as quick gates.
- Add Linux CI which requires the real LLVM pipeline, `glulxe`, and
  `glulxe-counted` instead of permitting local dependency skips.
- Use dynamic counts to reduce interpreted work in the optimization benchmark;
  the focused fixture remains above classic even though Life is now below it.
- Validate a larger game corpus both ways, recording behavior, optimization
  coverage, story size, static instructions, and dynamic instructions.
- Resolve the correctness and LLVM-effect-model findings in the review.
- Consider typed memory and fuller Glk modeling only after corpus validation
  shows that the additional optimization scope is worthwhile.
- Consider lifting custom `@"..."` opcodes as opaque operations instead
  of bailing the whole routine (low priority; rare outside test suites).
