---
title: Milestone 4 Design - Marlin Response Parser
milestone: 4
status: done
date: 2026-07-07
tags: [printerctl, milestone, design, marlin]
---

# Milestone 4 Design — Marlin Response Parser

Source of truth: [[Agents]], [[architecture]], [[roadmap]].

This note records the concrete design decisions behind the Milestone 4
implementation, ahead of the milestone report in
[[milestone-4|docs/milestones/milestone-4.md]].

## 1. Module split

A fifth independent static library, matching `architecture.md` §4's
`marlin` module boundary:

- `printerctl_marlin` (`include/printerctl/marlin.h`,
  `src/marlin/marlin.c`): response classification and typed report
  parsers. Pure text processing — no I/O, no heap, no global state, no
  dependency on `cli`, `config`, `logging` or `gcode`.
- Nothing links it yet. The serial module (Milestone 5) and the single
  Marlin commands (Milestone 6) become its first real consumers; this
  milestone deliberately builds and tests it in isolation, exactly as
  `roadmap.md` §7 requires ("independently from serial transport").

Its own CTest binary, `printerctl_marlin_tests`, links only
`printerctl_marlin` (plus `m` for the test comparisons).

## 2. Classification table

`printerctl_marlin_classify()` maps one received line (newline already
stripped — that is the future serial module's job) to exactly one of six
types. Every line classifies; classification never fails for actual text.

| Line shape | Type | `terminal` | Caller behavior |
|---|---|---|---|
| `ok` or `ok <payload>` | `ACK` | true | command acknowledged, send next |
| `Error:<text>` | `ERROR` | true | command failed, stop gracefully |
| `echo:busy:<text>` | `BUSY` | false | keep waiting |
| `busy:<text>` | `BUSY` | false | keep waiting |
| `echo:<text>` | `ECHO` | false | informational, keep waiting |
| `Resend:<n>` | `RESEND` | true | unsupported in MVP, stop with explicit status |
| anything else | `INFO` | false | informational, keep waiting |

Decisions inside the table:

- **`echo:busy:` is checked before `echo:`** — Marlin emits busy
  keepalives as `echo:busy: processing`; classifying them as plain echo
  would defeat the M7 streaming loop's "continue waiting on busy"
  behavior.
- **`terminal` is precomputed** so the streaming state machine (M7)
  reads one boolean instead of re-encoding the table.
- **`Resend:` is terminal-unsupported**, per `roadmap.md` §7 acceptance
  criteria: the MVP never sends line numbers or checksums, so a resend
  request is unrecoverable and must stop the print with an explicit
  status, never be ignored. The requested line number is decoded when it
  is a clean decimal (`has_resend_line`); a malformed value keeps the
  RESEND classification but reports no number rather than guessing.
- **Exact-case prefixes** (`ok`, `Error:`, `busy:`, `echo:`,
  `Resend:`), matching what Marlin sources actually emit. Case variants
  real firmware never produces stay `INFO`, which is safe: an
  unrecognized line never acknowledges a command and never fails one.
- **`payload` is a borrowed pointer into the caller's line** (documented
  lifetime, same discipline as `cli.h`'s argv pointers) — no copy, no
  buffer-size policy for a module that stores nothing.

## 3. Typed report parsers

Three standalone parsers decode the report lines the M6 commands need
(`temp`/M105, `position`/M114, `info`/M115). They take either a full
line or a classified `ok` payload, return
`PRINTERCTL_MARLIN_ERR_NO_MATCH` when the line is not that kind of
report, and zero the output struct in every non-OK case.

- **Temperature** (`T:actual /target B:actual /target @:... B@:...`):
  both the spaced (`T:210.00 /210.00`) and unspaced (`T:23.5/0.0`)
  variants parse. Token-boundary matching keeps `B@:` (bed power) from
  being misread as `B:`. Hotend-only lines are valid (`has_bed ==
  false`); a line with neither pair is NO_MATCH.
- **Position** (`X: Y: Z: E:` before `Count`): only the logical
  coordinates are decoded; stepper counts after `Count` are ignored. X,
  Y and Z are all required so arbitrary text containing an `X:` never
  misclassifies; E is optional (`has_e`).
- **Firmware info** (`FIRMWARE_NAME:... MACHINE_TYPE:... UUID:...`):
  M115 values contain spaces, so each value runs from its key to the
  next known key or end of line. `FIRMWARE_NAME` is required for a
  match; other fields default to empty. Text fields are fixed 128-byte
  buffers with silent truncation (display-only data; real Marlin values
  are far shorter). An unparsable `EXTRUDER_COUNT` yields
  `has_extruder_count == false`, never a guess.

## 4. Status values

```c
PRINTERCTL_MARLIN_OK
PRINTERCTL_MARLIN_ERR_INVALID_ARGUMENT   /* NULL line or NULL out */
PRINTERCTL_MARLIN_ERR_NO_MATCH           /* line is not this kind of report */
```

`NO_MATCH` is an expected, non-exceptional answer ("this line is not a
temperature report"), which is why it is a status and not an operator
error: M6 will probe report parsers against informational lines while
waiting for `ok`.

## 5. Compatibility assumptions

Verified against Marlin's documented output shapes (1.x and 2.x):
`ok` with optional payload (including `ADVANCED_OK`'s `ok N.. P.. B..`),
`echo:busy: processing`, `Error:` kill messages, `Resend:` +
line number, M105 reports with and without the space before `/`, M114's
`Count` split, and M115's `KEY:value with spaces` capability line.
Multi-hotend reports (`T0:`/`T1:`) are out of scope: the target printer
(EasyThreed K9, single extruder) never emits them; revisit if a
multi-tool printer ever becomes a target.

## 6. Deferred

- Wiring the classifier into a real receive loop: M5 (serial) and M6/M7.
- `Resend:` recovery, line numbering, checksums: post-MVP backlog items
  1–3.
- Rendering decoded reports for operators (`temp`/`position`/`info`
  output): M6, where the commands exist.
