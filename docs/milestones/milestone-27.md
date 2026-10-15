# Milestone 27 — Macro Domain & Safety Contract

## Status

In progress as of 2026-09-07.

## Summary

M27 introduces a pure macro domain module:

- typed macro definitions;
- explicit categories;
- explicit capability mask;
- typed MOVE steps;
- complete-definition validation;
- deterministic built-in registry metadata.

No physical macro execution exists in M27.

## Files changed

- `include/printerctl/macro.h`
- `src/macro/macro.c`
- `tests/test_macro.c`
- `CMakeLists.txt`
- `tests/CMakeLists.txt`
- `docs/phase-3-roadmap.md`
- `docs/design/m27-macro-domain.md`
- `docs/milestones/milestone-27.md`
- `docs/phase-2-roadmap.md`
- `docs/roadmap.md`
- `docs/testing.md`
- `README.md`

## Decisions

- Built-in planned macros are represented as registry placeholders because
  machine geometry is not yet authoritative.
- Complete executable validation rejects empty macros.
- Movement steps carry explicit axes, coordinates/displacement and feedrate.
- The future executor must use deterministic modal semantics instead of relying
  on inherited G90/G91/feedrate state.

## Safety impact

Zero physical effects.

The module does not link to daemon runtime, socket, PLP, CLI, print engine,
printer, serial, Marlin or G-code streaming.

## Remaining work

Future milestones must add execution and protocol/CLI integration without
turning macros into arbitrary G-code submission.
