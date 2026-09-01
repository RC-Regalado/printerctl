# M19 Design — PLP Client Library

## Objective

Create reusable client transport/protocol abstraction for `printerctl`.

## Required properties

Separate:
- connection lifecycle;
- request construction;
- request ID generation;
- send/write-all;
- receive/incremental parse;
- correlation;
- error classification.

CLI handlers must not own byte-level framing.

## Failure classes

At minimum:
- socket unavailable;
- connection refused/reset;
- bounded wait failure;
- malformed PLP;
- unsupported version;
- request ID mismatch;
- remote status error.

Preserve structured errors for CLI exit-code mapping.

## Existing protocol reuse

Reuse existing PLP serializer/parser where appropriate.

Do not duplicate framing logic.

## Scope

Prove using existing non-physical operations such as PING/VERSION/STATUS.

No new hardware operation belongs here.

## Implementation

Implemented as `printerctl_client`, a reusable Unix-socket PLP/1 client
layer.

Responsibilities:

- connect/close lifecycle;
- monotonic non-zero request IDs;
- PLP request serialization through the existing codec;
- write-all over `send(..., MSG_NOSIGNAL)`;
- bounded `poll()` wait;
- incremental response parsing through the existing parser;
- response type and request ID correlation;
- remote status, malformed protocol, timeout and I/O error
  classification.

M19 does not migrate CLI commands and does not add PLP operations.
