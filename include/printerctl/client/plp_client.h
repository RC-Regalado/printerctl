#ifndef PRINTERCTL_CLIENT_PLP_CLIENT_H
#define PRINTERCTL_CLIENT_PLP_CLIENT_H

#include "printerctl/protocol/plp1.h"

#include <stdint.h>

#define PRINTERCTL_CLIENT_DEFAULT_SOCKET_PATH "/tmp/printerctl.sock"
#define PRINTERCTL_CLIENT_ERROR_MAX 256

typedef enum {
    PRINTERCTL_CLIENT_OK = 0,
    PRINTERCTL_CLIENT_ERR_INVALID_ARGUMENT,
    PRINTERCTL_CLIENT_ERR_SOCKET_UNAVAILABLE,
    PRINTERCTL_CLIENT_ERR_CONNECT,
    PRINTERCTL_CLIENT_ERR_IO,
    PRINTERCTL_CLIENT_ERR_TIMEOUT,
    PRINTERCTL_CLIENT_ERR_PROTOCOL,
    PRINTERCTL_CLIENT_ERR_CORRELATION,
    PRINTERCTL_CLIENT_ERR_REMOTE
} printerctl_client_status_t;

typedef struct {
    printerctl_client_status_t status;
    uint16_t wire_status;
    char message[PRINTERCTL_CLIENT_ERROR_MAX];
} printerctl_client_error_t;

typedef struct {
    int fd;
    uint32_t next_request_id;
    uint8_t response_buffer[PRINTERCTL_PLP1_MAX_FRAME_SIZE];
} printerctl_client_t;

printerctl_client_status_t printerctl_client_connect(
    printerctl_client_t *client,
    const char *socket_path,
    printerctl_client_error_t *out_error);

void printerctl_client_close(printerctl_client_t *client);

/* On success, out_response->payload borrows from client-owned storage
 * and remains valid until the next request or close. */
printerctl_client_status_t printerctl_client_request(
    printerctl_client_t *client,
    uint8_t message_type,
    const uint8_t *payload,
    uint16_t payload_length,
    int timeout_ms,
    printerctl_plp1_frame_t *out_response,
    printerctl_client_error_t *out_error);

printerctl_client_status_t printerctl_client_ping(
    printerctl_client_t *client,
    int timeout_ms,
    printerctl_client_error_t *out_error);

printerctl_client_status_t printerctl_client_version(
    printerctl_client_t *client,
    int timeout_ms,
    printerctl_plp1_version_response_t *out_version,
    printerctl_client_error_t *out_error);

printerctl_client_status_t printerctl_client_status(
    printerctl_client_t *client,
    uint32_t requested_fields,
    int timeout_ms,
    printerctl_plp1_status_response_t *out_status,
    printerctl_client_error_t *out_error);

#endif
