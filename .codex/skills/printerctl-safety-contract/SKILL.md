---
name: printerctl-safety-contract
description: Preserve printerctl physical-safety guarantees for execution, serial ownership, signals, power handling, cancellation, and any code capable of affecting printer behavior.
---

# Skill: printerctl-safety-contract

## Purpose
Preserve physical-safety guarantees while modifying execution, daemon, serial, signals, power, or print control.

## When to use
Mandatory for print_engine, printer, serial ownership, daemon execution, pause/resume/cancel, signals, APCUPSD, recovery, or any code capable of physical printer action.

## Required context
Read docs/safety.md, Agents.md, architecture.md, active milestone task, APCUPSD documentation, and current print-engine design/as-built notes.

## Invariants
Never generate automatically: homing, movement, heating, extrusion, emergency stop, print resume after recovery/restart/power restoration, or automatic worker restart.

Cancellation means stop host transmission and use existing cleanup semantics. Do not invent M112, M410, homing, parking, cooldown, retraction, or other cleanup G-code unless a future approved decision explicitly changes policy.

Persisted state is evidence, not authority to resume. Client reconnect, daemon restart, host reboot, serial reconnect, or power restoration must not auto-resume a print.

Preserve low-battery behavior and ensure at most one effective serial owner.

## Workflow
1. Trace external intent to serial output.
2. Inspect automatic transitions and failure/shutdown paths.
3. Verify low-battery and cancellation semantics.
4. Run hardware-independent tests first.
5. Hardware smoke tests require explicitly approved safe input.

## Verification
No new automatic physical action, no unapproved cancel G-code, no auto-resume, unambiguous serial ownership, low-battery behavior intact, failure paths tested.
