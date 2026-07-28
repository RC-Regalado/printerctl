---
title: Milestone 11 — Core Documentation Completion
milestone: 11
status: done
date: 2026-07-21
tags: [printerctl, milestone, documentation]
---

# Milestone 11 — Core Documentation Completion

Source of truth: [[Agents]], [[architecture]], [[roadmap]].

## 1. Technical summary

The revised `roadmap.md`'s Milestone 11 asks for the printing core's
documentation to be complete and coherent before Phase II's daemon
transformation begins. This milestone is documentation-only: no
production code changed, and the full build/test suite is unaffected
(verified unchanged: 8/8 test binaries, both compilers, zero warnings).

Three new reference documents were added, consolidating material that
was previously scattered across `Agents.md`, `architecture.md` and
per-milestone design notes into single operator-facing references:

- `docs/safety.md` — the full safety policy (homing, `M112`, dry-run,
  graceful failure, power-failure cancellation, no auto-resume/
  reconnect, exit-code-to-safety mapping, hardware-testing policy).
- `docs/apcupsd.md` — the roadmap's required "power-failure behavior"
  topic: what event triggers cancellation, how the script communicates
  with the process (the active-print PID marker plus `SIGPWR`), what
  resources close, what information is preserved, why no auto-resume,
  and how to inspect a cancelled job.
- `docs/testing.md` — the eight test binaries and what each covers, the
  test patterns behind them (pty mock printer, scripted control
  callback, forked-CLI-plus-signals, disposable `XDG_STATE_HOME`,
  `/proc/self/fd` descriptor-leak proof, the no-auto-resume rerun
  proof), the milestone-by-milestone hardware-validation record, and
  how to verify the build/test instructions from a clean checkout.

`architecture.md` gained a new §7.5 mapping the roadmap's job-model
lifecycle states (`queued`, `starting`, `printing`, `paused`,
`completed`, `cancelled_operator`, `cancelled_power_failure`,
`failed_serial`, `failed_printer`) onto what already exists in
`printer.h`'s status/state enums and the session record — honestly
noting that `queued`/`starting` have no direct equivalent yet (there is
no job queue before Milestone 13), rather than inventing one. §13's
open technical decisions 3 and 6 were resolved (test framework: the
hand-rolled `check()` harness, de facto; daemon mode: yes, per the
revised roadmap's Phase II).

`Components.md` and `context-ia.md` — two firmware-facing context
documents that had drifted to describing a "Milestone 7" state (pause/
resume/cancel and `home` both listed as unavailable, hardware described
as Z-min-only) — were brought current: every command's real status,
the `record` module, the APCUPSD script, and the end-stop hardware
update. `docs/design/unix-socket-service-analysis.md` (an earlier,
pre-roadmap-revision daemon proposal) was marked superseded in its
protocol specifics, with a pointer to the roadmap's actual Phase II
plan, rather than left silently conflicting with it.
`Agents.md`'s own "Documentation" list was corrected: it named
`docs/architecture.md` and `docs/marlin.md`, neither of which was ever
created — the project settled on `architecture.md` at the repository
root starting with Milestone 0, and the Marlin protocol contract lives
in its §6.5, not a separate file.

## 2. Files changed

- `docs/safety.md` (new)
- `docs/apcupsd.md` (new)
- `docs/testing.md` (new)
- `architecture.md` (§7.5 printing lifecycle states; §13 items 3 and 6
  resolved; cross-references to the three new docs)
- `Components.md` (brought current: implemented commands, `record`
  module, APCUPSD script, end-stop update, daemon now planned not just
  "out of scope")
- `context-ia.md` (implemented-commands list and hardware section
  brought current)
- `docs/design/unix-socket-service-analysis.md` (superseded-status note)
- `Agents.md` (Documentation section's stale paths corrected)
- `README.md` (status line; cross-references to the three new docs)
- `docs/milestones/milestone-11.md` (new, this report)

Historical per-milestone reports and design notes under
`docs/milestones/` and `docs/design/` were intentionally left
untouched — they are point-in-time records of what was true and decided
at the time, not living references; only `README.md`, `architecture.md`,
`Agents.md`, `Components.md`, `context-ia.md` and the `docs/` root-level
reference docs are treated as living documentation that tracks current
behavior.

## 3. Acceptance validation

Mapped to the new roadmap's Milestone 11 acceptance criteria:

- **La arquitectura actual está documentada:** `architecture.md` already
  tracked every milestone through 10 plus the end-stop update; this
  milestone added the printing-lifecycle state mapping (§7.5) and
  resolved the two remaining open technical decisions (§13).
- **El fallo de alimentación tiene una política explícita:**
  `docs/apcupsd.md` covers all six items the roadmap's "Power-failure
  behavior" topic lists explicitly (triggering event, script-to-process
  communication, resources closed, information preserved, why no
  auto-resume, how to inspect a cancelled job); `docs/safety.md` §6
  states the invariant (nothing beyond "stop transmitting" is ever
  sent) as part of the consolidated safety reference.
- **Los scripts tienen documentación individual:**
  `docs/scripts/printerctl-low-battery.md` already existed from
  Milestone 10 and covers every item the roadmap's "Script
  documentation" checklist requires (purpose, usage, arguments,
  environment variables, dependencies, side effects, exit codes,
  possible failures, recovery procedure); it is the only `.sh` in the
  repository.
- **Las instrucciones de compilación y prueba funcionan:** verified by
  actually copying the current working tree (excluding build
  directories) into a fresh directory and running the exact commands
  `README.md` documents — `cmake -S . -B build`, `cmake --build build`,
  `ctest --test-dir build --output-on-failure` — from there: clean
  configure, warning-free build, 8/8 tests passed, `--help`/`--version`
  smoke-tested.
- **El roadmap está preparado para la fase daemon:** the
  `unix-socket-service-analysis.md` proposal is now explicitly marked
  superseded rather than left as a silently conflicting alternative
  design; `architecture.md` §13 item 6 points at the roadmap's actual
  Phase II plan as the current one.

## 4. Verification

- GCC build (`build-gcc`, clean rebuild): warning-free with
  `-std=c17 -Wall -Wextra -Wpedantic`; 8/8 test binaries pass.
  (Documentation-only milestone: no production code changed, so this
  confirms no regression rather than new behavior.)
- Clang build (`build-clang`, clean rebuild): warning-free; 8/8 test
  binaries pass.
- Codex validation on 2026-07-21: reviewed the combined Claude changes
  for Milestone 10, Milestone 11 and the end-stop/home policy update;
  reran GCC/Clang builds, CTest, `clang-tidy`, `bash -n` for the APCUPSD
  script and `git diff --check`; smoke-tested dry-run `home`,
  `home X`, `home Y`, `home Z`, `send G28` rejection, `power
  low-battery` with no active print, `--dry-run power low-battery`, and
  the APCUPSD script success path with a stand-in `printerctl`.
- Clean-checkout verification: the working tree was copied (minus
  `.git`, build directories and editor/agent state) into
  `/tmp/printerctl-clean-check`, then `cmake -S . -B build
  -DCMAKE_C_COMPILER=gcc`, `cmake --build build` and `ctest --test-dir
  build --output-on-failure` were run there directly — configure,
  build and all eight suites succeeded with no manual setup beyond a
  C17 compiler and CMake, confirming `README.md`'s documented steps are
  self-sufficient.
- Cross-reference sweep: every `docs/*.md` path mentioned across
  `README.md`, `architecture.md`, `Agents.md`, `Components.md`,
  `context-ia.md` and the `docs/` reference docs themselves resolves to
  a real file, with the sole (intentional) exceptions being
  `Agents.md`'s corrected note naming the two paths that were never
  created, and `docs/milestones/milestone-0.md`'s historical record of
  the same fact at the time.

## 5. Risks found

- None specific to this milestone (documentation-only, no production
  code changed). The main latent risk this milestone addresses is
  documentation drift itself: `Components.md` and `context-ia.md` had
  silently fallen four-plus milestones behind actual behavior before
  this pass, which is exactly the kind of drift Codex's "revisar
  coherencia" task exists to catch. No process change was introduced to
  prevent recurrence beyond this milestone's existence; future
  milestones should keep including these two files in their own
  documentation-update step rather than treating them as one-off
  context handed to a firmware agent and then forgotten.

## 6. Decisions taken

- Three new root-level-under-`docs/` reference documents
  (`safety.md`, `apcupsd.md`, `testing.md`), matching this project's
  established lowercase naming convention (`docs/logging.md`) rather
  than the new roadmap's literal capitalized suggestions
  (`docs/Safety.md` etc.) — consistency with the existing convention
  was judged more valuable than literal path matching, since the
  roadmap's capitalization does not match anything else in this
  repository.
- `architecture.md` (root) remains the canonical location for the
  architecture document; no `docs/architecture.md` was created. Same
  reasoning: matching the convention established since Milestone 0
  over introducing a second, differently-located copy.
- No dedicated `docs/marlin.md` was created; the Marlin protocol
  contract continues to live in `architecture.md` §6.5 and
  `Components.md` §4, which already cover it and are already
  cross-referenced from the relevant places.
- Historical per-milestone documents were left untouched on principle,
  even where they describe now-superseded states (e.g. Milestone 1's
  report correctly describing `home`'s original confirmation-flag
  behavior) — they are point-in-time records, not living references;
  only the "living" documents listed in §2 were brought current.

## 7. Next steps

Continue with Phase II, Milestone 12: extract the print engine from the
current CLI so it no longer depends on CLI arguments, the terminal, or
any UI shape — the daemon-agnostic foundation `printerctld` (Milestone
17) will eventually run.
