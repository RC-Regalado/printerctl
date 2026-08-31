---
name: printerctl-testing
description: Apply printerctl milestone validation using deterministic tests, PTY/mock Marlin, fault injection, concurrency checks, sanitizers, regression protection, and safe hardware validation.
---

# Skill: printerctl-testing

## Purpose
Standardize milestone validation with deterministic tests, PTY/mock Marlin, fault injection, concurrency checks, sanitizers, regression protection, and safe hardware validation.

## When to use
Use for daemon/runtime, PLP, sockets, execution, serial, Marlin, signals/power, persistence, or concurrency changes.

## Required context
Read docs/testing.md, active milestone task, relevant design/as-built notes, and docs/safety.md.

## Invariants
- Test order: unit -> pure protocol/runtime -> PTY/mock Marlin -> fault injection -> sanitizers -> regression -> hardware smoke test when required.
- Never assume 1 write == 1 read. Test fragmentation, coalescing, EOF, EINTR, malformed input, and disconnects.
- PLP needs independent byte-level golden vectors, not only encoder/decoder round trips.
- Threaded changes need repeated start/stop, startup shutdown, worker faults, resource lifetime, and TSan when supported.
- Print integration must cover ACK happy path, Error:, Resend:, timeout, serial disconnect, pause/resume/cancel, client disconnect, second start, and low battery when in scope.
- New tests and the full existing suite must pass.

## Verification
Completion report states build/test commands and results, sanitizer results, guardrail results, skipped checks and reasons, hardware tests if any, untested paths, and technical debt.
