# inform6-llvm

An Inform 6 compiler with an LLVM-based code generator for the Glulx target.
Instead of encoding bytecode directly as it parses, routines are lifted to
LLVM IR, run through LLVM's optimization passes, and lowered back to Glulx
bytecode. See [DESIGN.md](DESIGN.md) for the architecture and milestones.

A fork of the upstream
[Inform 6 compiler](https://github.com/DavidKinder/Inform6), with the
sources moved under `./src/` and the LLVM modules added alongside them.

## Building

A Nix devshell provides the toolchain (LLVM 21, clang, make):

```
nix develop
make
```

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

With `$LLVM=0` the compiler behaves exactly like upstream. `$LLVM=1`
routes every routine through the capture buffer but replays it without
optimizing (for testing the seam — output stays byte-identical). The
default, `$LLVM=2`, is the full pipeline; `$LLVM=3` additionally dumps
each routine's IR before and after optimization and reports routines the
pipeline could not handle.

## Status

- **M1 (done):** with `$LLVM=1`, every routine's instruction stream is
  captured and replayed through the classic encoder — output is
  byte-identical to upstream. This proves the interception seam.
- **M2 (done):** each routine is lifted to verified LLVM IR (~67% of a
  full library game lifts; the rest fall back).
- **M3 (done):** full round trip. Lifted routines are optimized by LLVM
  (`mem2reg`, `instcombine`, `simplifycfg`, `reassociate`, `gvn`, `dce`)
  and lowered back to Glulx bytecode; anything the lowerer can't handle
  falls back to the classic encoding per routine. On Cloak of Darkness,
  360 of 548 routines come out optimized and the interpreter transcript
  is identical to the classic build's.
- **M4 (next):** coverage — model VM-stack values crossing branches (phi
  nodes), glk dispatch, memory ops, byte-width accesses, and a smarter
  slot allocator for lowered routines.
- **M5:** validation at scale — compile a real corpus both ways, compare
  interpreter transcripts, measure code size and instruction counts.

See [DESIGN.md](DESIGN.md) for the full milestone definitions.

## Tests

```
make test         # build and run the test suite (Glulx only)
make clean-tests  # remove test artifacts (.ulx, logs, IR dumps)
```

`run-m1.sh` checks that `$LLVM=1` (capture/replay) output is
byte-identical to classic output. `run-m3.sh` compiles each test both
ways, runs both story files under glulxe (provided by the devshell), and
requires identical transcripts.

Two compliance tests exercise the API surface beyond ordinary game code:

- `veneer.inf` calls every veneer routine the Glulx compiler can emit
  (property/class machinery, strict-mode checks, print rules, class
  messages, `glk()`, dynamic strings, actions) with layout-independent
  output, compared transcript-for-transcript.
- `glulxercise.inf` is Andrew Plotkin's Glulx interpreter unit test
  (public domain, from <https://eblong.com/zarf/glulx/>), driven by
  `glulxercise.walk`. It is self-checking, so instead of a transcript
  diff the classic build must pass every check and the LLVM build may
  fail only the known layout-sensitive ones (`@catch` tokens are stack
  addresses; `jumpabs` into another routine's body) — see the comment
  in `run-m3.sh`.

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
- M4 coverage work and M5 validation at scale — see [Status](#status)
  and [DESIGN.md](DESIGN.md).
- Consider lifting custom `@"..."` opcodes as opaque operations instead
  of bailing the whole routine (low priority; rare outside test suites).
