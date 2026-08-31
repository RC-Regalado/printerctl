---
title: Printerctl Local Protocol v1 (PLP/1)
tags: [printerctl, protocol, plp1, design, reference, milestone-14]
status: done
date: 2026-08-21
---

# Printerctl Local Protocol v1 (PLP/1)

Source of truth: [[Agents]], [[docs/architecture|architecture]] §10,
`docs/roadmap.md`'s M14 section. Implemented by
`include/printerctl/protocol/plp1.h`/`plp1_parser.h` and
`src/protocol/plp1.c`/`plp1_parser.c`; see
[[milestone-14|docs/milestones/milestone-14.md]] for the implementation
report and verification results.

This is a durable, topic-named reference (like `docs/safety.md`/
`docs/testing.md`), not a per-milestone design note: M15-M17 all depend
on this document staying the single source of truth for the wire
contract, independent of which milestone happens to touch it next.

## 1. Scope and identity

**Printerctl Local Protocol v1**, short name **PLP/1**: the binary,
length-prefixed, big-endian wire contract local clients use to talk to
the future `printerctld` over `AF_UNIX`/`SOCK_STREAM`. Milestone 14
implements the codec only — encode, decode, and an incremental
byte-stream parser — entirely against in-memory buffers. It never opens
a socket, never knows a file descriptor, thread or pipe, and never
touches `print_engine`, `printer` or `serial`; Milestone 15 connects it
to a real transport without changing this contract.

## 2. Wire header

Every PLP/1 frame:

```text
Offset  Size  Field
────────────────────────────────────
0       4     frame_length
4       1     version
5       1     message_type
6       2     flags
8       4     request_id
12      2     status
14      2     reserved
16      N     payload
```

Header size including `frame_length`: 16 bytes
(`PRINTERCTL_PLP1_HEADER_SIZE`). Header size after `frame_length`: 12
bytes (`PRINTERCTL_PLP1_MIN_FRAME_LENGTH`).

## 3. `frame_length`

`uint32`, big-endian. Represents the number of bytes **after** the
`frame_length` field itself:

```text
total_frame_size = 4 + frame_length
payload_size     = frame_length - 12
```

Valid range: `PRINTERCTL_PLP1_MIN_FRAME_LENGTH` (12) to
`PRINTERCTL_PLP1_MAX_FRAME_LENGTH` (65536), enforced by
`printerctl_plp1_validate_frame_length()` before any other parsing
happens — this is the check a stream parser runs on just the first 4
bytes, before deciding how many more bytes to wait for.
`PRINTERCTL_PLP1_MAX_FRAME_SIZE` (65540) is the corresponding total
buffer size a caller needs to guarantee one full frame always fits.

## 4. Endianness

Every multi-byte field — `frame_length`, `flags`, `request_id`,
`status`, `reserved`, and every multi-byte field inside a payload
(string lengths, bitmaps, numeric fields) — is big-endian. Nothing is
serialized by writing a C struct's raw memory
(`write(&struct, sizeof(struct))`): `printerctl_plp1_frame_t` has no
wire-compatible layout guarantee across compilers/platforms, so
`plp1.c` packs and unpacks every field byte-by-byte through small
`static` helpers (`write_u16be`/`write_u32be`/`read_u16be`/`read_u32be`).

## 5. Version

`PRINTERCTL_PLP1_VERSION_1 = 0x01`. `version != 1` decodes as
`PRINTERCTL_PLP1_UNSUPPORTED_VERSION`. No major/minor negotiation is
implemented in M14.

## 6. Message type registry

```text
0x00–0x0F protocol/runtime
0x10–0x1F printer queries
0x20–0x2F print control
0x30–0x3F jobs       reserved
0x40–0x4F events     reserved
0x50–0x5F history    reserved
0x60–0x7F future
0x80–0xFF reserved
```

Implemented in Phase 1:

| ID | Name |
|---|---|
| 0x01 | PING |
| 0x02 | VERSION |
| 0x03 | STATUS |
| 0x20 | PRINT_START |
| 0x21 | PRINT_PAUSE |
| 0x22 | PRINT_RESUME |
| 0x23 | PRINT_CANCEL |

These IDs are part of the wire contract and must never be reused for an
unrelated operation once published. A `message_type` outside this list
decodes as `PRINTERCTL_PLP1_UNKNOWN_MESSAGE_TYPE`. Extending the
registry means updating both this table and the `message_type_known()`
switch in `plp1.c`.

## 7. Flags

`uint16` bitmap:

| Bit | Value | Name |
|---|---|---|
| 0 | 0x0001 | REQUEST |
| 1 | 0x0002 | RESPONSE |
| 2 | 0x0004 | ERROR |
| 3 | 0x0008 | HAS_PAYLOAD |
| 4 | 0x0010 | EVENT |
| 5 | 0x0020 | MORE |
| 6-15 | — | reserved |

Exactly one of REQUEST/RESPONSE/EVENT must be set; anything else (none,
or more than one) decodes as `PRINTERCTL_PLP1_INVALID_FLAGS`. Any bit
outside `PRINTERCTL_PLP1_FLAG_KNOWN_MASK` (0x003F) set also decodes as
`INVALID_FLAGS` — unknown bits are never silently ignored. EVENT and
MORE are reserved with no functional semantics in Phase 1.
`printerctl_plp1_encode_frame()` automatically OR's `HAS_PAYLOAD` into
the written flags whenever the frame's payload is non-empty; callers do
not set it by hand — and both `encode_frame()` and `decode_frame()`
reject a frame where `HAS_PAYLOAD` disagrees with whether a payload is
actually present, as `INVALID_FLAGS`.

**Role vs. message type.** Every Phase 1 message type (§6) is a plain
request/response exchange — none of them is an event-family message
(the 0x40-0x4F range is reserved but empty in M14) — so `EVENT`
combined with any message type defined today is rejected as
`INVALID_FLAGS`, at both `encode_frame()` and `decode_frame()`. This is
checked separately from, and in addition to, the generic
"exactly-one-role-bit" structural check above: a frame can have
perfectly valid *flags shape* (exactly one role bit) and still be
rejected because that role is not one the specific `message_type`
supports yet.

**Mandatory payload-per-message/role validation.** Beyond the generic
`HAS_PAYLOAD`-vs-actual-presence check above, `decode_frame()` (and
`encode_frame()`, as a caller-input check) also enforces, structurally
and unconditionally, whether a *specific* `(message_type, role)`
combination must carry a payload, must not, or may either way — this is
not left to whichever payload-specific decoder or
`printerctl_plp1_expect_no_payload()` call a caller happens to remember
to invoke afterward; a malformed frame is rejected at `decode_frame()`
itself, before any payload-specific parsing is even attempted:

| Message type | Request | Response (success) |
|---|---|---|
| PING | forbidden | forbidden |
| VERSION | forbidden | **required** |
| STATUS | **required** | **required** |
| PRINT_START | **required** | forbidden |
| PRINT_PAUSE / RESUME / CANCEL | forbidden | forbidden |

"Required" for `PRINT_START`'s request means a payload of at least 2
bytes (the string's length prefix) must be present — even an *empty*
`source_path` still needs those 2 bytes; a request with no payload at
all (not even the length prefix) is rejected here, before
`printerctl_plp1_decode_print_start_request()` is ever reached.

**`ERROR` overrides the table above.** Any `RESPONSE` with the `ERROR`
flag set — regardless of `message_type` — may carry the generic error
payload (§19) or may carry none; both are valid, for every message
type including ones that otherwise forbid a payload (e.g. an
`ERROR`-flagged `PING` response may legitimately carry an error
message). This check is evaluated first, ahead of the per-message-type
table, precisely so it can override it.

All rejections from this table use
`PRINTERCTL_PLP1_INVALID_PAYLOAD`, the same status the payload-specific
decoders already use for their own internal shape/length problems — the
two checks are complementary layers (frame-level presence/absence here,
payload-level shape in the specific decoder), not competing sources of
truth.

## 8. `request_id`

`uint32`, big-endian. `0` is reserved for uncorrelated messages
(principally events); a `REQUEST` or `RESPONSE` frame carrying
`request_id == 0` is rejected as `PRINTERCTL_PLP1_MALFORMED_FRAME`, at
both `encode_frame()` (caller misuse) and `decode_frame()` (malformed
wire content) — `0` is what the reservation exists to protect, so a
correlated frame claiming it is treated the same as any other framing
inconsistency, not silently accepted. `EVENT` is exempt (0 is exactly
what it is reserved for); since no Phase 1 message type actually
supports `EVENT` yet, this exemption is currently only observable as
"the request_id check does not additionally fire" rather than as a
frame that fully succeeds — see
`tests/test_plp1_frame.c`'s `test_request_id_zero_rejected()`, which
compares an `EVENT` frame with `request_id 0` against one with a
non-zero `request_id` and confirms both fail identically (for the
role/type reason only).

Beyond the zero/non-zero distinction, **the codec does not generate or
manage request IDs** — sequencing and wraparound handling remain the
future client's responsibility, per the task's own explicit
instruction.

## 9. `status`

`uint16`, big-endian, decoded as a raw value (not the
`printerctl_plp1_wire_status_t` enum) so a frame may carry a status this
build does not yet recognize (forward compatibility) — callers compare
it against the enum constants explicitly.

Consistency, enforced by both `encode_frame()` (as caller-input
validation, `PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT`) and `decode_frame()`
(as wire validation, `PRINTERCTL_PLP1_MALFORMED_FRAME` — no more
specific status code exists in the registry for this particular
inconsistency):

- A request's `status` MUST be `SUCCESS` (0x0000).
- A response's `ERROR` flag must be set if and only if `status !=
  SUCCESS`.

## 10. Status codes

| Code | Name |
|---|---|
| 0x0000 | SUCCESS |
| 0x0101 | MALFORMED_FRAME |
| 0x0102 | UNSUPPORTED_VERSION |
| 0x0103 | UNKNOWN_MESSAGE_TYPE |
| 0x0104 | INVALID_FLAGS |
| 0x0105 | INVALID_PAYLOAD |
| 0x0106 | FRAME_TOO_LARGE |
| 0x0201 | INVALID_ARGUMENT |
| 0x0202 | INVALID_STATE |
| 0x0203 | BUSY |
| 0x0204 | NOT_PRINTING |
| 0x0301 | WORKER_UNAVAILABLE |
| 0x0302 | INTERNAL_ERROR |
| 0x0303 | SHUTTING_DOWN |
| 0x0401 | PRINTER_ERROR |
| 0x0402 | SERIAL_ERROR |
| 0x0403 | PRINTER_TIMEOUT |
| 0x0501 | FILE_NOT_FOUND |
| 0x0502 | FILE_NOT_READABLE |
| 0x0503 | INVALID_GCODE_SOURCE |

`printerctl_plp1_wire_status_t` in `plp1.h` fixes these as the enum's
own numeric values, so encoding a status is a direct write of the enum
and decoding needs no lookup table. M14 only defines and serializes
these codes — deciding *when* the future daemon uses a domain error
(0x02xx) versus a printer error (0x04xx) is out of scope here.

## 11. `reserved`

`uint16`, must be `0`. A non-zero value decodes as
`PRINTERCTL_PLP1_MALFORMED_FRAME`. Not used for any extension yet.

## 12. Structural vs. semantic errors

This distinction is about *which stage* detected the problem, not a
property of the status code value (the same code, `MALFORMED_FRAME`,
covers both an out-of-range `frame_length` and a non-zero `reserved`
field — one structural, one semantic):

- **Structural/framing failure** — `frame_length` out of range, or a
  `buffer_length` not matching the declared frame exactly (impossible/
  truncated frame). Detected by
  `printerctl_plp1_validate_frame_length()`, called by the parser on
  just the first 4 bytes, *before* the frame boundary is established.
  The byte stream itself is no longer trustworthy for framing — a
  transport should stop reading frames from this connection.
  `printerctl_plp1_parser_take_frame()` reports this as
  `PRINTERCTL_PLP1_PARSER_ERR_MALFORMED` and does **not** advance past
  it (there is nothing sane to skip to).
- **Semantic frame failure** — unknown message type, invalid flags,
  non-zero reserved, invalid payload. Detected by
  `printerctl_plp1_decode_frame()` once the frame boundary is already
  known and trusted. The connection can stay open; a real daemon can
  respond with an error and keep going.
  `printerctl_plp1_parser_take_frame()` reports this as
  `PRINTERCTL_PLP1_PARSER_FRAME_REJECTED`, **after** advancing past the
  rejected frame, so the caller can keep parsing the next one.

M14 never closes a connection itself (it doesn't have one) — this
distinction exists so M15's transport knows which of its two possible
reactions (disconnect vs. respond-and-continue) applies.

## 13. Strings

`uint16` big-endian length, followed by that many raw UTF-8 bytes — no
NUL terminator on the wire.

```text
"test.gcode"  ->  00 0A 74 65 73 74 2E 67 63 6F 64 65
```

`printerctl_plp1_decode_string()` validates the length prefix is fully
present, the declared length does not exceed the available payload, and
the string fits the caller's destination buffer (rejecting with
`INVALID_PAYLOAD` rather than truncating — used for identifiers like
`PRINT_START`'s path, where truncation would silently name a different
file). The generic error payload's decoder
(`printerctl_plp1_decode_error_payload()`) is the one exception: it
truncates into the caller's buffer instead of erroring, because that
payload is informative text only. A `NULL` buffer with a `0` length is
accepted as a legitimate (if insufficient) input — a payload that is
entirely absent, not caller misuse — and falls straight through to the
same `INVALID_PAYLOAD` result a too-short buffer produces; only a
`NULL` buffer paired with a *non-zero* length is rejected as
`PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT`. `decode_string()` reports how
many bytes it consumed (`*out_consumed`); callers with nothing else in
the payload (`PRINT_START`, the generic error payload) additionally
require `*out_consumed` to equal the whole payload length, rejecting
any trailing bytes left over as `INVALID_PAYLOAD` — `decode_string()`
itself only validates the string field, not what, if anything, follows
it.

## 14. PING (0x01)

Request: `flags = REQUEST`, `request_id != 0`, `status = SUCCESS`, no
payload. Response: `flags = RESPONSE`, same `request_id`, `status =
SUCCESS`, no payload. Both validated with
`printerctl_plp1_expect_no_payload()`. The codec never auto-responds —
that is the future daemon's job.

### Golden vectors (`request_id = 42`)

Verified byte-for-byte by `tests/test_plp1_frame.c` in both directions
(`encode_frame()` producing these exact bytes, `decode_frame()`
recovering the exact field values from them — not merely a
self-consistent round trip through this implementation's own codec).

Request:

```text
00 00 00 0C   frame_length = 12
01            version = 1
01            message_type = PING
00 01         flags = REQUEST
00 00 00 2A   request_id = 42
00 00         status = SUCCESS
00 00         reserved = 0
```

Response:

```text
00 00 00 0C   frame_length = 12
01            version = 1
01            message_type = PING
00 02         flags = RESPONSE
00 00 00 2A   request_id = 42
00 00         status = SUCCESS
00 00         reserved = 0
```

## 15. VERSION (0x02)

Request: no payload. Response payload:

```text
uint8  protocol_version
uint32 capability_bitmap
```

Capability bits (`plp1.h`):

| Bit | Value | Name |
|---|---|---|
| 0 | 0x00000001 | PRINT_CONTROL |
| 1 | 0x00000002 | PRINTER_STATUS |
| 2 | 0x00000004 | EVENTS (reserved/future) |
| 3 | 0x00000008 | JOB_QUEUE (reserved/future) |
| 4 | 0x00000010 | HISTORY (reserved/future) |
| 5 | 0x00000020 | MULTI_PRINTER (reserved/future) |

M14 only defines the bits; the future daemon decides which it actually
publishes. No bit is assumed active by the codec.
`printerctl_plp1_decode_version_response()` requires the payload to be
exactly 5 bytes — neither shorter (including entirely absent) nor
longer — rejecting either as `INVALID_PAYLOAD`.

## 16. STATUS (0x03)

A STATUS response represents a snapshot **already known** to
`printerctld` — never a fresh Marlin probe. It never includes
temperature, physical position, firmware info, a fresh serial probe,
queue or history data, or printer enumeration.

Request payload: `uint32 requested_fields`, exactly 4 bytes — shorter,
absent or longer (trailing bytes) all decode as `INVALID_PAYLOAD`.
Response payload: `uint32 present_fields`, followed by the fields
present, always in canonical order regardless of the order requested.

### Field bitmap

| Bit | Value | Field |
|---|---|---|
| 0 | 0x0001 | daemon_state |
| 1 | 0x0002 | worker_state |
| 2 | 0x0004 | execution_state |
| 3 | 0x0008 | progress |
| 4 | 0x0010 | elapsed_time |
| 5 | 0x0020 | commands_sent |
| 6 | 0x0040 | commands_acknowledged |
| 7 | 0x0080 | source_position |
| 8 | 0x0100 | failure_reason |

Bits 9-31 are reserved
(`PRINTERCTL_PLP1_STATUS_FIELD_KNOWN_MASK = 0x01FF` covers bits 0-8).
Both `printerctl_plp1_decode_status_response()` (`present_fields`) and
`printerctl_plp1_decode_status_request()` (`requested_fields`) reject
any bit outside that mask, as `INVALID_PAYLOAD` — a decoder cannot
safely size or skip a *response* field it does not know how to parse,
so an unrecognized bit there is a hard failure by necessity; a
*request* setting a reserved bit is rejected too, for consistency with
the rest of this protocol's "no bit is ever silently ignored" rule
(§7) rather than being quietly tolerated as a harmless no-op. The two
**encoders** (`printerctl_plp1_encode_status_request()`/
`_encode_status_response()`) enforce the identical mask as their own
caller-input check (`PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT`) — a
reserved bit is rejected before it is ever written to a payload, not
merely when read back, so this codec can never itself produce a frame
its own decoder would then refuse.

### Canonical order and wire widths (M14 decisions)

The task specified the bitmap and canonical order but left field widths
open ("Definir representación wire solamente si resulta necesaria...").
M14 fixes them as follows, recorded here rather than left implicit:

| Field | Width | Notes |
|---|---|---|
| daemon_state | 1 byte | wire enum, see below |
| worker_state | 1 byte | wire enum, see below |
| execution_state | 1 byte | reserved wire enum; Milestone 16 defines real values |
| progress | 1 byte | 0-100 whole percent, matches `print_engine`'s `progress_percent` |
| elapsed_time | 4 bytes | seconds |
| commands_sent | 4 bytes | a print sending >4 billion commands is not physically plausible |
| commands_acknowledged | 4 bytes | same rationale |
| source_position | 4 bytes | source line number; a file with >4 billion lines is not plausible |
| failure_reason | 2 bytes | reuses `printerctl_plp1_wire_status_t`'s numeric space; 0 = none |

`daemon_state` wire enum: `0 UNKNOWN, 1 STARTING, 2 RUNNING, 3 STOPPING,
4 STOPPED, 5 FAILED`. `worker_state` wire enum: `0 UNKNOWN, 1
INITIALIZING, 2 READY, 3 STOPPING, 4 STOPPED, 5 FAULTED`. These are
deliberately separate C definitions from
`include/printerctl/daemon/runtime.h`'s `printerctl_daemon_state_t`/
`printerctl_daemon_worker_state_t` — `protocol/` does not include
`daemon/` (architectural isolation, Agents.md §10). `UNKNOWN = 0` has no
daemon-side equivalent; it exists only for a STATUS response describing
a field with no known value. A future dispatcher (M15/M16) is
responsible for mapping the daemon's own C enums onto these wire values.

`execution_state` was reserved by M14 and defined by M16:
`1 IDLE, 2 RUNNING, 3 PAUSED, 4 CANCELLING, 5 COMPLETED, 6 CANCELLED,
7 FAILED`.

A requested-but-absent field is not an error — it simply does not
appear in `present_fields`; `printerctl_plp1_decode_status_response()`
zeroes every field whose bit is absent rather than inventing a
placeholder value like `0` that could be mistaken for a real one. The
payload length must match `present_fields` **exactly** — both a payload
shorter than what it implies and one with trailing bytes left over
after the last declared field decode as `INVALID_PAYLOAD`; a completely
absent payload (`NULL`/0 bytes, e.g. a STATUS response sent with no
payload at all) decodes the same way, not as caller misuse.

## 17. PRINT_START (0x20)

Request payload: a single length-prefixed string, `source_path`. No job
ID, priority, queue position, printer ID, auto-resume flag or G-code
contents — the frame transports the reference, never the file; M14
never opens this path. An empty path decodes successfully as an empty
string (a codec-valid, if degenerate, value) — whether that is an
acceptable print request is Milestone 16 domain logic, not a codec
concern.

Success response: no payload. Means only "request accepted", never
"print finished" — the operational semantics belong to Milestone 16.

Implemented daemon error mapping after Phase 1 remediation:

| Condition | Status |
|---|---|
| A daemon-owned print is already active | `BUSY` |
| Worker command channel/runtime unavailable | `WORKER_UNAVAILABLE` |
| Source path does not exist | `FILE_NOT_FOUND` |
| Source path exists but is not readable | `FILE_NOT_READABLE` |
| Source path is not a regular G-code file or fails G-code validation | `INVALID_GCODE_SOURCE` |
| Serial open/configuration/startup failure | `SERIAL_ERROR` |
| Printer response error / unsupported resend / unexpected response | `PRINTER_ERROR` |
| Printer response timeout | `PRINTER_TIMEOUT` |

`BUSY` is reserved for the active-execution case only; clients must key
on the numeric status, never diagnostic text.

### Golden vector (`source_path = "/tmp/a.gcode"`, `request_id = 42`)

Verified byte-for-byte by `tests/test_plp1_frame.c`, same discipline as
§14's PING vectors.

Payload (14 bytes: 2-byte string length + 12-byte path):

```text
00 0C                                              string length = 12
2F 74 6D 70 2F 61 2E 67 63 6F 64 65                "/tmp/a.gcode"
```

`frame_length = 12 + 14 = 26 = 0x1A`. Full request frame:

```text
00 00 00 1A   frame_length = 26
01            version = 1
20            message_type = PRINT_START
00 09         flags = REQUEST | HAS_PAYLOAD
00 00 00 2A   request_id = 42
00 00         status = SUCCESS
00 00         reserved = 0
00 0C         string length = 12
2F 74 6D 70 2F 61 2E 67 63 6F 64 65   "/tmp/a.gcode"
```

## 18. PRINT_PAUSE / PRINT_RESUME / PRINT_CANCEL (0x21/0x22/0x23)

No payload on request or response, validated with
`printerctl_plp1_expect_no_payload()`. M14 defines only the empty-
payload contract; the approved *future* semantics (pause-while-paused
is idempotent success, resume-without-active-execution is
`NOT_PRINTING`, cancel-while-idle is `NOT_PRINTING`) are recorded here
for continuity but are **not implemented** by anything in M14 — no
state machine exists yet to enforce them.

## 19. Generic error payload

An `ERROR`-flagged response may carry:

```text
uint16 message_length
bytes[message_length]
```

UTF-8, informative only. Clients must never parse this text to
determine an error's class — `status` is the only programmatic signal.
`printerctl_plp1_decode_error_payload()` truncates into the caller's
*destination* buffer rather than requiring it to hold an
arbitrary-length message — but that is a separate concern from wire
validity: the *wire* payload itself must still match
`message_length` exactly (`2 + message_length` bytes, no more, no
less, no fewer), rejected as `INVALID_PAYLOAD` otherwise. Truncation
only ever happens against the caller's own buffer size, never against
what the wire declared.

## 20. Client disconnect contract

Documented here, enforced later: once a future `PRINT_START` is
accepted, the print's lifetime becomes independent of the initiating
client's connection lifetime. **Socket disconnect is not
`PRINT_CANCEL`.** M14 has no socket and no cancellation logic — this is
a forward-looking contract statement for M16/M17 to honor, not
behavior implemented here.

## 21. Parser requirements

`printerctl_plp1_parser_t` (`plp1_parser.h`) turns an arbitrary byte
stream into a sequence of complete frames without assuming any
relationship between how bytes arrive and where frame boundaries fall:
one frame split across many `feed()` calls, many frames in one `feed()`
call, and a complete frame followed by a partial next frame all work
(`tests/test_plp1_parser.c` exercises all three, plus several
deterministic arbitrary cut points, byte-at-a-time feeding, a
structural fault, a semantic rejection that still allows continued
parsing, and buffer-full partial consumption).

State model (conceptually `READ_LENGTH → VALIDATE_LENGTH → READ_FRAME →
PARSE_HEADER → VALIDATE_HEADER → PARSE_PAYLOAD → FRAME_READY`, per the
roadmap): implemented as two functions rather than an explicit state
enum — `printerctl_plp1_parser_feed()` only ever appends caller bytes
into a caller-provided buffer (compacting the unconsumed tail first),
and `printerctl_plp1_parser_take_frame()` re-checks from scratch each
call whether a complete frame is available, decoding it via
`printerctl_plp1_decode_frame()` once it is. `feed()` never hard-fails
on a large input chunk — it consumes as much as fits and reports
`*out_consumed`, so a real transport (M15) can always make forward
progress regardless of its own read chunk size, as long as the parser's
buffer is at least one `PRINTERCTL_PLP1_MAX_FRAME_SIZE`.

A frame handed back via `take_frame()`'s `out_frame->payload` borrows
directly from the parser's internal buffer and is valid only until the
next `feed()`/`take_frame()` call — the same "duration of the call"
discipline every borrowed pointer in this codebase already uses (e.g.
`printerctl_printer_line_fn` in `printer.h`).

## 22. Memory and ownership

Every function in `plp1.h`/`plp1_parser.h` takes caller-provided
buffers; nothing in `plp1.c`/`plp1_parser.c` calls `malloc`. No
allocation is ever driven directly by an unvalidated `frame_length` —
`validate_frame_length()` runs before any buffer sizing decision. The
parser's buffer is supplied by its caller (test files use a fixed
`uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE]` on the stack, matching
this codebase's stack-value convention throughout).

## 23. Versioning and compatibility

Only `version == 1` is accepted in M14; there is no negotiation. Status
codes, message type IDs and flag bits are additive-only once published
(existing values must never be reused for an unrelated meaning) — this
is a wire contract commitment, not merely a style preference, since a
client and `printerctld` may run different builds.

## 24. Deferred

- `AF_UNIX`/`SOCK_STREAM`, socket path/permissions, accept loop,
  fragmented-stream integration with a real transport — Milestone 15.
- Real daemon dispatch (translating a decoded frame into an internal
  command/event, per `include/printerctl/daemon/`) — Milestone 16.
- `execution_state`'s actual value set — Milestone 16, once
  `print_engine` is integrated into the worker.
- `PRINT_START`/`PAUSE`/`RESUME`/`CANCEL` state-transition semantics
  (idempotency, `NOT_PRINTING`, `BUSY` on a second start) — Milestone
  16.
- `JOB_*`, `QUEUE_*`, `HISTORY_*` message families, event subscriptions,
  multi-printer routing, WebSocket/HTTP representations — later phases,
  per `docs/roadmap.md` §4.
- Real client disconnect handling honoring §20's contract — Milestone
  16/17.
