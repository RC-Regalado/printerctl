---
title: Milestone 3 Design — G-code Parser and Dry Run
status: done
date: 2026-07-07
tags: [printerctl, milestone, design, gcode, dry-run, safety]
---

# Milestone 3 Design — G-code Parser and Dry Run

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]].

## Scope

Milestone 3 adds safe G-code file parsing for `--dry-run print FILE`.
It does not add serial transport, Marlin acknowledgement handling or real
printing.

## Decisions

- `gcode` is a standalone streaming reader: no heap allocation, no serial
  dependency, one normalized command per call.
- Only regular files are accepted. The reader snapshots file
  device/inode/size/mtime after open and fails if the file changes while it
  is being read.
- Maximum raw source line length is `4096` content bytes, excluding the
  newline.
- Normalization strips:
  - full-line and inline `;` comments;
  - parenthesized comments, nested up to depth 8;
  - leading/trailing spaces and tabs;
  - blank/comment-only lines.
- Malformed input includes unmatched or unterminated parenthesized
  comments, control bytes other than tab, stray carriage returns, and BOM
  bytes anywhere except byte 0.
- Progress is byte-based: floored whole percent from bytes consumed over
  open-time file size. Empty files are complete.
- `G28` is blocked by default. Detection is case-insensitive, supports an
  optional `N<digits>` prefix and conservative variants such as `G 28`,
  `G028` and `G28.1`.
- The CLI dry-run path always passes `allow_g28 = false`; `print` has no
  confirmation flag in this milestone.

## CLI behavior

`printerctl --dry-run print FILE`:

- parses and reports normalized commands to stdout;
- includes source line number and progress percentage;
- logs dry-run state transitions and a final progress summary;
- logs per-command progress only in verbose mode;
- returns `0` on clean parse, `4` for unsafe `G28`, and `6` for other
  G-code file errors;
- never opens a serial device.

Other valid commands, including `print FILE` without `--dry-run`, still
return exit code `5`.

## Privacy

Structured logs keep the Milestone 2 privacy rule: no raw file path and no
G-code command text. Human-readable output may show the file path and
normalized command text because it is the operator-facing dry-run report.

## Risks

- Parenthesized comment handling is intentionally stricter than many
  slicers need; malformed comments fail early instead of guessing.
- `G28` detection is conservative and may block rare subcodes. This is
  acceptable because the printer has no trusted endstop path yet.
- Byte progress can jump unevenly on comment-heavy files; it is stable and
  cheap, and good enough before real streaming exists.
