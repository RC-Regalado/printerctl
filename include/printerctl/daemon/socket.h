#ifndef PRINTERCTL_DAEMON_SOCKET_H
#define PRINTERCTL_DAEMON_SOCKET_H

#include "printerctl/daemon/runtime.h"

#include <stdbool.h>
#include <stddef.h>

#define PRINTERCTL_DAEMON_SOCKET_ERROR_MAX 256
#define PRINTERCTL_DAEMON_SOCKET_PATH_MAX 108

typedef enum {
    PRINTERCTL_DAEMON_SOCKET_OK = 0,
    PRINTERCTL_DAEMON_SOCKET_ERR_INVALID_ARGUMENT,
    PRINTERCTL_DAEMON_SOCKET_ERR_PATH_TOO_LONG,
    PRINTERCTL_DAEMON_SOCKET_ERR_IN_USE,
    PRINTERCTL_DAEMON_SOCKET_ERR_SOCKET,
    PRINTERCTL_DAEMON_SOCKET_ERR_BIND,
    PRINTERCTL_DAEMON_SOCKET_ERR_LISTEN,
    PRINTERCTL_DAEMON_SOCKET_ERR_ACCEPT,
    PRINTERCTL_DAEMON_SOCKET_ERR_IO,
    PRINTERCTL_DAEMON_SOCKET_ERR_PROTOCOL
} printerctl_daemon_socket_status_t;

typedef struct {
    printerctl_daemon_socket_status_t status;
    char message[PRINTERCTL_DAEMON_SOCKET_ERROR_MAX];
} printerctl_daemon_socket_error_t;

typedef struct {
    int listen_fd;
    char socket_path[PRINTERCTL_DAEMON_SOCKET_PATH_MAX];
    bool owns_socket_path;
    printerctl_daemon_runtime_t *runtime;
} printerctl_daemon_socket_server_t;

/*
 * Local transport: AF_UNIX/SOCK_STREAM server owned by the control context.
 * M15 introduced PING/VERSION/STATUS; M16 adds PRINT_* dispatch through the
 * runtime boundary. This module still never owns print_engine, serial,
 * Marlin, G-code, queue or jobs directly.
 */
printerctl_daemon_socket_status_t printerctl_daemon_socket_listen(
    printerctl_daemon_socket_server_t *server,
    const char *socket_path,
    printerctl_daemon_runtime_t *runtime,
    printerctl_daemon_socket_error_t *out_error);

/*
 * Accepts and serves one client until EOF or a malformed stream. Sequential
 * clients are supported by calling this again; simultaneous clients are not
 * part of M15.
 */
printerctl_daemon_socket_status_t printerctl_daemon_socket_serve_one(
    printerctl_daemon_socket_server_t *server,
    printerctl_daemon_socket_error_t *out_error);

typedef bool (*printerctl_daemon_socket_should_stop_fn)(void *context);

printerctl_daemon_socket_status_t printerctl_daemon_socket_serve_one_interruptible(
    printerctl_daemon_socket_server_t *server,
    printerctl_daemon_socket_should_stop_fn should_stop,
    void *context,
    printerctl_daemon_socket_error_t *out_error);

void printerctl_daemon_socket_close(printerctl_daemon_socket_server_t *server);

#endif
