# inform6-llvm

An Inform 6 compiler with an LLVM-based code generator for the Glulx target.
Instead of encoding bytecode directly as it parses, routines are lifted to
LLVM IR, run through LLVM's optimization passes, and lowered back to Glulx
bytecode. See [DESIGN.md](DESIGN.md) for the architecture and milestones.

Based on the upstream [Inform 6 compiler](https://github.com/DavidKinder/Inform6)
(a convenience clone lives in `./inform6/`; the working sources are copied
to `./src/`).

## Building

A Nix devshell provides the toolchain (LLVM 21, clang, make):

```
nix develop
make
```

## Usage

Same as upstream Inform 6. The LLVM pipeline is controlled by the `$LLVM`
option (Glulx only) and is **on by default**:

```
./inform6-llvm -G game.inf game.ulx              # LLVM pipeline (default)
./inform6-llvm -G '$LLVM=0' game.inf game.ulx    # classic upstream codegen
./inform6-llvm -G '$LLVM=2' game.inf game.ulx    # + dump IR to inform6-llvm-dump.ll
```

With `$LLVM=0` the compiler behaves exactly like upstream.

## Status

- **M1 (done):** with `$LLVM=1`, every routine's instruction stream is
  captured and replayed through the classic encoder — output is
  byte-identical to upstream. This proves the interception seam.
- **M2 (done):** `$LLVM=2` additionally lifts each routine to verified
  LLVM IR and dumps it to `inform6-llvm-dump.ll` (~67% of a full library
  game lifts; the rest fall back). Output is still byte-identical.
- **Next (M3):** run LLVM passes over the IR and lower it back to Glulx.

## Tests

```
cd tests && ./run-m1.sh
```

`cloak.inf` (a full library game) needs the Inform 6 standard library at
`tests/lib`:

```
git clone --depth 1 https://gitlab.com/DavidGriffith/inform6lib.git tests/lib
```

(The test script passes `+language_name=english` because the compiler's
default language include is capitalized "English", while the library ships
`english.h` — which matters on a case-sensitive filesystem.)
