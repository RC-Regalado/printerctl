---
title: Milestone 25 — Remove Legacy Serial Ownership
milestone: 25
status: implemented
date: 2026-09-03
tags: [printerctl, milestone, phase-2, cli, build, safety]
---

# Milestone 25 — Remove Legacy Serial Ownership

Source of truth: `Agents.md`, `docs/architecture.md`, `docs/phase-2-roadmap.md`.
Design details: `docs/design/m25-remove-legacy-serial.md`.

## 1. Technical summary

M25's goal was to make the daemon boundary mandatory: `printerctl`
(the CLI) must retain no path to serial open/read/write, printer
ownership, direct Marlin exchange, or host `print_engine` execution.

Investigation found this was already fully true, with zero source
changes required: M20 (read-only queries), M21 (send/home/sd
controls), M22 (`print FILE`) and M23 (pause/resume/cancel) each moved
their last remaining direct-serial CLI code path onto a PLP/1 client
call as they landed, and nothing since has reintroduced one.
`CMakeLists.txt`'s `printerctl_cli` target links only `printerctl_
config`, `printerctl_logging`, `printerctl_gcode` (dry-run parsing
only), `printerctl_record` (session/marker files) and `printerctl_
client` — none of which reach `printerctl_serial`, `printerctl_
printer`, `printerctl_marlin` or `printerctl_core` (print_engine), and
`src/cli/cli.c`/`src/main.c` contain no includes of or calls into any
of those modules.

What M25 actually needed, and did not have before this milestone, was
the design doc's explicit "build graph gate": a *deterministic* check
that would catch a forbidden dependency if one were ever reintroduced,
rather than relying on the fact holding by accident of nobody having
broken it yet. Added `printerctl_build_boundary_tests`
(`tests/test_build_boundaries.c`), a new permanent `ctest` that runs
`nm` on the actual linked `printerctl` executable and fails if any
`printerctl_serial_*`/`printerctl_printer_*`/`printerctl_marlin_*`/
`printerctl_print_engine_*` symbol appears in it.

## 2. Files changed

- `tests/test_build_boundaries.c` (new) — the build/dependency gate.
- `tests/CMakeLists.txt` — registers `printerctl_build_boundary_tests`,
  depending only on the `printerctl` executable target (via
  `$<TARGET_FILE:printerctl>`) and the C runtime, deliberately linking
  no `printerctl_*` library itself.
- `docs/phase-2-roadmap.md`, `docs/design/m25-remove-legacy-serial.md`,
  `docs/milestones/milestone-25.md` — this milestone's documentation.

No other source file changed: the removal this milestone targets was
already complete before it started.

## 3. Verification

```bash
cmake -S . -B build-verify-gcc -DCMAKE_C_COMPILER=gcc -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-verify-gcc
ctest --test-dir build-verify-gcc --output-on-failure

cmake -S . -B build-verify-clang -DCMAKE_C_COMPILER=clang
cmake --build build-verify-clang
ctest --test-dir build-verify-clang --output-on-failure
```

Result: both compilers warning-free; 19/19 test binaries pass on each
(18 pre-existing + the new `printerctl_build_boundary_tests`).

ASan+UBSan (`-fsanitize=address,undefined -g -O1`, `build-asan`):
19/19 pass, no sanitizer reports.

Negative-control sanity check (not part of `ctest`, run manually to
confirm the gate has teeth): `nm build/printerctld | grep -cE
'printerctl_(serial|printer|marlin)_|printerctl_print_engine_'`
reports 31 matches — `printerctld` legitimately owns all four modules,
so the same pattern that must find zero matches in `printerctl` is
confirmed to find real matches when they genuinely exist.

## 4. Risks found

### Blocking

None.

### Non-blocking

- The new test shells out to `nm` via `popen()`. If `nm` were ever
  unavailable in a build environment, the test would fail loudly on
  its own "nm runs successfully" assertion rather than silently
  reporting a false pass — this was a deliberate design choice (see
  Decisions below), but it does mean the check has an external tool
  dependency `ctest`'s other tests do not.

### Future

- M26 (Compatibility & Fault Validation) is the final Phase 2
  milestone; it is unaffected by this one.

## 5. Decisions taken

- Checked the actual linked `printerctl` binary (via `nm`) rather than
  walking `CMakeLists.txt`'s `target_link_libraries()` graph
  programmatically. A static library only contributes object code to
  the final executable if something in it is actually referenced, so
  this is a physical fact about the shipped artifact — a link-graph
  audit would only prove today's `CMakeLists.txt` text looks correct,
  not that the resulting binary is actually clean.
- Added an explicit "sanity" assertion (the binary must contain a
  known-present symbol, `printerctl_cli_run`) before trusting an
  absence-of-forbidden-symbols result, so a broken or missing `nm`
  cannot produce a silent false pass.
- Verified the check's discriminating power against a real
  negative case (`printerctld`, which legitimately links all four
  forbidden modules) rather than trusting an empty-result pass at face
  value.
- Made no source changes beyond the new test, since none were needed:
  resisted the temptation to "clean up" already-clean code or add
  defensive restructuring the design doc did not ask for.

## 6. Next steps

Request review of M25 before advancing to M26 (Compatibility & Fault
Validation) — the final milestone of Phase 2.
