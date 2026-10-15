# M27 Design — Macro Domain & Safety Contract

## Objective

M27 defines a typed, bounded, daemon-owned macro domain for future built-in
calibration and maintenance sequences.

It produces zero physical effects.

## Domain model

Implemented in:

- `include/printerctl/macro.h`
- `src/macro/macro.c`

Core structs:

- `printerctl_macro_definition_t`
  - stable `id`;
  - `display_name`;
  - `description`;
  - `category`;
  - `required_state`;
  - `capabilities`;
  - ordered `steps`.
- `printerctl_macro_step_t`
  - `type`;
  - explicit `axes`;
  - `x`, `y`, `z`;
  - `feedrate_mm_min`.

Bounds:

- `PRINTERCTL_MACRO_ID_MAX = 64`
- `PRINTERCTL_MACRO_DISPLAY_NAME_MAX = 96`
- `PRINTERCTL_MACRO_DESCRIPTION_MAX = 256`
- `PRINTERCTL_MACRO_STEPS_MAX = 32`

## Ownership

Macro definitions are daemon-owned domain data.

Future clients must request macros by stable id only:

```text
client
→ macro id
→ daemon registry lookup
→ validated typed steps
→ worker execution
```

Clients must not submit arbitrary G-code lists, scripts or raw macro bodies.

## Categories

The domain defines:

- `CALIBRATION`
- `MAINTENANCE`
- `MOTION`
- `THERMAL`
- `FILAMENT`
- `DIAGNOSTIC`

Only MOVE-capable placeholders exist in M27.

## Capabilities

Defined capability bits:

- `MOVE`
- `HEAT`
- `EXTRUDE`
- `FAN`

Capabilities are explicit authorization metadata. They are not inferred from
G-code strings.

M27 validates that MOVE steps require the MOVE capability.

## Step types

M27 defines:

- `MOVE_ABSOLUTE`
- `MOVE_RELATIVE`

Each movement step carries:

- involved axes;
- explicit coordinates/displacement;
- explicit positive feedrate.

M27 deliberately does not define:

- `RAW_GCODE`
- `SCRIPT`
- `SHELL`
- `LOOP`
- `CONDITION`
- `VARIABLE`
- `MACRO_CALL`

## Required execution state

M27 defines:

- `IDLE_ONLY`

Future execution must reject macro start while `PRINT` or another execution is
active.

## Modal-state policy for M28

The future executor must not depend on inherited printer modal state.

Expected executor contract:

```text
MOVE_ABSOLUTE
→ deterministic absolute-coordinate semantics

MOVE_RELATIVE
→ deterministic relative-coordinate semantics
```

Feedrate must always come from the step. The executor must not rely on inherited
modal feedrate.

Postcondition choice: after macro execution or failure, the executor should leave
the printer in a deterministic known coordinate mode selected by the executor,
rather than trying to restore an unknown previous mode.

## Validation contract

`printerctl_macro_validate_definition()` validates a complete executable
definition before any future physical step may be authorized.

Validation covers:

- stable id present and syntactically bounded;
- display name and description present;
- valid category;
- valid capability mask;
- valid required state;
- step count in `1..PRINTERCTL_MACRO_STEPS_MAX`;
- known step types;
- required axis fields present;
- finite numeric coordinates;
- finite positive feedrate;
- step type consistent with capabilities.

Machine-bound validation is intentionally absent because machine dimensions are
not yet authoritative configuration.

## Registry

`printerctl_macro_builtin_count()`, `printerctl_macro_builtin_at()` and
`printerctl_macro_builtin_find()` expose deterministic built-in metadata.

M27 built-ins:

- `calibrate-front-left`
- `calibrate-front-right`
- `calibrate-center`
- `calibrate-back-left`
- `calibrate-back-right`
- `park`

These are non-executable placeholders with zero steps. They are intentionally
not valid executable definitions until a future milestone provides safe
machine-geometry-backed steps.

`printerctl_macro_validate_registry()` validates unique stable IDs.

## Sequential and non-transactional semantics

Future macro execution is sequential and non-transactional.

If execution fails at step `N`:

```text
steps < N may already have physical effects
steps > N must not be newly authorized
```

There is no automatic rollback.

## Cancellation semantics

Future macro cancellation means:

```text
stop authorization of future macro steps
```

It must not automatically send:

- `M112`
- `M410`
- `HOME`
- movement;
- recovery;
- resume.

The Phase 1 R6 transmission barrier remains authoritative.

## Client lifecycle semantics

Future accepted macro execution is daemon-owned:

```text
client disconnect != macro cancellation
```

M27 does not add async jobs, operation IDs or event subscriptions.

## Observability direction

Future status projection should extend the unified execution model:

```text
execution.kind = PRINT | MACRO
execution.state = ...
```

Macro-specific projection should remain daemon/control-owned:

```text
macro id
current step
total steps
```

Socket and CLI layers must not own an independent mutable macro state machine.

## Safety

M27 adds no path that can:

- open serial for macros;
- send G-code;
- move axes;
- home;
- heat;
- extrude;
- send `M112`;
- send `M410`;
- resume or recover automatically.

## M28 update

M28 builds on this domain with worker-owned execution for selected built-in
MOVE macros. The M27 domain contract remains authoritative: clients still do
not submit arbitrary macro bodies or raw G-code lists.
