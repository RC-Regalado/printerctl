# M29 Independent Review — Macro Public Surface and PrusaSlicer Import

Scope: uncommitted working tree on `feat/macros` (M29 diff + new files listed in
`docs/milestones/milestone-29.md`). Reviewed against Agents.md §8-15,
`docs/design/m28-macro-execution.md`, `docs/phase-3-roadmap.md`,
`docs/design/m29-macro-public-surface.md`, `docs/design/protocol-v1.md` §20a.
No repo file other than this report was written.

## Verdict

**APPROVE WITH REQUIRED FIXES**

The protocol, daemon dispatch, ownership model and CLI are sound. Nothing in
M29 adds automatic motion, homing or heating; MACRO_RUN only forwards a stable
id and the worker still owns resolution, validation, exclusion and every
serial write. The fixes required before closing are all in the PrusaSlicer
importer (`src/config/prusa_import.c`). It can silently produce calibration
bounds from data PrusaSlicer itself would ignore (commented `.ini` lines,
G-code body comments, binary G-code), and its output feeds machine motion
directly.

Counts: BLOCKING 0 · RESOLVE_SOON 5 · ACCEPTABLE 6 · FUTURE_IMPROVEMENT 4

## Operator Acceptance - 2026-10-18

The operator approved M29: "Aprobado el M29." This records operator
acceptance, not a new independent review or evidence of remediation.
The technical verdict above remains APPROVE WITH REQUIRED FIXES; M29-R1
through M29-R5 have no recorded closure evidence and remain required for
technical closure. No fixes, new test runs, deployment or hardware operations
were performed for this documentation-only update.

## Findings

### RESOLVE_SOON

**M29-R1. `;`-commented lines in a `.ini` profile are imported as active values.**
`src/config/prusa_import.c:241-243`. Any line starting with `;` is stripped and
parsed as a key, whatever the file type. PrusaSlicer's own ini reader treats
`;` lines as comments. Reproducer `/tmp/m29r/semicolon.ini`: the line
`; bed_shape = 0x0,500x0,500x500,0x500` comes before the real
`bed_shape = 0x0,235x0,...`, and because the first occurrence wins, the
output is `calibration.max_x=470.000` on a 235 mm bed (exit 0). The header
also wrongly says "sliced G-code configuration block".
Fix: in `.ini` mode, ignore `;` and `#` lines. Only accept `; key = value`
lines in G-code mode (see R2). Add a test.

**M29-R2. G-code body comments take precedence over the embedded config block.**
`src/config/prusa_import.c:129-130,153-155` (first occurrence wins) together
with `:241-252`. PrusaSlicer writes its config between
`; prusaslicer_config = begin` and `; prusaslicer_config = end`, at the end of
the file. Any earlier `; travel_speed = 999` (custom start G-code, a
post-processor, a hand-edited file) wins over it. Reproducer
`/tmp/m29r/body.gcode` gives `xy_feedrate=59940.000` instead of 6000.
Fix: when the file has a `prusaslicer_config = begin` marker, read keys only
between begin and end. If it has neither `[sections]` nor a marker, treat it
as `.ini`. Add tests for body-before-block, and for a duplicate key inside the
block (reject or last-wins).

**M29-R3. The binary G-code check can be bypassed.**
`src/config/prusa_import.c:212-230`. The `GCDE` magic is checked only when
the first `fgets()` chunk is a complete line. If the first 4095 bytes contain
no `\n`, the overlong-line branch runs first, sets `first_line = false`, and
the check is skipped. Readable `key = value` lines later in the file (for
example uncompressed bgcode metadata) are then imported. Reproducer
`/tmp/m29r/fake.bgcode` (GCDE header + 5000 bytes + ini lines) is accepted
as a "PrusaSlicer printer profile" with a 300 mm bed, exit 0. NUL bytes are
also silently handled as "overlong" (`nul.ini`).
Fix: read the first 4 bytes with `fread`, then `rewind`, before line
parsing. Treat any NUL byte as a binary or unsupported file.

**M29-R4. Non-rectangular `bed_shape` falls back to its bounding box.**
`src/config/prusa_import.c:77-112`. For a round or custom bed, the corners of
the shrunk bounding box can be off the bed. For a circle of radius R with
inset i, a corner lies at √2·(R−i) from the centre, which is beyond R once
R > 3.41·i (≈102 mm with the 30 mm default). The `calibrate-*-left/right`
macros move to exactly those corners. Reproducer `round.ini` (R = 100) gives
±70 corners, 99 mm from centre; R = 150 would put them off the bed.
Fix: accept only an axis-aligned rectangle (4 points, 2 distinct X, 2 distinct
Y) and reject anything else with `ERR_UNSUPPORTED_FORMAT`. Alternatively,
document and test it explicitly.

**M29-R5. The output does not show which printer profile it came from.**
`src/config/prusa_import.c:405-411`. A sliced `.gcode` carries whichever
printer profile it was sliced with. Of the repo's 34 `gcode/*.gcode` files, 23
were sliced with profile `printer_settings_id = Easy` (10 with `BiquB1`) (100×100 bed,
e.g. `Carriage.gcode:102476,102705`). They therefore produce
`calibration.max_x=70.000` and `xy_feedrate=7800`, not the B1's 205 and 9000.
That is harmless in this case (smaller), but the opposite case (a bigger
profile imported for a smaller printer) puts targets off the bed, and the
operator sees nothing that flags the mismatch.
Fix: read `printer_settings_id` and `printer_model` and print them in the
header comment. Mention in the README that a `.gcode` only reflects the
profile it was sliced with.

### ACCEPTABLE

- **A1. Reading the config from the control thread is race-free.**
  `runtime.c:139` copies the config into `worker_args` before
  `pthread_create` (`runtime.c:143`). The worker copies it into `ctx.config`
  (`worker.c:774`) and never writes `args` again. `runtime.c:103` memsets the
  runtime only at start. `printerctl_daemon_macro_calibration_config()` is a
  pure projection. No other shared mutable state was added: MACRO_LIST uses
  only the immutable registry, and `server->runtime` is never NULL
  (`socket.c:111`).
- **A2. `send_macro_list` stack use.** `socket.c:433-489` uses roughly 13.4 KB
  (`value`) + 13.6 KB (`payload`) + ~1.3 KB (`steps`). The worst-case
  encoding (2 + 32×426 = 13 634 B) fits the 64 KiB frame. The control thread
  already holds 64 KiB frame buffers (`socket.c:233,808`). Fine on a main
  thread; note it if this is ever run on small-stack threads.
- **A3. MACRO_RUN handling matches PRINT_START** (`socket.c:498-529` vs
  `:642-672`). The control thread blocks while the worker opens serial. This
  is pre-existing behaviour. The CLI may time out (5 s) while the macro still
  starts, the same as `print`.
- **A4. Embedded NUL in the MACRO_RUN id is accepted and truncated.**
  `plp1.c:1537-1563` relies on the generic `decode_string` (pre-existing for
  every PLP string). Reproducer: `"calibrate-center\0zz"` decodes to
  `calibrate-center` and would run. A client can only choose ids that are in
  the registry, so this is not a safety bypass. It is still a parsing
  ambiguity: consider rejecting `memchr(NUL)` in the macro decoders (and
  generically in §13).
- **A5. Exit code for a non-executable macro.** `cli.c:880` maps
  `INVALID_STATE` to 7 (COMM_ERROR), although the message at `cli.c:1154`
  says it is a configuration problem. `PRINTERCTL_EXIT_CONFIG_ERROR` (3)
  would be more accurate. The current mapping is consistent with existing
  commands and is not documented otherwise.
- **A6. Test hygiene.** The suite is green when isolated (see Verification).
  Two weaknesses: (a) under ASan,
  `test_cli_print_sigint_kills_observer_not_the_print` failed once in 3 direct
  runs. Its `query_print_status_until_state` retry budget is 200×10 ms
  (`tests/test_printerctld.c:519-532`), which is tight under sanitizers.
  (b) `ctest -j4` on the ASan build timed out `printerctld_tests` at 180 s;
  serial runs pass. Several tests share `/tmp/printerctl.sock`, so the suite
  is not `-j`-safe; `docs/testing.md` warns about this only partially.

### FUTURE_IMPROVEMENT

- **F1. Missing tests:** MACRO_LIST while a macro is active (the claim "valid
  during a print or macro" in `protocol-v1.md` §20a is untested); a second
  MACRO_RUN → BUSY while a macro runs; importer cases for CRLF (works, but is
  untested), R1-R4, units (`100mm/s` is correctly rejected), and duplicates.
- **F2. Formatted output is not re-validated.** `prusa_import.c:340-341,436-442`
  checks values before `%.3f` rounding. `travel_speed = 0.000001` produces
  `calibration.xy_feedrate=0.000` with exit 0, and the daemon then lists the
  macros as unavailable. The header's claim (`prusa_import.h:91-93`) that it
  validates "with the same rules the daemon applies" holds only before
  rounding. Either validate the rounded values, or round-trip through
  `printerctl_config_load_file` and `printerctl_macro_resolve_builtin`.
- **F3. STATUS does not identify the execution kind or macro id**, so the
  `macro run` observer cannot confirm it is following its own macro.
  Already listed as debt in `milestone-29.md`.
- **F4.** `power low-battery` does not reach a running macro, and there is no
  MACRO_CANCEL (documented debt). Macros never home and nothing tracks homed
  state: an operator who skips `home` sends absolute G90 XY moves to an
  unhomed machine. This is the inherited M28 decision; the CLI help, README
  and generated config all warn about it. Consider a homed-state gate in a
  later milestone.

## Checked and correct

- Codec: count ≤ 32, non-empty id, reserved flags, truncation and trailing
  bytes are all rejected (`plp1.c:1449-1513`). Wire bounds are
  `_Static_assert`ed against the macro domain (`socket.c:420-425`). Frame
  rules are correct: LIST request has no payload and its response requires
  one; RUN request requires a payload and its response has none
  (`plp1.c:173-176`). Message types and the capability bit are additive.
- The CLI grammar rejects missing, empty and overlong ids and extra
  arguments. `--dry-run` makes no daemon contact. `config from-prusa` sends
  the config to stdout and logs to stderr (checked by redirecting), with no
  daemon or serial access. Negative, NaN and Inf insets are rejected.
- Parser edge cases that work: CRLF, negative bed origins (`neg.ini`), values
  with units rejected, `[section]` bundles rejected, directory input reported
  as a read error. There is no `setlocale` anywhere, so strtod and printf use
  the C locale.
- Real profile `~/.config/PrusaSlicer/printer/BiquB1.ini` produces 30/205/30/205,
  safe_z 5, xy 9000, z 300 mm/min. These are sensible values for the B1.
- Build boundary: `printerctl` has no `printerctl_serial_`, `printerctl_printer_`,
  `printerctl_marlin_` or `printerctl_print_engine_` symbols (nm). It links only
  the client, protocol and prusa_import code. `printerctl_build_boundary_tests`
  passes.
- Docs (README, safety.md, testing.md, design, milestone) match the code. The
  exceptions are the `prusa_import.h:91-93` validation claim (F2), and the
  "printer profile" vs "G-code" label, which is wrong for `;`-commented ini
  files (R1).

## Verification

```text
cmake -S . -B build-m29review -DCMAKE_C_COMPILER=gcc && cmake --build build-m29review -j8
  -> no warnings
cmake -S . -B build-m29review-asan -DCMAKE_C_COMPILER=gcc \
      -DCMAKE_C_FLAGS='-fsanitize=address,undefined -g -O1' && cmake --build build-m29review-asan -j8
  -> 1 pre-existing warning (tests/test_plp_client.c:262 -Wformat-truncation, file not in M29 diff)
bwrap ... env TMPDIR=/tmp ctest --test-dir build-m29review --timeout 180   (x4, incl. -j4)
  -> 100% tests passed out of 21, every run
bwrap ... ctest --test-dir build-m29review-asan -j4 --timeout 180
  -> 20/21; printerctld_tests Timeout (parallel load, see A6)
bwrap ... build-m29review-asan/tests/printerctld_tests   (x3, serial)
  -> pass, pass, 1x FAIL "the daemon-owned print reaches COMPLETED despite the
     observer's SIGINT death" (timing flake, A6); no ASan/UBSan reports
nm build-m29review/printerctl | grep serial_/printer_/marlin_/print_engine_  -> none
printerctl config from-prusa on BiquB1.ini, 34 gcode/*.gcode files and reproducers in
/tmp/m29r/{semicolon.ini,body.gcode,fake.bgcode,round.ini,neg.ini,crlf.ini,units.ini,
nul.ini,tiny.ini,bundle.ini,dup.ini}  -> results quoted in findings
/tmp/m29r/nul.c (linked against libprinterctl_protocol_plp1.a) -> embedded-NUL id accepted (A4)
```
