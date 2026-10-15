#ifndef PRINTERCTL_DAEMON_IPC_H
#define PRINTERCTL_DAEMON_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "printerctl/macro.h"

/*
 * Milestone 13 internal IPC envelopes: the messages exchanged over
 * `cmd_pipe` (control -> worker) and `event_pipe` (worker -> control)
 * inside a single `printerctld` process. This is NOT the external
 * Printerctl Local Protocol (Milestone 14) -- it never crosses a
 * process boundary, never needs a documented wire byte order, and both
 * ends are compiled from this exact header, so there is no
 * cross-version concern to design against. Do not reuse these types as
 * a shortcut for the external protocol, and do not reuse the external
 * protocol's framing here (Agents.md SS12).
 *
 * Framing: printerctl_daemon_command_t and printerctl_daemon_event_t are
 * fixed-size structs. Each starts with `wire_size` (self-describing:
 * always sizeof() the struct) so a reader can validate a full message
 * arrived intact, independent of the type/token fields that follow.
 * Nothing here assumes a single write()/read() moves a whole message:
 * printerctl_daemon_ipc_write_*()/_read_*() loop with EINTR/EAGAIN
 * retry, the same way printerctl_serial_write_line()/_read_line()
 * already do in src/serial/serial.c. A short read exactly at a message
 * boundary (0 bytes) is PRINTERCTL_DAEMON_IPC_EOF; a short read mid-way
 * through a message is PRINTERCTL_DAEMON_IPC_ERR_TRUNCATED; a
 * full-length read whose wire_size/type/result do not match what this
 * header defines is PRINTERCTL_DAEMON_IPC_ERR_MALFORMED.
 *
 * M16 adds a bounded path payload for PRINT_START and print-visibility
 * fields for PRINT_UPDATE events. This remains process-internal IPC,
 * not the external PLP/1 wire contract.
 */

#define PRINTERCTL_DAEMON_COMMAND_PATH_MAX 4096
#define PRINTERCTL_DAEMON_COMMAND_TEXT_MAX 1024
#define PRINTERCTL_DAEMON_RESPONSE_TEXT_MAX 4096

/* M13 defines only the two commands needed to validate the runtime:
 * NOP proves a correlated round-trip; SHUTDOWN drives the lifecycle.
 * START/PAUSE/RESUME/CANCEL are conceptually reserved for Milestone 16
 * but must not be implemented here (no premature behavior). */
typedef enum {
    PRINTERCTL_DAEMON_CMD_NOP = 0,
    PRINTERCTL_DAEMON_CMD_SHUTDOWN,
    PRINTERCTL_DAEMON_CMD_PRINT_START,
    PRINTERCTL_DAEMON_CMD_PRINT_PAUSE,
    PRINTERCTL_DAEMON_CMD_PRINT_RESUME,
    PRINTERCTL_DAEMON_CMD_PRINT_CANCEL,
    PRINTERCTL_DAEMON_CMD_PRINT_CANCEL_POWER_FAILURE,
    PRINTERCTL_DAEMON_CMD_QUERY_TEMP,
    PRINTERCTL_DAEMON_CMD_QUERY_POSITION,
    PRINTERCTL_DAEMON_CMD_QUERY_INFO,
    PRINTERCTL_DAEMON_CMD_RAW_SEND,
    PRINTERCTL_DAEMON_CMD_HOME,
    PRINTERCTL_DAEMON_CMD_SD_PAUSE,
    PRINTERCTL_DAEMON_CMD_SD_RESUME,
    PRINTERCTL_DAEMON_CMD_SD_CANCEL,
    PRINTERCTL_DAEMON_CMD_MACRO_EXECUTE
} printerctl_daemon_command_type_t;

typedef struct {
    uint32_t wire_size; /* always sizeof(printerctl_daemon_command_t) */
    printerctl_daemon_command_type_t type;
    uint32_t token;     /* internal correlation token; 0 is never assigned by the
                          * control context (see runtime.h) */
    char path[PRINTERCTL_DAEMON_COMMAND_PATH_MAX]; /* PRINT_START only */
    char text[PRINTERCTL_DAEMON_COMMAND_TEXT_MAX]; /* RAW_SEND only */
    uint8_t axis; /* HOME only: PRINTERCTL_PLP1_HOME_* value without including protocol/ here */
    char macro_id[PRINTERCTL_MACRO_ID_MAX]; /* MACRO_EXECUTE only */
} printerctl_daemon_command_t;

typedef enum {
    PRINTERCTL_DAEMON_EXECUTION_KIND_NONE = 0,
    PRINTERCTL_DAEMON_EXECUTION_KIND_PRINT,
    PRINTERCTL_DAEMON_EXECUTION_KIND_MACRO
} printerctl_daemon_execution_kind_t;

typedef enum {
    PRINTERCTL_DAEMON_EVENT_READY = 0,
    PRINTERCTL_DAEMON_EVENT_COMMAND_COMPLETED,
    PRINTERCTL_DAEMON_EVENT_PRINT_UPDATE,
    PRINTERCTL_DAEMON_EVENT_STOPPING,
    PRINTERCTL_DAEMON_EVENT_STOPPED,
    PRINTERCTL_DAEMON_EVENT_FAULT
} printerctl_daemon_event_type_t;

typedef enum {
    PRINTERCTL_DAEMON_RESULT_OK = 0,
    PRINTERCTL_DAEMON_RESULT_ERROR
} printerctl_daemon_result_t;

#define PRINTERCTL_DAEMON_EVENT_DETAIL_MAX 128

typedef struct {
    uint32_t wire_size; /* always sizeof(printerctl_daemon_event_t) */
    printerctl_daemon_event_type_t type;
    uint32_t token;     /* the command's token for COMMAND_COMPLETED; 0 for
                          * READY/STOPPING/STOPPED and for a FAULT not tied to
                          * one specific command */
    printerctl_daemon_result_t result;
    printerctl_daemon_execution_kind_t execution_kind;
    uint8_t execution_state;
    uint8_t progress_percent;
    uint32_t commands_sent;
    uint32_t commands_acknowledged;
    uint32_t commands_total;   /* best-effort; 0 = unknown, see print_engine.h */
    uint32_t source_position;
    uint16_t failure_reason;
    uint16_t wire_status; /* COMMAND_COMPLETED error status; 0 on success/unused */
    char macro_id[PRINTERCTL_MACRO_ID_MAX];
    uint32_t macro_current_step;
    uint32_t macro_total_steps;
    uint16_t macro_terminal_result;
    bool query_has_hotend;
    double query_hotend_actual;
    double query_hotend_target;
    bool query_has_bed;
    double query_bed_actual;
    double query_bed_target;
    double query_x;
    double query_y;
    double query_z;
    bool query_has_e;
    double query_e;
    char query_firmware_name[128];
    char query_machine_type[128];
    char query_protocol_version[128];
    char query_uuid[128];
    bool query_has_extruder_count;
    int query_extruder_count;
    char response_text[PRINTERCTL_DAEMON_RESPONSE_TEXT_MAX];
    char detail[PRINTERCTL_DAEMON_EVENT_DETAIL_MAX]; /* optional diagnostic text */
} printerctl_daemon_event_t;

typedef enum {
    PRINTERCTL_DAEMON_IPC_OK = 0,
    PRINTERCTL_DAEMON_IPC_EOF,           /* clean end of stream at a message boundary --
                                           * not an error, the expected shape of an
                                           * orderly peer departure */
    PRINTERCTL_DAEMON_IPC_ERR_TRUNCATED, /* end of stream mid-message */
    PRINTERCTL_DAEMON_IPC_ERR_MALFORMED, /* wire_size/type/result out of range */
    PRINTERCTL_DAEMON_IPC_ERR_IO         /* read()/write() failure other than EOF */
} printerctl_daemon_ipc_status_t;

/*
 * Writes `cmd`/`event` to `fd` as raw bytes, looping over partial
 * writes and retrying EINTR/EAGAIN, so the whole struct is always fully
 * written or an error is returned -- never a partial message. `fd` must
 * be a valid, writable, blocking descriptor (the internal pipes are
 * never made non-blocking). Returns PRINTERCTL_DAEMON_IPC_ERR_IO on any
 * write() failure other than EINTR/EAGAIN.
 */
printerctl_daemon_ipc_status_t printerctl_daemon_ipc_write_command(
    int fd, const printerctl_daemon_command_t *cmd);

printerctl_daemon_ipc_status_t printerctl_daemon_ipc_write_event(
    int fd, const printerctl_daemon_event_t *event);

/*
 * Reads one full `cmd`/`event` from `fd`, looping over partial reads
 * and retrying EINTR/EAGAIN. Returns PRINTERCTL_DAEMON_IPC_EOF when the
 * peer closed the pipe before any byte of a new message arrived (the
 * expected shape of an orderly departure), PRINTERCTL_DAEMON_IPC_ERR_TRUNCATED
 * when it closed mid-message, PRINTERCTL_DAEMON_IPC_ERR_MALFORMED when a
 * full-length message has an out-of-range wire_size/type/result, and
 * PRINTERCTL_DAEMON_IPC_ERR_IO for any other read() failure. `*out_cmd`/
 * `*out_event` are zeroed first and only meaningfully filled on
 * PRINTERCTL_DAEMON_IPC_OK.
 */
printerctl_daemon_ipc_status_t printerctl_daemon_ipc_read_command(
    int fd, printerctl_daemon_command_t *out_cmd);

printerctl_daemon_ipc_status_t printerctl_daemon_ipc_read_event(
    int fd, printerctl_daemon_event_t *out_event);

#endif
