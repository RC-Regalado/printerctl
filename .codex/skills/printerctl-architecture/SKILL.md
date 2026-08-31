---
name: printerctl-architecture
description: Preserve printerctl architectural boundaries, ownership, dependency direction, and milestone scope while evolving CLI, daemon, protocol, execution, and future clients.
---

# Skill: printerctl-architecture

## Purpose
Preserve printerctl architectural boundaries during evolution toward printerctld.

## When to use
Use for daemon, runtime, PLP dispatch, print-engine integration, CLI migration, queue, TUI/Web, and multi-printer work.

## Required context
Read Agents.md, architecture.md, roadmap.md, docs/safety.md, docs/testing.md, the active milestone task, and current design/as-built notes.

## Invariants
- Dependency direction: clients -> PLP -> printerctld -> runtime/worker -> print_engine -> printer -> serial.
- Never introduce core -> daemon/socket/PLP, printer -> daemon, serial -> daemon, or protocol -> printer/serial dependencies.
- Control context owns daemon lifecycle, transport dispatch, public snapshot, and worker availability.
- Worker owns mutable execution state.
- Control must not directly mutate print_engine. Worker must not directly mutate the public snapshot.
- Reuse print_engine as print lifecycle authority; never duplicate streaming, interruption recording, or cancellation lifecycle in daemon code.
- Do not implement future milestone capabilities early. A second PRINT_START during M16 returns BUSY; it is not queued.
- Prefer incremental migration over rewrites.

## Workflow
1. Read active milestone and architecture sources.
2. Identify responsibility owner before coding.
3. Check dependency direction.
4. Implement only active scope.
5. Run build/tests/guardrails.
6. Update as-built documentation when implementation differs from design.

## Verification
Confirm no lower layer gained daemon/UI/transport dependencies, execution has one mutable owner, lifecycle logic is not duplicated, future features were not added, regressions pass, and docs reflect as-built behavior.
