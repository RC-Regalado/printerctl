# Phase 2 Testing Strategy

## Principle

Every milestone must leave the repository buildable and testable.

Migration must be proven as an ownership change, not only as functional equivalence.

## Layers

### Unit
- config ownership;
- client serialization/parsing;
- response mapping;
- CLI argument mapping;
- safety validation.

### Protocol
For every new PLP operation:
- golden request;
- golden response;
- fragmented frame;
- coalesced frames;
- malformed payload;
- unexpected type/version;
- invalid field values.

### Daemon integration
Use PTY/mock before hardware:
- worker routing;
- queries;
- physical command errors;
- cancellation;
- disconnect;
- second client where required.

### Process-level
Real `printerctl` + real `printerctld` + PTY/mock.

Assert:
- stdout/stderr;
- exit status;
- daemon survival;
- serial ownership;
- session behavior;
- cleanup.

### Safety
Before hardware:
- raw G28 rejection;
- explicit HOME path;
- no automatic G28/G0/G1/M104/M109/M140/M190/M112/M410;
- R6 cancellation barrier preserved;
- SIGPWR preserved;
- no auto-resume.

### Sanitizers
- ASan;
- UBSan;
- TSan on concurrency-relevant subset.

### Boundaries
At M25/M26 prove CLI target has no direct dependency on:
- serial;
- printer;
- direct Marlin exchange;
- host execution engine.

## Milestone tests

### M18
- daemon config valid/invalid;
- client config valid/invalid;
- cross-owned key policy;
- dry-run with daemon down.

### M19
- PING/VERSION/STATUS through client API;
- absent socket;
- reset;
- truncated response;
- request ID mismatch;
- incompatible PLP version.

### M20
- temp success/failure/missing report;
- position success/failure/missing report;
- info success/failure/missing report;
- verify client does not open serial.

### M21
- send success;
- send G28 rejected;
- home all/X/Y/Z;
- SD pause/resume/cancel;
- printer Error/Resend/timeout mapping;
- no policy bypass.

### M22
- print happy path;
- unreadable/malformed source;
- serial unavailable;
- disconnect after START accepted;
- new client observes active print;
- terminal summary;
- no client-side print_engine ownership.

### M23
- pause;
- repeated pause;
- resume;
- repeated resume;
- cancel;
- second-client control;
- Ctrl+C observer;
- SD namespace semantics.

### M24
- no active print low-battery;
- active SIGPWR;
- repeated low-battery;
- daemon marker PID;
- no auto-resume;
- R6 no-new-TX guarantee.

### M25
- build graph rejects direct serial ownership in CLI;
- migrated command cannot fallback to serial if daemon down.

### M26
Run full Phase 2 scenario matrix plus legacy regression suite.

## Hardware policy

Hardware tests only after PTY/mock passes.

No automated test may add unapproved motion, homing, heating, extrusion, emergency-stop, or resume behavior.
