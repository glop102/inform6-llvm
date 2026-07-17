# Repository Guidance

## Documentation

- Keep `README.md` limited to durable information that remains useful across
  optimization changes: project purpose, build and usage instructions,
  architecture, test commands, and stable limitations.
- Do not put current benchmark counts, timings, percentages, opcode deltas,
  commit hashes, temporary migration advice, or other quickly stale results in
  `README.md`.
- Put measured performance results, investigation history, current regression
  baselines, detailed findings, and the optimization roadmap in `REVIEW.md`.
- Keep exact test expectations in the test scripts rather than duplicating them
  in `README.md`.
- When updating documentation, distinguish current behavior from historical
  measurements explicitly and verify source line references after code moves.

## Optimization Direction

- Prefer generic, deterministic optimization rules and target cost models.
- Prefer pathwise non-worsening CFG transformations when branch frequency is
  unknown. Do not treat operand shape as evidence of runtime likelihood.

