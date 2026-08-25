#ifndef PRINTERCTL_PROTOCOL_PLP1_PARSER_H
#define PRINTERCTL_PROTOCOL_PLP1_PARSER_H

#include "printerctl/protocol/plp1.h"

#include <stdint.h>

/*
 * Milestone 14 incremental parser: turns an arbitrary stream of byte
 * chunks into a sequence of complete PLP/1 frames, without assuming any
 * relationship between how bytes were fed in and where frame boundaries
 * fall ("no debe asumir 1 recv() == 1 frame", even though this module
 * never calls recv() itself). Like plp1.h, it only ever touches
 * caller-provided memory -- no socket, no fd, no allocation.
 *
 * Usage (the shape a future transport, Milestone 15, will follow):
 *
 *   uint8_t buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
 *   printerctl_plp1_parser_t parser;
 *   printerctl_plp1_parser_init(&parser, buffer, sizeof(buffer));
 *
 *   for (;;) {
 *       size_t consumed;
 *       printerctl_plp1_parser_feed(&parser, chunk, chunk_length, &consumed, &error);
 *       // if consumed < chunk_length, the buffer was full: drain ready
 *       // frames (below) and feed the remainder before reading more.
 *
 *       printerctl_plp1_frame_t frame;
 *       while (printerctl_plp1_parser_take_frame(&parser, &frame, &error)
 *              == PRINTERCTL_PLP1_PARSER_FRAME_READY) {
 *           // handle `frame`; frame.payload is borrowed from `buffer`
 *           // and is only valid until the next feed()/take_frame() call.
 *       }
 *   }
 *
 * A structural framing failure (an out-of-range frame_length) means the
 * byte stream itself is no longer trustworthy for framing -- per the
 * task, this is the point a real transport should stop reading frames
 * from this connection and disconnect, distinct from decode_frame()'s
 * own failures (unknown message type, invalid flags, ...), which stay
 * on a known frame boundary and can be answered with an error response
 * on an otherwise-still-open connection.
 */

typedef struct {
    uint8_t *buffer;        /* caller-provided; capacity should be >=
                              * PRINTERCTL_PLP1_MAX_FRAME_SIZE so one full
                              * frame is always guaranteed to fit */
    size_t buffer_capacity;
    size_t buffered_length; /* total valid bytes in buffer, including
                              * already-taken ones awaiting compaction */
    size_t consumed_offset; /* bytes at the front already handed out by
                              * take_frame(); compacted away lazily, at
                              * the start of the next feed() */
} printerctl_plp1_parser_t;

/* Zeroes *parser and binds it to `buffer` (borrowed; must outlive the
 * parser). Does not take ownership -- same discipline as every other
 * module in this codebase. */
void printerctl_plp1_parser_init(
    printerctl_plp1_parser_t *parser, uint8_t *buffer, size_t buffer_capacity);

/*
 * Compacts any unconsumed tail to the front of the buffer, then appends
 * as many of `data`'s `length` bytes as fit in the remaining capacity.
 * `*out_consumed` reports how many bytes were actually appended -- it
 * can be less than `length` when the buffer is full of not-yet-taken
 * frames; the caller should drain them with take_frame() and feed the
 * remainder. Feeding is never a hard failure purely because of a large
 * chunk size: as long as buffer_capacity is at least one max frame,
 * every feed() call makes forward progress.
 */
printerctl_plp1_result_t printerctl_plp1_parser_feed(
    printerctl_plp1_parser_t *parser, const uint8_t *data, size_t length,
    size_t *out_consumed, printerctl_plp1_error_t *out_error);

typedef enum {
    PRINTERCTL_PLP1_PARSER_FRAME_READY = 0,
    PRINTERCTL_PLP1_PARSER_NEED_MORE,
    PRINTERCTL_PLP1_PARSER_FRAME_REJECTED,      /* semantic decode failure (unknown
                                                  * type, bad flags, ...): the frame
                                                  * boundary WAS trusted and has
                                                  * already been consumed, so parsing
                                                  * may continue with the next frame;
                                                  * out_error->wire_status names what
                                                  * a real response would carry */
    PRINTERCTL_PLP1_PARSER_ERR_MALFORMED,       /* structural framing failure (bad
                                                  * frame_length): nothing was
                                                  * consumed, and the stream is no
                                                  * longer trustworthy for framing --
                                                  * see the header comment above */
    PRINTERCTL_PLP1_PARSER_ERR_INVALID_ARGUMENT /* NULL parser/out_frame/out_error */
} printerctl_plp1_parser_take_status_t;

/*
 * Call in a loop after each feed() until it returns NEED_MORE. On
 * FRAME_READY, *out_frame borrows directly from the parser's own
 * buffer -- valid only until the next feed()/take_frame() call. On
 * FRAME_REJECTED, *out_frame is zeroed (nothing to hand back) but the
 * parser has already advanced past the rejected frame.
 */
printerctl_plp1_parser_take_status_t printerctl_plp1_parser_take_frame(
    printerctl_plp1_parser_t *parser, printerctl_plp1_frame_t *out_frame,
    printerctl_plp1_error_t *out_error);

#endif
