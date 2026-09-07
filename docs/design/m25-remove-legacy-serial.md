# M25 Design — Remove Legacy Serial Ownership

## Objective

Make the daemon boundary mandatory for real printer operations.

## Required removal from printerctl

- serial open/write/read;
- printer ownership;
- Marlin exchange;
- host print_engine execution.

Local G-code parsing may remain only for dry-run.

## Build graph gate

Add deterministic check failing if CLI regains forbidden hardware dependencies.

## Failure

If daemon unavailable:

```text
migrated command -> explicit failure
```

Never:

```text
migrated command -> silent direct-serial fallback
```

## As-built notes

- Investigation before writing any code found all four forbidden
  removals already accomplished, as a side effect of M20 (queries),
  M21 (send/home/sd), M22 (print) and M23 (pause/resume/cancel) each
  moving their last direct-serial CLI code path onto a PLP client call:
  `CMakeLists.txt`'s `printerctl_cli` target links only
  `printerctl_config`, `printerctl_logging`, `printerctl_gcode`,
  `printerctl_record` and `printerctl_client` — transitively, none of
  those reach `printerctl_serial`, `printerctl_printer`,
  `printerctl_marlin` or `printerctl_core`. `src/cli/cli.c` and
  `src/main.c` contain zero `#include`s of `serial.h`/`printer.h`/
  `marlin.h`/`print_engine.h` and zero calls to any
  `printerctl_serial_*`/`printerctl_printer_*`/`printerctl_marlin_*`/
  `printerctl_print_engine_*` symbol.
- This milestone's actual deliverable was the "build graph gate" this
  design doc calls for, since nothing before M25 made that removal
  *durable* against a future regression. Added `tests/test_build_
  boundaries.c` / `printerctl_build_boundary_tests`: it runs `nm` on
  the real, linked `printerctl` executable (via `$<TARGET_FILE:
  printerctl>`, the same compile-definition pattern `printerctld_tests`
  already uses for binary paths) and fails if any of the four forbidden
  symbol-name prefixes appear in it.
- Deliberately checks the *linked binary*, not `CMakeLists.txt`'s
  `target_link_libraries()` graph: a static library only contributes
  object code to the final executable if something actually references
  a symbol from it, so an empty `nm` grep is a physical fact about the
  shipped binary, not a claim about a specific CMake line staying
  written a certain way. Verified the check has teeth (is not
  vacuously passing) by confirming `nm` on `printerctld` — which
  legitimately owns `print_engine`/`printer`/`serial`/`marlin` — trips
  the same pattern with dozens of matches.
- No test previously exercised "daemon unavailable → explicit failure,
  never silent serial fallback" as a build-level guarantee; it was
  already covered behaviorally (extensively, across the M20-M23 test
  suites: every migrated command reports a communication error with no
  daemon running). M25 adds the complementary structural guarantee:
  since no serial code is linked into `printerctl` at all, no fallback
  code path can exist to silently take, regardless of what a future
  patch might attempt.
