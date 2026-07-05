---
title: Milestone 1 Design - CLI Grammar and Configuration Model
milestone: 1
status: done
date: 2026-07-04
tags: [printerctl, milestone, design, cli, config]
---

# Milestone 1 Design — CLI and Configuration

Source of truth: [[Agents]], [[architecture]], [[roadmap]].

This note records the concrete design decisions behind the Milestone 1
implementation, ahead of the milestone report in
[[milestone-1|docs/milestones/milestone-1.md]].

## 1. Module split

Two independent static libraries, matching `architecture.md` §4's `cli` and
`config` module boundaries:

- `printerctl_config` (`include/printerctl/config.h`, `src/config/config.c`):
  configuration model, defaults, `key=value` file parsing, override
  application and precedence resolution. No knowledge of argv or CLI syntax.
- `printerctl_cli` (`include/printerctl/cli.h`, `src/cli/cli.c`): argv
  parsing and the `main()`-facing composition function. Depends on
  `printerctl_config` (`target_link_libraries(printerctl_cli PUBLIC
  printerctl_config)`) but `config` has no dependency back on `cli`.

Each module has its own CTest binary (`printerctl_config_tests`,
`printerctl_tests`), so the config parser is verified without any CLI
grammar involved, and vice versa.

## 2. Pure parser vs. composition

`printerctl_cli_parse(argc, argv, *out_request, *out_error)` is a pure
function: argv in, a `printerctl_cli_request_t` or a
`printerctl_cli_error_t` out. It never touches a file, never resolves
`--config`, never prints anything, and never allocates on the heap. This
makes the full argument grammar (global options, subcommands, per-command
arguments) unit-testable without stdout/stderr capture plumbing, and keeps
`printerctl_cli_run()` a thin composition: parse, resolve config, enforce
the `home` safety gate, render one line of output, return an exit code.

`main()` stays a two-line shim calling `printerctl_cli_run()`, unchanged
from Milestone 0.

## 3. Grammar

```text
printerctl [GLOBAL_OPTIONS] COMMAND [COMMAND_ARGS]
printerctl --help
printerctl --version
```

- `--help` and `--version` are standalone invocations: valid only as the
  sole argument (`argc == 2`), never combined with global options or a
  command. This was an explicit decision to keep their behavior trivially
  predictable rather than letting them short-circuit option parsing at any
  position.
- Global options accept only `--option VALUE`; `--option=VALUE` is
  rejected with a message that names the correct form (`reject_equals_form`
  in `cli.c`), rather than falling through to a generic "unrecognized
  option" error. Global options may appear only before `COMMAND` and at
  most once each — duplicates and anything left over after the command's
  own arguments are usage errors.
- `--port`'s value must be non-empty; an empty string (`--port ""`) is a
  usage error, not an accepted empty port. This mirrors the config file's
  `port` key (§6), which rejects an empty value the same way.
- Boolean flags (`--dry-run`, `--verbose`) only turn a value on; there is no
  `--no-dry-run`. Turning a flag off is only possible by not passing it (or
  by not setting it in a config file), consistent with "flags only
  activate" in the acceptance brief.
- Commands and their argument shapes:
  - `print FILE` — exactly one argument, not validated for existence in
    this milestone (that belongs to the G-code reader in Milestone 3), but
    an empty string is rejected as a usage error.
  - `send COMMAND` — exactly one argument (the shell already collapses `send
    "M115 X10"` into one argv element); Milestone 1 does not inspect its
    content beyond rejecting an empty string as a usage error.
  - `home [--i-understand-no-endstops]` — zero or one argument, and if
    present it must be exactly that flag.
  - `pause`, `resume`, `cancel`, `temp`, `position`, `info` — no arguments.

## 4. Ownership without heap

`printerctl_command_args_t.print_file` and `.send_command` are `const
char *` pointers straight into the `argv` passed to
`printerctl_cli_parse()` — no copy, no allocation. This is safe because
`printerctl_cli_run()` consumes them synchronously within the same
invocation that owns `argv` (which lives for the whole process in
`main()`), and the header documents the constraint explicitly: these
pointers must not be retained past the invocation that produced them.

Everything that *is* copied (port, config path) is copied into a
fixed-size buffer sized generously for real use
(`PRINTERCTL_CONFIG_PORT_MAX` = 256, `PRINTERCTL_CONFIG_PATH_MAX` = 4096).
Any value that does not fit is a hard error (`--port` value too long,
`--config` path too long, or a `port=` value too long inside a config
file) — never silent truncation. This is the direct implementation of the
"ownership without heap" recommendation: fixed, bounded storage, and
failure over truncation on overflow.

## 5. `config_overrides_t` and precedence

`printerctl_config_overrides_t` mirrors `printerctl_config_t` field for
field, with one `has_*` flag per field. This is what lets "the user typed
`--baud 115200`" (equal to the default) behave identically to "the user
typed `--baud 250000`" (different from the default): the flag records
*that a value was set*, independent of *what* it was set to. Without it,
applying overrides by comparing against defaults would silently treat an
explicit default value as "not set."

`printerctl_config_resolve()` implements the precedence from
`architecture.md` §6.2 directly:

```c
printerctl_config_set_defaults(out_config);           // 1. defaults
if (cli_overrides->has_config_path) {                  // 2. config file
    printerctl_config_load_file(...);
    printerctl_config_apply_overrides(out_config, &file_overrides);
}
printerctl_config_apply_overrides(out_config, cli_overrides); // 3. CLI
```

`printerctl_config_apply_overrides()` is the single place that applies a
sparse override set onto a resolved config, reused for both the file layer
and the CLI layer — there is exactly one code path for "does this field
win," not two.

## 6. Config file format (Milestone 1 subset)

Root-level `key=value` lines only:

```ini
port=/dev/ttyACM0
baud=115200
timeout=5000
dry_run=false
verbose=false
```

- Comments are full-line only (`#` as the first non-whitespace character);
  inline trailing comments after a value are not supported in Milestone 1,
  to avoid ambiguity about whether `#` could ever be a legal character
  inside a future value.
- Leading/trailing whitespace around the line, the key and the value is
  trimmed.
- Booleans must be exactly `true` or `false` — no `1`/`0`/`yes`/`no`
  aliases, to keep the format's error surface small and unambiguous.
- Recognized keys are exactly `port`, `baud`, `timeout`, `dry_run`,
  `verbose`. Anything else — an unrecognized key, a key repeated in the
  same file, a line without `=`, an empty key, an out-of-range or
  malformed value, an empty value for `port` (`port=` with nothing, or
  only whitespace, after the `=`), a value too long for its field, a
  line whose content exceeds `PRINTERCTL_CONFIG_LINE_MAX - 2` (510)
  characters excluding its trailing newline, or a file that cannot be
  opened — is a distinct, explicit `printerctl_config_status_t` value with
  a human-readable message and (where applicable) a 1-based line number.
- A read error on the file handle *after* `fopen()` already succeeded (for
  example, the path names a directory or other non-regular file) is
  reported as `PRINTERCTL_CONFIG_ERR_READ_FAILED` rather than silently
  treated as a clean end-of-file; `printerctl_config_load_file()` checks
  `ferror()` once its read loop ends. If a parse error was already found on
  an earlier line, that error is kept and the read-error check is not
  reached — a parse error always takes priority over a later read failure.
- There is no implicit config path. A config file is only read when
  `--config PATH` is given explicitly; there is no `~/.printerctlrc` or
  similar auto-discovery in Milestone 1.

**Forward compatibility with Milestone 10.** `architecture.md` describes an
`[apcupsd]` section (`enabled`, `on_powerfail`, `on_online`,
`auto_resume`). This parser only understands bare `key=value` lines at
the root; a line like `[apcupsd]` does not start with `#` and has no `=`,
so today it is rejected as `PRINTERCTL_CONFIG_ERR_MALFORMED_LINE`. That is
intentional and safe — Milestone 1 has no section parser to route it to —
but it means Milestone 10 must add section-header recognition to this same
loop (or a layer above it) rather than assuming root-level keys and
sectioned keys are already compatible.

## 7. Exit codes

Fixed, public and stable (`include/printerctl/cli.h`):

| Value | Meaning |
|---|---|
| 0 | Success, including `--help`/`--version` |
| 2 | Usage error (bad argv) |
| 3 | Configuration error (bad `--config` file) |
| 4 | Safety policy rejection (`home` without confirmation) |
| 5 | Parsed and resolved successfully, but not yet implemented |

No ranges are reserved for future categories; a new category added later
gets the next unused integer. Value `1` is intentionally left unused here
even though Milestone 0 used it for "usage error" — Milestone 1 redefines
the taxonomy from scratch per the roadmap's own acceptance criterion ("CLI
tests cover valid and invalid commands" implies a settled taxonomy), and
`1` is conventionally reserved by many CLI tools for "general/unexpected
error," which this project does not currently raise deliberately.

## 8. Safety: `home` confirmation lives only in the CLI

`--i-understand-no-endstops` is parsed as part of the `home` command's
arguments in `cli.c`; there is no equivalent config-file key, and
`printerctl_config_t` has no field for it. This is deliberate: a
confirmation that could be silently satisfied by a config file (which
could be checked into a repo, copied between machines, or forgotten about)
would defeat the purpose of requiring an explicit, per-invocation
acknowledgement. The safety check runs in `printerctl_cli_run()` after
config resolution but before the "not yet implemented" fallback, and does
not consult `config.dry_run` at all — `--dry-run home` without
confirmation still returns `PRINTERCTL_EXIT_SAFETY_ERROR` (4), exactly
like a real invocation. Dry-run affects whether commands touch the serial
port (once Milestone 5+ exists); it must never affect whether a
destructive command is allowed to be *considered* confirmed.

## 9. `send` and G28 (deferred safety note)

`send COMMAND` in Milestone 1 never inspects `COMMAND`'s content — it is
parsed as an opaque string and, like every other valid command, resolved
against config and then rejected with `PRINTERCTL_EXIT_UNAVAILABLE` (5)
because nothing executes yet. This is safe *today* only because nothing
executes. Before Milestone 6 (`send` starts actually writing to the
serial port), the command-safety policy needs a shared check — reachable
from both the G-code streamer (`gcode`/`printer`, Milestone 3/7) and
`send` — so that `send "G28"` is held to the same
`--i-understand-no-endstops` confirmation as the `home` command. Milestone
1 does not implement this because there is no execution path yet to guard;
recorded here so Milestone 6 does not ship `send` as an unguarded bypass of
the `home` safety policy.

## 10. Deviation from `architecture.md`'s configuration sketch

`architecture.md` §6.2 sketches `printerctl_config_t` with `const char
*port` and two APCUPSD fields. This implementation instead uses a
fixed-size `char port[256]` (see §4 above) and omits
`apcupsd_enabled`/`apcupsd_auto_resume` entirely — those belong to
Milestone 10 and adding them now, unused and untested, would be dead
configuration surface. Both deviations are scoped to Milestone 1 and
recorded here rather than as an update to `architecture.md`, since
`architecture.md` explicitly frames that struct as an "initial
configuration model."

## 11. Logging

`Agents.md` and `architecture.md` both list logging as a required
capability, but it is explicitly Milestone 2's deliverable
("Logging Foundation"). Milestone 1 has no logging calls anywhere —
`printerctl_cli_run()` writes directly to the `out`/`err` streams given to
it, which is operator-facing output, not a log. Wiring logging through
`cli`/`config` is deferred so Milestone 2 can design the logging API
against real call sites (parse errors, config errors, safety rejections,
the "not yet implemented" fallback) without Milestone 1 having guessed at
an API that gets thrown away.
