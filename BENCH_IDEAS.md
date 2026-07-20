# Benchmark Ideas: Quantifying the Inlining Opportunity

Companion to `EXPLAIN.md`. The question these benchmarks serve: how much
dynamic win is available from selective inlining of small hot helper
routines (call/return dispatch elimination plus call-barrier
dissolution), and do larger games look like cloak or like something
worse? See "Would full IR coverage unlock optimization wins?" in
`EXPLAIN.md` for why inlining — not construct coverage — is the
candidate for the next win tier.

## Step zero (free): ceiling estimate from existing data

No new benchmark is needed for the first number. The attribution data
already collected on every benchmark run has per-routine `self_ops` and
`call_count`; every call pair costs two dispatches (`callfi`/`call` +
`return`) that inlining deletes outright, plus argument setup that it
mostly deletes too. Summing over the small hot leaves in the existing
cloak profile (Z__Region 2,776 calls, Unsigned__Compare 1,861, CP__Tab
1,766, RT__ChLDB 1,207, ...) puts the ceiling in the 5-10% band before
counting second-order wins.

Outline of the script (reuses `tests/attrib.py`):

```python
# ceiling.py ASM_LOG PROFILE [MAX_OPS_PER_CALL=12]
# Estimate inlining headroom from an existing profiled run.
import sys
sys.path.insert(0, "tests")
from attrib import named_self_ops

named, dropped = named_self_ops(sys.argv[1], sys.argv[2])
thresh = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0
total = sum(s for s, _ in named.values()) + dropped

rows = []
for name, (self_ops, calls) in named.items():
    if calls == 0:
        continue
    per_call = self_ops / calls
    if per_call <= thresh:              # "small helper" proxy
        rows.append((calls * 2, calls, per_call, name))

ceiling = sum(r[0] for r in rows)
print(f"call/return dispatch ceiling: {ceiling} "
      f"({100.0 * ceiling / total:.2f}% of {total})")
for saved, calls, per_call, name in sorted(rows, reverse=True)[:20]:
    print(f"  {saved:>7}  {name} ({calls} calls, {per_call:.1f} ops/call)")
```

Caveats to keep in mind when reading its output:

- Counts only the call+return pair. Argument setup savings and
  barrier-dissolution wins (GVN/LICM across what used to be an opaque
  call) come on top; frame-teardown cost inside the interpreter's call
  handler is not a counted dispatch at all, so wall-clock gains may
  exceed the dispatch estimate.
- `self_ops / calls` is a proxy for "small helper" — it conflates a
  small body with a loopy body that averages cheap. Real candidate
  selection needs emitted-size per routine (the compiler already knows
  it) rather than dynamic average.
- Recursive routines and C0 policy-classic routines (`CA__Pr`) are not
  inlining candidates; the script does not exclude them, so skim the
  top rows.

## Group 1: call-overhead rulers (classics)

Historic call-stress microbenchmarks. Benefit: they calibrate the
per-call dispatch constant precisely, giving the multiplier for
back-of-envelope estimates on any profile. Limitation: deep recursion
cannot be flattened, so they overstate what inlining recovers — use
them as rulers, not as win predictions.

- **Recursive Fibonacci** — the traditional per-call-cost meter.
- **Towers of Hanoi** — recursion classic, near-zero body work.
- **Takeuchi (tak)** — *the* LISP function-call benchmark;
  call-dominated to the point of absurdity.
- **Ackermann** — the historic call-stress test (keep arguments small;
  Glulx stack depth is finite).

## Group 2: helper-pattern kernels (realistic inlining wins)

These model the code shape the user's question is about: larger games
employing small helper functions. Benefit: they show what inlining
*realistically* recovers, and they become the focused regression gates
if an inlining pass is ever built.

- **Quicksort with `swap()`/`compare()` helpers** — the textbook case:
  tiny leaf helpers called O(n log n) times. Also a good slot-pressure
  test after inlining.
- **Sieve of Eratosthenes** (the old Byte benchmark) written with
  `is_marked()`/`mark()` helpers — doubles as an array/bounds lowering
  test in both strict and `-~S` modes.
- **Dhrystone (Inform 6 port)** — the historically perfect fit:
  procedure integration famously gamed Dhrystone in the 80s precisely
  because its Proc1..Proc8/Func1..Func3 structure is "small helpers
  called constantly." About a day of porting; would be the flagship
  number for the investigation.
- **Richards** (OS-simulation benchmark from the Smalltalk/V8 lineage)
  — object/property-dispatch heavy. Written with Inform objects and
  property calls it hammers `CA__Pr`, which makes it doubly useful: it
  stress-tests the C0 policy-classic decision under a dispatch load
  cloak never generates. Stretch goal; port is bigger than Dhrystone.

## Group 3: real-game corpora (does "larger" change the picture?)

Benefit: cloak is one data point; these test whether real games at
scale still show zero fallbacks and the same helper-dominated profile.

- **Advent** (`advent.inf`, the 350-point Colossal Cave) — a genuinely
  larger library game than cloak, freely distributed with the Inform 6
  examples. Needs RNG determinism (below) because of dwarf movement.
  **DONE (2026-07-20):** `tests/adventBenchmark.nix` + `tests/advent.walk`,
  sourced from the pinned `inform6-testing` input (`Advent.inf` + bundled
  `i6lib-611`), start-to-victory walk, determinism via glulxe's built-in
  `--rngseed 1`. Dynamic: upstream 7,298,793 vs direct 6,942,464 (−4.88%).
  **Fallbacks are NOT zero here:** backend split is 492 direct / 6 classic /
  3 fallback — the first story to exercise the direct-IR fallback valve, so
  the "expectation: zero fallbacks" above does not hold at Advent's scale.
  Profile shape does match (helper/veneer-dominated: Parser__parse, Z__Region,
  RA__Pr are the hot movers).
- **Balances** and the other Nelson sample games — smaller, free,
  deterministic-friendly secondary points.
- **Dungeon** (mainframe Zork's Inform 6 port) — larger still, if a
  cleanly licensed source is pinned.
- **Synthetic parser-stress story** — one room, N objects with
  overlapping vocabulary, a script that references them; sweep N. This
  scales exactly the `Parser__parse`/`NounWord`/`Z__Region` paths that
  dominate real play, and is the most controllable proxy for "big game
  with lots going on."

## RNG determinism

Glulxe seeds its RNG from the clock. Two options, in preference order
seen so far:

- **`faketime`** — previously used in this project to effectively fix
  the seed: run the interpreter under a frozen timestamp and the seed
  is constant. Works today with unmodified interpreter builds; the
  benchmark/test apps run at `nix run` time, so wrapping the glulxe
  invocation is straightforward.
- **Seed knob in the patched interpreters** — `glulxe-counted` and
  `glulxe-profiled` are already patched in-repo; adding a
  `GLULXE_RNG_SEED` environment variable is a few lines and more robust
  than clock spoofing. Doing this would also permanently kill the known
  glulxercise classic self-check flake (documented in the workflow
  notes), which is caused by exactly this clock seeding.

## Sourcing policy: flake inputs, not vendoring

Official/third-party games should be pulled in as pinned flake inputs
(`flake = false` fetches of the upstream archives — IF Archive URLs
are stable, and GitHub mirrors exist for the Inform 6 example games),
not vendored into `stories/`. The repo already follows this pattern for
`inform6lib`. Benefits:

- Upstream provenance and license stay attached to the pin instead of a
  copied file.
- Updates are explicit lock-file events, matching the existing
  "upstream baseline drift is a deliberate compatibility event" policy.
- Removes the "new story files must be `git add`ed before nix sees
  them" footgun for these corpora.

**Cloak falls under this too**: `stories/cloak.inf` is vendored today
and should migrate to a flake input as part of this work, with the
vendored copy removed in a later cleanup once the input is pinned and
the tests point at it.

**Cloak migration — SKIPPED (2026-07-20).** No clean pinnable source
matches our build: Roger Firth's original site (firthworks.com) is dead,
and the one cloak in the `inform6-testing` corpus we pinned for Advent
(`cloak-metro84-v3test.inf`) is Z-machine-v3 only — grammar version 1 plus
inline `@split_window`/`@set_cursor`/`@print_obj` and `#IFV5` read/save/
restore — so it will not compile to Glulx and cannot drive our glulxe
harness (it also inlines the mInform library, so none of the standard
veneer routines the benchmark profiles exist). `stories/cloak.inf` stays
vendored. If revisited, the path is "vendor-as-pin": publish the current
byte-identical file to an owned repo and pin that, which keeps all pinned
cloak baselines put.

## Suggested sequencing

1. Run the step-zero ceiling script against the existing cloak profile
   (free; sets expectations).
2. Dhrystone port + quicksort-with-helpers as focused kernels — these
   become the inlining regression gates later.
3. ~~Advent via flake input with a seeded/faketime interpreter — the
   second real corpus point beside cloak; check its backend split and
   fallback count first (expectation: zero fallbacks, same profile
   shape).~~ **DONE (2026-07-20)** — pinned `inform6-testing` input,
   determinism via built-in `--rngseed 1` (no faketime needed); backend
   split 492/6/**3 fallback** (expectation of zero did not hold), profile
   shape matches, −4.88% dynamic. See the Advent bullet in Group 3.
4. Parser-stress story with swept N.
5. Richards as the stretch goal / C0-policy re-test.
