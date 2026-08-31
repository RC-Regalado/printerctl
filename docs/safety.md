---
title: Safety Policy
tags: [printerctl, safety, policy, reference]
---

# Safety Policy

Source of truth: [[Agents]], [[Kernel/Architecture]]. This is the
operator-facing consolidation of the safety rules scattered across
`Agents.md` (the directive) and `architecture.md` §8 (the acceptance
criteria); read this document for the rationale and worked examples,
those two for the authoritative rule text.

Safety has priority over convenience: the printer must never move,
heat, extrude or home except as a direct, explicit result of an
operator's own command.

## 1. Homing (`G28`)

Min end-stops are installed and enabled on all three axes (X, Y, Z), as
of 2026-07-21 — see `Agents.md`'s Home amendment. This changes what is
*safe*, but not the underlying principle (explicit action only):

- `home` sends `G28` (all axes). It requires no confirmation flag,
  only a configured `--port` — homing is an ordinary, expected
  operation once end-stops exist.
- `home X` / `home Y` / `home Z` send `G28 X` / `G28 Y` / `G28 Z` to
  home a single axis.
- `G28` remains refused everywhere else, unconditionally, with no
  bypass:
  - `send "G28"` (or any command containing `G28`) exits `4`
    (`PRINTERCTL_EXIT_SAFETY_ERROR`).
  - A `print` G-code file containing `G28` fails before that line is
    ever transmitted, also exit `4`.

The reason `G28` stays blocked in `send` and `print` even though
homing itself is now safe: homing must always be an explicit, direct
request through the dedicated `home` command, never an unexpected side
effect of a raw command typed for another purpose, or of a line buried
in a sliced file. A file that happens to contain `G28` (e.g. a start
G-code snippet copied from another printer's profile) should fail loud
and early, not home the printer as a surprise mid-stream.

```bash
printerctl --port /dev/ttyUSB0 home       # G28 -- allowed
printerctl --port /dev/ttyUSB0 home X     # G28 X -- allowed
printerctl --port /dev/ttyUSB0 send "G28" # refused, exit 4
printerctl --port /dev/ttyUSB0 print with-g28.gcode # refused, exit 4
```

## 2. Emergency stop (`M112`)

`M112` is never sent automatically by printerctl, under any
circumstance — not on a fault, not on a power-failure cancellation, not
on any internal error path. It passes through `send "M112"` exactly
like any other operator-typed command, because it is reserved for the
operator's own, deliberate emergency action.

## 3. No automatic homing, heating, movement or extrusion

Beyond `G28` specifically: nothing in printerctl issues a heating
(`M104`/`M109`/`M140`/`M190`), movement (`G0`/`G1`) or extrusion command
on its own initiative, ever. Every G-code command the printer receives
either comes from the file the operator asked to `print`, or from a
command the operator typed directly (`send`, `home`, the SD-print
controls).

## 4. Dry-run guarantee

`--dry-run` never opens or writes to a serial device, for any command,
including `home`. A dry run reports the line(s) that would be
transmitted and exits `0` on success — no hardware is ever touched,
regardless of what the command would otherwise do.

## 5. Graceful failure, always

G-code streaming (`print`) stops gracefully — no further command is
transmitted, the file and the serial port are closed, a summary is
rendered — on every failure mode: `Error:`, `Resend:` (unsupported in
the MVP), a response timeout, a serial disconnect, or a malformed/unsafe
line found mid-file. See [[Kernel/Architecture]] §7.5 for the full state
mapping and [[docs/testing]] for how each path is exercised.

## 6. Power-failure cancellation

A low-battery/imminent-shutdown signal (`printerctl power low-battery`,
normally run by `scripts/apcupsd/printerctl-low-battery.sh`) cancels an
active print exactly like the operator's own `Ctrl+C`, with one rule
that never bends: **nothing beyond "stop transmitting" is ever sent**.
No `M112`, no `M410`, no homing, no resume command, no additional
movement — the goal is to stop cleanly, not to attempt a physically
risky sequence while power may be compromised. See [[docs/apcupsd]] for
the full mechanism.

For streamed host prints, cancellation has three distinct moments:
request, commit, and stopped. Bytes for a command whose serial write
already began before commit may still be in flight and may still receive
its ACK. After commit, the TX authorization gate forbids starting any
new G-code command transmission.

## 7. No automatic resume, no automatic reconnect

Nothing in printerctl ever reads an interrupted-session record back to
resume a print — there is no code path that does, by construction, not
merely by convention (see `record.h`'s module contract). A serial
disconnect or a power-failure cancellation ends the session; restarting
the job is always the operator's own, later, explicit decision. There
is also no automatic reconnect: reopening a USB CDC device toggles DTR
and resets the controller, so a "reconnected" session would face an
unknown, cold printer state anyway (see
[[docs/design/milestone-9-recovery-records]] §6).

## 8. Exit codes as a safety signal

| Code | Meaning | Safety relevance |
|---|---|---|
| `4` | Safety policy rejection | `G28` via `send`/`print` (never `home`); the only code a safety gate itself produces |
| `7` | Communication error | Includes a serial disconnect mid-print — always a graceful stop, never a silent one |
| `9` | Cancelled by the operator | `SIGINT`/`SIGTERM` during `print`; nothing sent to the printer |
| `10` | Cancelled by a power-failure signal | `SIGPWR` during `print`; nothing sent to the printer beyond stopping |

The full exit-code table lives in `README.md`; this row selection is
just the safety-relevant subset.

## 9. Hardware testing policy

Because the printer performs real physical motion, hardware validation
in this project is deliberately limited to commands that cannot cause
harm even if something is misconfigured: `M105`, `M114`, `M115`, `G4`
(dwell), and — since the end-stop update — `home`/`home X`/`home
Y`/`home Z` (bounded, switch-terminated motion). Motion, heating or
extrusion commands are never streamed to real hardware without an
explicit request from the person operating the session. See
[[docs/testing]] §Hardware validation for the milestone-by-milestone
record of what was confirmed on real hardware.
