---
title: Milestone 12 — Extract Print Engine
milestone: 12
status: in_progress
date: 2026-07-28
tags: [printerctl, milestone, core, print-engine, daemon]
---

# Milestone 12 — Extract Print Engine

Source of truth: [[Agents]], [[Kernel/Architecture]], [[Kernel/Roadmap]]. Design
details: [[milestone-12-print-engine|docs/design/milestone-12-print-engine.md]].

## 1. Technical summary

Phase II has started with Milestone 12. The current print flow was
reviewed before implementation:

- `src/printer/printer.c` already contains the ACK-driven streamer in
  `printerctl_printer_print_file()`.
- `src/cli/cli.c` still owns process/adapter concerns: signal handlers,
  terminal rendering, active-print PID marker publication, session-record
  warnings and exit-code mapping.
- `include/printerctl/printer.h` already exposes progress and control
  callbacks, including separate operator and power-failure cancellation.

The approved direction for implementation is extraction, not rewrite:
create a core print engine that owns the application print lifecycle and
reuses the existing printer streamer. The CLI should become an adapter
over that engine.

## 2. Files modified

- `docs/design/milestone-12-print-engine.md` (new)
- `docs/milestones/milestone-12.md` (new)

No production code has been changed yet in this phase.

## 3. Risks found

- The current streamer is already tested; replacing it would add risk
  without satisfying new acceptance criteria.
- Terminal output and record-warning behavior must become events before
  the daemon can consume the same core.
- Signal handling must remain outside the core. The engine should expose
  control requests, not install process-level handlers.
- A fake non-blocking `tick()` API would be premature unless the daemon
  implementation actually needs it.

## 4. Decisions taken

- M12 will reuse `printerctl_printer_print_file()` as the lower-level
  streamer.
- M12 will extract the lifecycle currently embedded in `run_print_command()`:
  connect, active marker, stream, interrupted-session record and
  disconnect.
- M12 will not implement daemon, Unix socket IPC, web gateway, JSON,
  queueing or multi-client semantics.
- Initial production implementation should add only the core files that
  contain real behavior; no placeholder modules.

## 5. External review

Claude review was requested on 2026-07-28 with the required architecture
review prompt. The command did not produce a technical response within
the execution window and was interrupted. No external recommendations
were incorporated. This remains as a point-in-time note.

As of 2026-07-29, Claude is no longer part of the active workflow.
Before code changes, Codex Lead should assign scoped Codex subagents for
implementation and independent review, then integrate only changes that
match
[[milestone-12-print-engine|docs/design/milestone-12-print-engine.md]],
`architecture.md` and `roadmap.md`.

## 6. Verification

- `git status --short`: clean before M12 documents were added.
- `git diff --check`: clean before M12 documents were added.
- No build/test run was required for this documentation-only start step.

## 7. Next steps

1. Assign scoped Codex subagents for M12 implementation and review.
2. Review the resulting public API before accepting it.
3. Run GCC and Clang builds.
4. Run the full CTest suite.
5. Add/update engine tests proving operation without CLI parsing or
   terminal output.
6. Update living documentation after production changes.
