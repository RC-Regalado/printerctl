---
title: Milestone 14 — Printerctl Local Protocol v1
milestone: 14
status: done
date: 2026-08-21
tags: [printerctl, milestone, protocol, plp1]
---

# Milestone 14 — Printerctl Local Protocol v1

Source of truth: [[Agents]], [[docs/architecture|architecture]]. Roadmap:
`docs/roadmap.md` (Phase 1). Full wire specification:
[[protocol-v1|docs/design/protocol-v1.md]] (durable reference, not a
per-milestone note — see that document's own header for why).

## 1. Technical summary

Milestone 13 built the daemon's internal runtime/IPC boundary. M14 adds
the *external* wire contract that boundary will eventually serve: PLP/1,
a binary, length-prefixed, big-endian protocol. This milestone is a pure
codec — encode a C-level frame description to exact wire bytes, decode
wire bytes into a validated frame, and an incremental parser that turns
an arbitrary byte stream into a sequence of complete frames — with zero
knowledge of sockets, pipes, threads, or `print_engine`. It is testable
entirely against in-memory buffers, which is exactly what its three test
binaries do.

This is the first binary/big-endian wire encoding in the codebase
(`grep -rn "htons\|ntohl" src include` found nothing before this
milestone) — implemented hand-rolled, byte-by-byte, deliberately never
serializing a C struct's raw memory to the wire.

## 2. Files changed

- `include/printerctl/protocol/plp1.h`, `src/protocol/plp1.c` (new) —
  constants, wire enums, `printerctl_plp1_frame_t`, header encode/decode/
  validation, the string codec, and payload codecs for VERSION, STATUS
  (request+response), PRINT_START and the generic error payload.
- `include/printerctl/protocol/plp1_parser.h`, `src/protocol/plp1_parser.c`
  (new) — the incremental byte-stream parser
  (`printerctl_plp1_parser_feed()`/`_take_frame()`).
- `tests/test_plp1_frame.c` (new) — header validation, flags, version,
  reserved field, status/ERROR consistency, message type registry, and
  three byte-exact golden vectors (PING request, PING response,
  PRINT_START).
- `tests/test_plp1_payloads.c` (new) — VERSION/STATUS/PRINT_START/error
  payload codec tests, including canonical field ordering and truncation
  behavior.
- `tests/test_plp1_parser.c` (new) — fragmentation (byte-at-a-time,
  arbitrary cut points, header/payload splits), coalesced frames, a
  complete-plus-partial-next-frame case, a structural framing fault, a
  semantic rejection that still allows continued parsing, and buffer-full
  partial consumption.
- `CMakeLists.txt` — two new static libraries,
  `printerctl_protocol_plp1` and `printerctl_protocol_plp1_parser`
  (the latter depending only on the former) — neither links any other
  `printerctl_*` library, `config`, or `logging`.
- `tests/CMakeLists.txt` — three new test targets.
- `docs/design/protocol-v1.md` (new) — the durable wire specification.
- `docs/roadmap.md` — M14 status line updated to DONE.
- `docs/architecture.md` — new implementation-status note (§22),
  matching M13's §21 pattern; §10 (the pre-implementation sketch) left
  as-is.

No changes to `core`, `printer`, `serial`, `marlin`, `gcode`, `cli`,
`daemon`, or `Components.md`/`context-ia.md` — confirmed by inspection
(`grep -n "#include" src/protocol/*.c include/printerctl/protocol/*.h`
shows only standard C headers and the protocol module's own headers)
rather than merely asserted.

## 2a. Review fixes (post-implementation round)

An independent review found the initial validation incomplete in four
blocking areas, all in `src/protocol/plp1.c`, plus a documentation gap.
All addressed in this same milestone:

1. **`request_id == 0` was accepted in a REQUEST/RESPONSE.** The
   original read of the task ("no administra generación ni wraparound")
   was too permissive: `0` is *reserved* for uncorrelated messages, so a
   correlated frame claiming it is itself a framing violation, not a
   client-generation concern. Fixed: both `encode_frame()` and
   `decode_frame()` now reject `request_id == 0` on `REQUEST`/`RESPONSE`
   as `MALFORMED_FRAME`; `EVENT` is exempt (0 is exactly what it is
   reserved for). Design doc §8 rewritten accordingly.
2. **No validation that a message type supports its role flag.** Added
   `message_type_allowed_roles()`: every Phase 1 message type supports
   `REQUEST`/`RESPONSE` only (none is an event-family message yet), so
   `EVENT` combined with any of them is now rejected as `INVALID_FLAGS`
   at both encode and decode. Design doc §7 updated.
3. **`HAS_PAYLOAD` vs. actual payload presence was never cross-checked
   on decode**, and on encode only ever additively OR'd in, never
   validated against a caller who set it by hand with no payload. Fixed
   in both directions, `INVALID_FLAGS` on mismatch.
4. **Every payload decoder accepted "at least enough" bytes instead of
   exactly enough** (`VERSION` response, `STATUS` request/response,
   `PRINT_START`, the generic error payload) — trailing bytes past what
   the codec actually consumed were silently accepted. All five now
   require exact length (or, for `STATUS` response and `PRINT_START`,
   exact consumption matching what the bitmap/string declared),
   rejecting extra bytes as `INVALID_PAYLOAD`.
5. **`STATUS` response `present_fields` never rejected reserved bits**
   (9-31) — a response could claim to carry a field this decoder has no
   size for. Fixed: `PRINTERCTL_PLP1_STATUS_FIELD_KNOWN_MASK` (0x01FF)
   added; any bit outside it in a *response's* `present_fields` is
   `INVALID_PAYLOAD` (a client's `requested_fields` is deliberately not
   restricted the same way — asking for an unknown field is harmless).
6. **A latent NULL-handling bug**, found while fixing (4): every payload
   decoder treated a `NULL` payload pointer as `ERR_INVALID_ARGUMENT`
   (caller misuse) even when it was `decode_frame()`'s own legitimate
   "zero-length payload" output (`payload == NULL` when
   `payload_length == 0`) — e.g. a `PRINT_START` request sent with no
   payload at all would have reported the wrong error class. Fixed
   across `decode_string()`, `decode_version_response()`,
   `decode_status_request()`, `decode_status_response()`, and
   `decode_error_payload()`: `NULL` is only rejected as caller misuse
   when paired with a non-zero length.

Tests added: `test_message_type_role_combinations()`,
`test_request_id_zero_rejected()` (replacing the old
`test_request_id_zero_in_request()`, which asserted the now-reversed
behavior), `test_has_payload_consistency()`,
`test_unexpected_payload_on_pause()` (wire-level, not just a
synthetically-built frame struct), `test_max_size_frame()` +
`test_max_size_frame_boundary()` (frame codec and parser respectively),
`test_required_payload_absent()`, `test_trailing_bytes_rejected()`,
`test_status_response_reserved_bits()` — 8 new test functions across the
three binaries, all passing, full sanitizer suite re-run after the
fixes (§3 below already reflects the post-fix state).

**Resolve-soon**: golden vectors (PING request/response, PRINT_START)
added directly to `docs/design/protocol-v1.md` §§14/17 as literal byte
tables, not only present in `tests/test_plp1_frame.c` — the spec is now
self-contained without needing to read the test file to see the exact
wire bytes.

## 2b. Review fixes (round 2)

A follow-up review found two items from round 1 still open:

1. **Mandatory payload validation was still per-caller, not
   per-message/role.** Round 1 added the `HAS_PAYLOAD`-vs-actual-payload
   structural check and payload-specific "exact length" checks, but
   nothing yet enforced *which* `(message_type, role)` combinations must
   or must not carry a payload at all — that was still only checked by
   whichever payload-specific decoder or `expect_no_payload()` call a
   caller happened to invoke afterward, e.g. a `PING` frame with an
   attached payload, or a `PRINT_START` request with none at all, would
   decode successfully at the generic frame level. Fixed:
   `message_role_payload_requirement()` (new, `plp1.c`) maps every
   `(message_type, role)` to forbidden/required/optional, with an
   `ERROR`-flagged response of any message type always optional
   (overriding the per-type rule, since it may carry the generic error
   payload instead of its normal shape). Enforced directly inside
   `decode_frame()` and, as a caller-input check, `encode_frame()` — see
   design doc §7's new subsection for the full table.
2. **`STATUS` request `requested_fields` did not reject reserved
   bits**, only the response's `present_fields` did (round 1's own
   documented, deliberate asymmetry — "asking for an unknown field is
   harmless"). The review closed this: both directions now reject any
   bit outside `PRINTERCTL_PLP1_STATUS_FIELD_KNOWN_MASK`, for
   consistency with the protocol's blanket "no bit is ever silently
   ignored" rule (§7) rather than carving out an exception for one
   direction.

Tests added/rewritten: `test_mandatory_payload_validation()` (replacing
`test_unexpected_payload_on_pause()`, whose premise — "decode_frame()
doesn't know, only `expect_no_payload()` rejects it" — the fix directly
inverted) covers all five message types' forbidden/required shape at
both `encode_frame()` and `decode_frame()`, plus the `ERROR`-flag
override in both directions (with and without a payload).
`test_message_type_role_combinations()` and `test_max_size_frame()`/
`test_max_size_frame_boundary()` needed updating too: the former now
supplies the correct dummy payload per `(type, role)` so it keeps
isolating the role/type check rather than accidentally failing on the
new payload-requirement check; the latter two switched their fixture
from `PING` (now payload-forbidden) to `PRINT_START` (whose request
legitimately carries an arbitrarily large payload), since `PING` could
no longer be used to build an oversized-payload frame at all.

## 2c. Review fixes (round 3)

Round 2 added `PRINTERCTL_PLP1_STATUS_FIELD_KNOWN_MASK` validation to
both STATUS *decoders* (`decode_status_request()`/
`decode_status_response()`) but not the two *encoders* — a caller could
construct a `requested_fields`/`present_fields` value with a reserved
bit set and `encode_status_request()`/`encode_status_response()` would
serialize it onto the wire without complaint, even though this same
codec's own decoder would then refuse to read it back. Fixed: both
encoders now reject a reserved bit as `PRINTERCTL_PLP1_ERR_INVALID_ARGUMENT`
(caller-input validation, matching every other caller-side check in
`encode_frame()`/the other encoders — never `ERR_MALFORMED`, which
stays reserved for decoding untrusted wire bytes).

Tests added: `test_encode_status_request_rejects_reserved_bits()` and
`test_encode_status_response_rejects_reserved_bits()`, each also
confirming the rejected call reports zero bytes written.

## 3. Verification

- `cmake -S . -B build-verify-gcc -DCMAKE_C_COMPILER=gcc` +
  `cmake --build`: clean, zero warnings under `-Wall -Wextra -Wpedantic`.
- Same with `-DCMAKE_C_COMPILER=clang`: clean, zero warnings.
- `ctest --test-dir build-verify-{gcc,clang} --output-on-failure`:
  14/14 test binaries pass under both compilers (the existing 11 plus
  the 3 new PLP/1 binaries).
- ASan+UBSan (`-fsanitize=address,undefined`, Clang): full 14-binary
  suite built and run — 14/14 pass, no sanitizer reports.
- ThreadSanitizer: run once on the 3 new binaries for consistency with
  the rest of the project's verification habit, though this module has
  no concurrency to meaningfully exercise — clean, as expected.
- All three golden vectors (PING request, PING response, PRINT_START
  with `"/tmp/a.gcode"`) matched the task's exact byte arrays on the
  first real run, both directions (`encode_frame()` producing those
  exact bytes, `decode_frame()` recovering the exact field values from
  them) — not merely a self-consistent round trip through this
  implementation's own encode/decode pair.
- **Round 2**: all of the above (GCC, Clang, 14/14 ctest under both,
  ASan+UBSan, TSan on the 3 protocol binaries) re-run after the §2b
  fixes — all still pass, including the golden vectors (unaffected by
  this round, since PING's shape did not change, only which other
  message types now require/forbid a payload).
- **Round 3**: same battery re-run after the §2c encoder-mask fixes —
  GCC/Clang clean, 14/14 ctest under both, ASan+UBSan clean, TSan clean
  on `printerctl_plp1_payload_tests`.

## 4. Risks found

- **STATUS payload field widths were unspecified by the task.**
  Resolved by picking concrete, documented widths (protocol-v1.md §16)
  rather than leaving the codec's shape ambiguous — flagged explicitly
  as an M14 decision since Milestone 16 (real metrics) or a future
  protocol-v2 discussion may want to revisit them once real engine
  telemetry is available.
- **The status/ERROR-flag consistency check has no dedicated status
  code.** A request with non-`SUCCESS` status, or a response whose
  `ERROR` flag disagrees with its status, both decode as the generic
  `MALFORMED_FRAME` — the closest fit in the given registry. Noted in
  protocol-v1.md §9 rather than inventing an unlisted code.
- **`printerctl_plp1_parser_take_frame()`'s structural-vs-semantic
  split required a fourth status value beyond the three sketched in
  planning** (`FRAME_READY`/`NEED_MORE`/`ERR_MALFORMED` alone would have
  conflated "stream untrustworthy, stop" with "this one frame was bad,
  keep going"). Added `PRINTERCTL_PLP1_PARSER_FRAME_REJECTED`, which
  advances past the frame before returning — verified via
  `test_semantic_rejection_allows_continued_parsing()`, which feeds an
  unknown-message-type frame followed by a valid one and confirms both
  outcomes.

## 5. Decisions taken

See [[protocol-v1|docs/design/protocol-v1.md]] for the full wire
specification; summary of implementation-level calls not fully pinned
down by the task text:

- Two C-level enums instead of one: `printerctl_plp1_wire_status_t`
  (frame content, fixed to the wire's numeric codes) is kept separate
  from `printerctl_plp1_result_t` (a codec function's own success/
  failure) — the wire vocabulary is externally frozen by the contract,
  so it does not get to double as this module's function-return type.
- `encode_frame()` validates the same structural rules `decode_frame()`
  enforces (version, flag consistency, status/ERROR consistency) as
  caller-input checks (`ERR_INVALID_ARGUMENT`), but does not validate
  `message_type` against the registry — encoding trusts caller-
  constructed data; registry membership is decode's job of validating
  untrusted wire input.
- `decode_string()` rejects a string that does not fit the caller's
  buffer; `decode_error_payload()` truncates instead — the two payload
  kinds have different truncation-safety properties (an identifier vs.
  informative text), so they get different behavior rather than one
  shared "always truncate" or "always reject" rule.
- The parser tracks a lazily-compacted `consumed_offset` rather than
  shifting the buffer inside every `take_frame()` call, so a borrowed
  frame from one `take_frame()` call is only ever invalidated by the
  *next* `feed()`/`take_frame()` call — never mid-call.
- `feed()` performs partial consumption (reporting `*out_consumed`)
  rather than hard-failing on an oversized chunk, so a real transport
  can always make forward progress regardless of its own read size.

## 6. Next steps

- Milestone 15 implements the `AF_UNIX`/`SOCK_STREAM` transport and
  wires it to `printerctl_plp1_parser_t` — the parser's `feed()`/
  `take_frame()` shape was designed with exactly that integration in
  mind (see protocol-v1.md §21's usage sketch).
- Milestone 16 defines `execution_state`'s real value set and
  implements the actual `PRINT_START`/`PAUSE`/`RESUME`/`CANCEL` state
  machine this milestone only reserved wire space and contract text for.
- Independent review should focus first on the status/ERROR-flag
  consistency validation and the parser's structural-vs-semantic split
  (design doc §§9, 12), per Agents.md §7's review priorities (protocol
  correctness ahead of style) — these are the two places this milestone
  made judgment calls the task text left implicit.
