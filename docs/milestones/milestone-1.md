---
title: Milestone 1 - CLI and Configuration
milestone: 1
status: done
date: 2026-07-04
tags: [printerctl, milestone, cli, config]
---

# Milestone 1 — CLI and Configuration

Source of truth: [[Agents]], [[architecture]], [[roadmap]]. Design details:
[[milestone-1-cli-config|docs/design/milestone-1-cli-config.md]].

## 1. Technical summary

Implemented the full Milestone 1 command-line grammar and a new `config`
module, without touching hardware, serial, G-code or logging.

`printerctl` now understands:

```text
printerctl [GLOBAL_OPTIONS] COMMAND [COMMAND_ARGS]
printerctl --help
printerctl --version
```

Global options — `--port`, `--baud`, `--timeout`, `--dry-run`, `--verbose`,
`--config` — accept only the `--option VALUE` form and must appear before
`COMMAND`, at most once each. Commands `print`, `send`, `home`, `pause`,
`resume`, `cancel`, `temp`, `position`, `info` are all recognized with
their documented argument shapes. Every valid command is parsed and its
configuration is fully resolved (defaults → optional `--config` file → CLI
overrides), then `printerctl_cli_run()` reports that execution is not yet
implemented and exits `5` — this proves the full parse/config pipeline
without ever pretending to control a printer. `home` is refused with exit
`4` unless `--i-understand-no-endstops` is given, including under
`--dry-run`.

Configuration is a new, independent module (`printerctl_config`):
`printerctl_config_t` (fixed-size `port` buffer, `baud`, `timeout_ms`,
`dry_run`, `verbose`) plus `printerctl_config_overrides_t`, a sparse
`has_*`-flagged mirror used both for `--config FILE` contents and CLI
options so precedence resolution is one code path
(`printerctl_config_apply_overrides`) applied twice, in order. See
[[milestone-1-cli-config]] for the full rationale, including the
deliberate deviations from `architecture.md`'s original config sketch, the
config file's forward-compatibility with Milestone 10's `[apcupsd]`
section, and why `send`/`G28` safety must be revisited before Milestone 6.

## 2. Files added or modified

```text
include/printerctl/cli.h        (rewritten: full grammar, exit codes, ownership contract)
include/printerctl/config.h     (new)
src/cli/cli.c                   (rewritten: pure parser + composition run function)
src/config/config.c             (new)
CMakeLists.txt                  (new printerctl_config library, linked into printerctl_cli)
tests/CMakeLists.txt            (new printerctl_config_tests target)
tests/test_cli.c                (rewritten: full grammar coverage)
tests/test_config.c             (new)
docs/design/milestone-1-cli-config.md (new)
docs/milestones/milestone-1.md  (this file)
README.md                       (updated to Milestone 1 status)
architecture.md                 (resolved config and exit-code decisions)
```

`src/main.c` is unchanged — it was already a two-line shim calling
`printerctl_cli_run()`, and that contract did not need to change.

## 3. Build and test targets

- `printerctl_config` — new static library, `src/config/config.c` only.
  No dependency on `cli` or argv; testable in complete isolation.
- `printerctl_cli` — static library, `src/cli/cli.c`, now `PUBLIC`-linked
  against `printerctl_config` so anything linking `printerctl_cli` (the
  `printerctl` binary, `printerctl_tests`) automatically gets `config`
  symbols too.
- `printerctl` — production executable, unchanged (`src/main.c` only).
- `printerctl_tests` — CLI grammar tests, linking `printerctl_cli`.
  Covers help/version, every global-option error case (unknown option,
  `--option=VALUE` rejected, duplicates, missing values, out-of-range
  `--baud`/`--timeout`), every command's valid and invalid argument
  shapes, the `home` safety gate (including under `--dry-run`), a CLI
  value equal to its default, and `--config` integration (valid file with
  CLI precedence, missing file, invalid file, duplicate key); plus, from
  the Milestone 1 review fixes, an empty `--port` value, an empty `print
  FILE`/`send COMMAND` argument, and a direct unit test of the pure
  `printerctl_cli_parse()` contract (resolved action/command, `print_file`
  aliasing straight into `argv`, and override values with their `has_*`
  flags).
- `printerctl_config_tests` — new, independent CTest binary linking only
  `printerctl_config` (not `printerctl_cli`). Covers defaults, a fully
  valid file, file-not-found, unknown key, duplicate key, malformed line,
  empty key, invalid/out-of-range `baud`, invalid/out-of-range `timeout`,
  invalid boolean, oversized line, oversized `port` value, file+CLI
  precedence, CLI value equal to default, an invalid file surfacing
  through `resolve`, and a file-set boolean surviving when the CLI does
  not touch that flag; plus, from the Milestone 1 review fixes, an empty
  and a whitespace-only `port` value, a read failure surfaced after
  `fopen()` already succeeded (opening a directory as the config path),
  and the exact line-length boundary (`PRINTERCTL_CONFIG_LINE_MAX - 2`
  characters accepted, one character more rejected).

Both binaries are registered with `enable_testing()`/`add_test()` and run
under `ctest`.

## 4. Decisions

1. **Two static libraries, not one.** `config` has no knowledge of argv or
   CLI syntax; `cli` depends on it, never the reverse. This matches
   `architecture.md` §4's module boundaries and lets `printerctl_config`
   be exercised by its own test binary with zero CLI machinery.
2. **Pure parser separated from the composition function.**
   `printerctl_cli_parse()` is argv-in, request/error-out, with no I/O, no
   filesystem access and no heap allocation. `printerctl_cli_run()` is the
   only place that calls it, resolves config, and renders output — this
   is what `main()` calls. See [[milestone-1-cli-config]] §2.
3. **No heap ownership; fixed buffers; overflow is a hard error.**
   `port` and the `--config` path are fixed-size buffers
   (256 and 4096 bytes). Any value that does not fit is rejected outright,
   never truncated. `print_file`/`send_command` are raw pointers into argv,
   valid only for the duration of the invocation that produced them — this
   is documented directly in `cli.h`, not left implicit.
4. **`config_overrides_t` uses one `has_*` flag per field.** This is what
   makes an explicit `--baud 115200` (equal to the default) behave
   identically to an explicit `--baud 250000` (different from the
   default) — both are "set," independent of value. Verified directly by
   `test_cli_value_equal_to_default` and
   `test_resolve_cli_value_equal_to_default`.
5. **Exit codes are a fixed five-value taxonomy, not reserved ranges.**
   `0` success, `2` usage, `3` configuration, `4` safety, `5` not yet
   available. `1` (used loosely by Milestone 0) is retired rather than
   reused for a new meaning.
6. **`home` confirmation is CLI-only, never a config key.** A safety
   confirmation that could be satisfied by a checked-in config file would
   defeat its purpose. `--dry-run home` without confirmation still exits
   `4` — dry-run controls whether serial I/O happens later, not whether a
   destructive command is considered acknowledged.
7. **Config file format: full-line comments only, no inline `#`.** Keeps
   the parser's error surface unambiguous for Milestone 1; can be revisited
   later without breaking the root `key=value` shape.
8. **No section-header parsing yet.** The root format stays valid once
   Milestone 10 introduces `[apcupsd]`; a section-header line is currently
   rejected as a malformed line, which is safe (fails loud) rather than
   silently ignored.
9. **Empty-string values are rejected outright, never silently accepted.**
   `--port ""`, `print ""`, `send ""`, and a config file's `port=` with
   nothing (or only whitespace) after the `=` are all explicit usage or
   configuration errors, rather than resolving to an empty port or file
   path that would surface confusingly later.
10. **A read failure after `fopen()` succeeds is a distinct, explicit
    config error.** `printerctl_config_load_file()` checks `ferror()` once
    its read loop ends, so a config path that opens but cannot actually be
    read (e.g. a directory) reports `PRINTERCTL_CONFIG_ERR_READ_FAILED`
    instead of being silently treated as a clean, complete read. A parse
    error already found on an earlier line always takes priority over a
    later read failure.

## 5. Deviations from `architecture.md`

- `printerctl_config_t.port` is a fixed-size `char[256]`, not `const char
  *`, so the resolved config never depends on the lifetime of argv or a
  file-read buffer and overflow can be rejected rather than truncated.
- `apcupsd_enabled`/`apcupsd_auto_resume` are omitted from
  `printerctl_config_t` entirely; they are Milestone 10 scope and adding
  them now would be untested, unused fields.

Both are recorded in detail in [[milestone-1-cli-config]] §10 and are
scoped to this milestone's implementation, not changes to the approved
architecture document itself.

## 6. Risks

- **`send` does not yet enforce the `G28` safety policy**, because it does
  not execute anything yet. Before Milestone 6 gives `send` a real
  execution path, it must share a command-safety check with the G-code
  streamer so `send "G28"` cannot bypass the `home` confirmation
  requirement. See [[milestone-1-cli-config]] §9.
- **Logging is entirely absent**, by design (Milestone 2 scope). All
  operator-facing text goes straight to the `out`/`err` streams passed
  into `printerctl_cli_run()`. This is intentional, not an oversight, but
  it means Milestone 2 needs real call sites (parse errors, config errors,
  safety rejections) to design its logging API against, which now exist.
- **Config file parser has a fixed line-content limit of
  `PRINTERCTL_CONFIG_LINE_MAX - 2` (510) characters, excluding the
  newline.** Reasonable for this milestone's five scalar keys; may need
  revisiting if a future section (e.g. APCUPSD) needs longer values.
- **`open_memstream`/`mkstemp` are POSIX**, used only in the two test
  binaries (never in production code), consistent with the Linux-only
  target platform noted in `Agents.md`.

## 7. Testing

Both suites pass under `ctest --test-dir <build-dir> --output-on-failure`,
on both compilers:

- GCC 16.1.1, `-std=c17 -Wall -Wextra -Wpedantic`: clean build (`cmake
  --build build --clean-first`), zero warnings, `printerctl_tests` and
  `printerctl_config_tests` both pass.
- Clang 22.1.6, same flags and clean-build procedure: zero warnings, both
  test binaries pass.

Manual smoke test of the built binary (`./build/printerctl`) confirmed:
`--help` documents every option, command and the home safety note;
`--version` prints `printerctl 0.1.0`; `home` without confirmation exits
`4`; `home --i-understand-no-endstops` exits `5` with the resolved config
echoed; `--dry-run home` (no confirmation) still exits `4`; `print`/`send`
parse and exit `5`; `--port=/dev/ttyACM0` (equals form) is rejected with
exit `2` and a message naming the correct syntax; `--config
/nonexistent` exits `3` with an explicit message.

Additionally, for the review fixes above: `--port ""`, `print ""` and
`send ""` each exit `2` with a message naming the empty value; a config
file containing `port=` (nothing after the `=`) exits `3` with "value for
'port' must not be empty"; and `--config /tmp` (a directory, not a
regular file) exits `3` with "error reading config file '/tmp': Is a
directory" instead of silently succeeding.

## 8. Next steps

Milestone 2 (Logging Foundation), per `roadmap.md` §5:

- Design the logging API against Milestone 1's real error/output call
  sites (`printerctl_cli_run()`'s parse errors, config errors, safety
  rejection, and the "not yet implemented" message).
- Decide plain-text vs. JSON Lines as the initial format.
- Add verbose-mode behavior — `config.verbose` already exists and is
  resolved correctly end-to-end, but nothing reads it yet.

Also carried forward, not part of Milestone 2 but recorded so it is not
lost: before Milestone 6 implements `send`'s execution path, define the
shared command-safety check that prevents `send "G28"` (or any equivalent)
from bypassing the `home` confirmation requirement (see
[[milestone-1-cli-config]] §9).
