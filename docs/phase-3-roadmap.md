# Phase 3 Roadmap — Safe Macros

## Phase status

```text
Phase 2 — DONE
Phase 3 — ACTIVE
M27 — DONE
M28 — DONE (operator acceptance 2026-10-18; see docs/audit/m28-remediation.md)
M29 — ACCEPTED BY OPERATOR (2026-10-18), technical fixes pending
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

**Status:** DONE — operator acceptance of the remediation on 2026-10-18,
recorded in `docs/audit/m28-remediation.md`. M29 acceptance remains separate.

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

## M29 — Macro Public Surface and PrusaSlicer Configuration Import

**Status:** ACCEPTED BY OPERATOR on 2026-10-18; technical closure pending
M29-R1 through M29-R5 from `docs/audit/m29-review.md`. Operator acceptance
does not constitute evidence that those findings are fixed.

Adds PLP `MACRO_LIST`/`MACRO_RUN`, the client wrappers, CLI `macro list` /
`macro run ID`, and `config from-prusa` to derive `calibration.*` keys from a
PrusaSlicer printer profile or sliced G-code. Still no user-defined macros,
DSL, arbitrary G-code, MACRO_CANCEL, queue, events, TUI/Web or multi-printer.
See `docs/milestones/milestone-29.md`.

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
