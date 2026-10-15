# Phase 3 Roadmap — Safe Macros

## Phase status

```text
Phase 2 — DONE
Phase 3 — ACTIVE
M27 — DONE
M28 — IN PROGRESS
```

## Phase objective

Introduce daemon-owned, typed and validated built-in macros without turning
macros into an arbitrary G-code bypass.

The architectural rule is:

```text
client
→ macro identifier
→ daemon-owned definition
→ typed validated steps
→ worker execution
```

Not:

```text
client
→ arbitrary G-code list
```

## M27 — Macro Domain & Safety Contract

**Status:** DONE

M27 introduces the pure macro domain model, validation rules and built-in
registry metadata.

M27 does not implement:

- physical macro execution;
- PLP `MACRO_*` messages;
- CLI macro commands;
- user-defined macro files;
- macro parser/DSL;
- queue/jobs/history;
- event subscriptions;
- TUI/Web;
- multi-printer support.

## Planned first built-ins

Stable IDs:

- `calibrate-front-left`
- `calibrate-front-right`
- `calibrate-center`
- `calibrate-back-left`
- `calibrate-back-right`
- `park`

Because machine geometry is not yet an authoritative configuration concept,
M27 records these as non-executable placeholders. Future milestones must provide
safe geometry/configuration before adding executable steps.

## M28 — Built-in Macro Execution

**Status:** IN PROGRESS

M28 adds worker-owned physical execution for validated built-in MOVE macros
only.

It proves:

```text
registry
→ complete validation
→ runtime ownership validation
→ worker-owned macro execution
→ typed MOVE translation
→ printer/Marlin
```

M28 does not add:

- PLP `MACRO_*` messages;
- CLI macro UX;
- user-defined macro files;
- arbitrary G-code lists;
- queue/jobs/history;
- event subscriptions;
- TUI/Web;
- multi-printer support.

Executable built-ins in M28:

- `maintenance-z-lift`
- `calibrate-front-left`
- `calibrate-front-right`
- `calibrate-center`
- `calibrate-back-left`
- `calibrate-back-right`

`park` remains registry metadata only until a later milestone gives it a safe
typed definition.

## Future direction

Later Phase 3 milestones may add daemon execution and observability, but must
preserve the M27 contract:

- stable IDs are protocol/API identifiers;
- display names/descriptions are presentation only;
- macro definitions are daemon-owned;
- complete definitions validate before first physical step;
- execution is sequential and non-transactional;
- client disconnect is not cancellation;
- cancellation stops authorization of future steps only.
