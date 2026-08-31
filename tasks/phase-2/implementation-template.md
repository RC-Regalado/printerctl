# Codex Implementation Task Template — Phase 2

## Milestone

`MXX — <name>`

## Objective

Implement only approved milestone scope.

## Required reading

- `README.md`
- `docs/phase-2-roadmap.md`
- `docs/phase-2-architecture.md`
- `docs/phase-2-contracts.md`
- `docs/phase-2-testing.md`
- milestone design file
- project `Agents.md`
- project safety/testing docs

## Before coding

Report:
1. files/modules expected to change;
2. current dependency path;
3. proposed dependency path;
4. safety-sensitive behavior touched;
5. protocol changes;
6. test plan;
7. conflicts with phase contracts.

If out-of-scope architecture is required, report blocker instead of silently expanding scope.

## Global constraints

- no queue/jobs/history/TUI/Web/multi-printer;
- no automatic homing/movement/heating/extrusion;
- no automatic M112/M410;
- no auto-resume;
- no direct serial fallback for migrated commands;
- preserve R6 TX barrier;
- STATUS stays snapshot-only;
- hardware safety enforced behind daemon boundary.

## Completion report

Provide:
- implementation summary;
- changed dependency graph;
- protocol additions;
- compatibility changes;
- test evidence;
- sanitizer evidence;
- safety evidence;
- technical debt;
- milestone verdict.
