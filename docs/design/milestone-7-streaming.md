---
title: Milestone 7 Design - Reliable G-code Streaming MVP
milestone: 7
status: done
date: 2026-07-07
tags: [printerctl, milestone, design, printer, streaming, gcode]
---

# Milestone 7 Design — Reliable G-code Streaming MVP

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]].

This note records the concrete design decisions behind the Milestone 7
implementation, ahead of the milestone report in
[[milestone-7|docs/milestones/milestone-7.md]].

## 1. No new module: the print session lives in `printer`

Streaming is `printerctl_printer_print_file()` inside the existing
`printer` module, not an eighth library. It is precisely the
composition `printer` exists for (architecture.md §6.7 — "coordinate
G-code streaming through `gcode`, `marlin` and `serial`"), and every
building block already existed: the M3 reader streams one normalized
command at a time with progress attached, and the M6 exchange loop
(`printerctl_printer_execute()`) already implements
send-one-line-and-wait-for-terminal with `busy:`/`echo:` handling and
TX/RX logging. The print loop composes exactly those two.

## 2. State machine: minimal explicit states for the MVP

The MVP session is strictly linear — connect → printing → completed or
failed — so the implementation keeps the state in control flow and
makes it *observable* rather than building a state-object framework
nothing else consumes yet: an INFO `state_transition` record marks
`state=printing` at start and `state=print_completed` /
`state=print_failed` (with the summary attached) at the end, and the
summary struct carries an explicit `printerctl_print_final_state_t`.
M8 (pause/resume/cancel) is the milestone that introduces states a
caller can act on; widening the enum then is an additive change.

## 3. Strictly one command in flight (the no-flooding guarantee)

The loop body is: read one command from the file → transmit it → wait
for the terminal response → only then read the next command. The next
command is not even *read from the file*, let alone transmitted, before
the previous acknowledgement. There is no send-ahead window, no
buffering of parsed commands, and therefore no way to flood the serial
buffer. The acceptance test makes this observable: a strict mock that,
after receiving each complete line, polls its side of the pty for
150 ms and fails the test if any byte arrives before it has sent its
`ok`.

## 4. Stop-gracefully semantics

Any failure — `Error:`, `Resend:`, timeout, serial failure/disconnect,
or a G-code file error mid-stream (including a G28 line) — follows one
path: no further command is transmitted, the reader is closed, the
summary is filled with exactly how far the print got
(sent/acknowledged counts, last acknowledged source line, byte
progress), one ERROR record plus the `state=print_failed` transition
are logged, and the caller gets the specific status. The session stays
connected (the caller owns disconnect), matching M6's
session-lifetime-in-the-caller shape.

Two failure-origin worlds map to different exit codes at the CLI:
transport/printer failures reuse the M6 mapping (7/8), file failures
map to the M3 codes (6, or 4 for G28) via the new
`PRINTERCTL_PRINTER_ERR_GCODE` status — an operator seeing exit 6 knows
the file is the problem regardless of whether it failed in a dry run or
mid-print.

## 5. Progress reporting

Per acknowledged command: the M3 byte-based whole-percent progress,
straight from the reader's result. Three consumers, three rates:

- **stdout**: one line per whole-percent change (a 200 k-line print
  renders at most ~100 progress lines, not 200 k).
- **log, DEBUG**: one `progress` record per acknowledgement (the full
  trace, `--verbose` only).
- **log, INFO**: one `progress` record per whole-percent change, so a
  non-verbose log still shows a bounded, useful progress trail.

The summary line (stdout) and the `state=print_*` transition (log)
both carry the final counts: state, file (stdout only — never in the
log, per the M2/M3 privacy rules), commands sent/acknowledged, last
acknowledged line, percent.

## 6. Timeout semantics inherited, deliberately

The M6 rule — the timeout bounds *silence*, not command duration —
matters even more for streaming: a long homing-free move or a `G4`
dwell answers its `ok` seconds later, and Marlin emits `busy:`
keepalives roughly every 2 s while blocking. Each received line
restarts the read deadline. The real-hardware smoke test exercised
exactly this: `G4 P250`'s acknowledgement arrived 253 ms after
transmission, well within one timeout window, with no early
transmission of the next command.

## 7. `print` without a port is a configuration error

Streaming requires a connection, so `print` without `--port` now fails
with exit 3 before touching the file (two M1-era CLI tests updated
accordingly, same as M6 did for the query commands). The dry-run path
is unchanged and still needs no port.

## 8. Hardware validation constraints

The smoke test against the real printer (`/dev/ttyUSB0`, Marlin
2.1.2.1) streamed a file of *harmless* commands only — `M105`, `G4`,
`M114`, `M115` — because the machine has only a Z-min endstop and the
Agents.md safety policy forbids automatic motion, heating or extrusion.
No movement command was streamed. This constraint holds for all future
hardware verification: motion G-code requires the operator explicitly
asking for it.

## 9. Deferred

- Pause/resume/cancel: M8 (the state machine grows real intermediate
  states there; architecture.md §13 item 1 stays open until then).
- Interrupted-session records, reconnect: M9.
- A cap on non-terminal lines per exchange (carried from M6): the
  timeout covers silence, the operator covers chatter.
- Line numbers/checksums/resend recovery: post-MVP backlog.
