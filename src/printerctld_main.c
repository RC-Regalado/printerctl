#define _POSIX_C_SOURCE 200809L

#include "printerctl/daemon/runtime.h"
#include "printerctl/daemon/socket.h"
#include "printerctl/config.h"
#include "printerctl/version.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PRINTERCTLD_DEFAULT_SOCKET "/tmp/printerctl.sock"

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t power_failure_requested = 0;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
    printerctl_daemon_worker_request_cancel_from_signal();
}

static void handle_power_signal(int signal_number)
{
    (void)signal_number;
    power_failure_requested = 1;
    printerctl_daemon_worker_request_power_failure_from_signal();
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: printerctld [--socket PATH] [--port PATH] [--baud N] [--timeout MS] "
            "[--config PATH] [--help] [--version]\n");
}

static int install_signal_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }

    action.sa_handler = handle_power_signal;
    if (sigaction(SIGPWR, &action, NULL) != 0) {
        return -1;
    }
    return 0;
}

static bool should_stop_client(void *context)
{
    (void)context;
    return stop_requested || power_failure_requested;
}

static void process_power_failure(printerctl_daemon_runtime_t *runtime)
{
    if (!power_failure_requested) {
        return;
    }
    power_failure_requested = 0;

    uint32_t token = 0;
    printerctl_daemon_error_t error;
    if (printerctl_daemon_runtime_send_print_cancel_power_failure(runtime, &token, &error) !=
        PRINTERCTL_DAEMON_OK) {
        fprintf(stderr, "printerctld: power-failure cancel request failed: %s\n", error.message);
        return;
    }

    printerctl_daemon_event_t event;
    if (printerctl_daemon_runtime_wait_command(runtime, token, &event, &error) !=
        PRINTERCTL_DAEMON_OK) {
        fprintf(stderr, "printerctld: power-failure cancel result failed: %s\n", error.message);
    }
}

int main(int argc, char *argv[])
{
    const char *socket_path = PRINTERCTLD_DEFAULT_SOCKET;
    printerctl_config_overrides_t overrides;
    printerctl_config_overrides_init(&overrides);

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("printerctld %s\n", PRINTERCTL_VERSION_STRING);
            return 0;
        }
        if (strcmp(argv[i], "--socket") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "printerctld: --socket requires a non-empty path\n");
                usage(stderr);
                return 2;
            }
            socket_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--port") == 0) {
            if (overrides.has_port) {
                fprintf(stderr, "printerctld: duplicate option '--port'\n");
                usage(stderr);
                return 2;
            }
            if (i + 1 >= argc || argv[i + 1][0] == '\0' ||
                strlen(argv[i + 1]) >= sizeof(overrides.port)) {
                fprintf(stderr, "printerctld: --port requires a valid non-empty path\n");
                usage(stderr);
                return 2;
            }
            snprintf(overrides.port, sizeof(overrides.port), "%s", argv[++i]);
            overrides.has_port = true;
            continue;
        }
        if (strcmp(argv[i], "--baud") == 0) {
            if (overrides.has_baud) {
                fprintf(stderr, "printerctld: duplicate option '--baud'\n");
                usage(stderr);
                return 2;
            }
            int parsed = 0;
            if (i + 1 >= argc || !printerctl_config_parse_baud(argv[i + 1], &parsed)) {
                fprintf(stderr, "printerctld: --baud requires a valid baud rate\n");
                usage(stderr);
                return 2;
            }
            overrides.baud = parsed;
            overrides.has_baud = true;
            i++;
            continue;
        }
        if (strcmp(argv[i], "--timeout") == 0) {
            if (overrides.has_timeout) {
                fprintf(stderr, "printerctld: duplicate option '--timeout'\n");
                usage(stderr);
                return 2;
            }
            int parsed = 0;
            if (i + 1 >= argc || !printerctl_config_parse_timeout(argv[i + 1], &parsed)) {
                fprintf(stderr, "printerctld: --timeout requires a valid millisecond value\n");
                usage(stderr);
                return 2;
            }
            overrides.timeout_ms = parsed;
            overrides.has_timeout = true;
            i++;
            continue;
        }
        if (strcmp(argv[i], "--config") == 0) {
            if (overrides.has_config_path) {
                fprintf(stderr, "printerctld: duplicate option '--config'\n");
                usage(stderr);
                return 2;
            }
            if (i + 1 >= argc || strlen(argv[i + 1]) >= sizeof(overrides.config_path)) {
                fprintf(stderr, "printerctld: --config requires a valid path\n");
                usage(stderr);
                return 2;
            }
            snprintf(overrides.config_path, sizeof(overrides.config_path), "%s", argv[++i]);
            overrides.has_config_path = true;
            continue;
        }

        fprintf(stderr, "printerctld: unknown argument '%s'\n", argv[i]);
        usage(stderr);
        return 2;
    }

    printerctl_config_t config;
    printerctl_config_error_t config_error;
    if (printerctl_config_resolve_for_view(
            &overrides, PRINTERCTL_CONFIG_VIEW_DAEMON, &config, &config_error) !=
        PRINTERCTL_CONFIG_OK) {
        fprintf(stderr, "printerctld: configuration error: %s\n", config_error.message);
        return 3;
    }

    if (install_signal_handlers() != 0) {
        fprintf(stderr, "printerctld: signal setup failed: %s\n", strerror(errno));
        return 1;
    }

    printerctl_daemon_runtime_t runtime;
    printerctl_daemon_error_t runtime_error;
    if (printerctl_daemon_runtime_start_with_config(&runtime, &config, NULL, NULL, &runtime_error) !=
        PRINTERCTL_DAEMON_OK) {
        fprintf(stderr, "printerctld: runtime start failed: %s\n", runtime_error.message);
        return 1;
    }

    printerctl_daemon_socket_server_t server;
    printerctl_daemon_socket_error_t socket_error;
    if (printerctl_daemon_socket_listen(&server, socket_path, &runtime, &socket_error) !=
        PRINTERCTL_DAEMON_SOCKET_OK) {
        fprintf(stderr, "printerctld: socket listen failed: %s\n", socket_error.message);
        printerctl_daemon_runtime_shutdown(&runtime, &runtime_error);
        return 1;
    }

    fprintf(stderr, "printerctld: listening on %s\n", socket_path);

    int exit_code = 0;
    while (!stop_requested) {
        process_power_failure(&runtime);

        struct pollfd pfds[2];
        pfds[0].fd = server.listen_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = printerctl_daemon_runtime_event_fd(&runtime);
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int rc = poll(pfds, 2, 250);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "printerctld: poll failed: %s\n", strerror(errno));
            exit_code = 1;
            break;
        }
        if (rc == 0) {
            continue;
        }

        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            if (printerctl_daemon_runtime_drain_events(&runtime, &runtime_error) !=
                PRINTERCTL_DAEMON_OK) {
                fprintf(stderr, "printerctld: runtime event drain failed: %s\n",
                        runtime_error.message);
                exit_code = 1;
                break;
            }
        }

        if (pfds[0].revents & POLLIN) {
            if (printerctl_daemon_socket_serve_one_interruptible(&server, should_stop_client, NULL,
                                                                 &socket_error) !=
                PRINTERCTL_DAEMON_SOCKET_OK) {
                fprintf(stderr, "printerctld: client handling failed: %s\n", socket_error.message);
                exit_code = 1;
                break;
            }
        }
    }

    printerctl_daemon_socket_close(&server);
    if (printerctl_daemon_runtime_shutdown(&runtime, &runtime_error) != PRINTERCTL_DAEMON_OK) {
        fprintf(stderr, "printerctld: runtime shutdown failed: %s\n", runtime_error.message);
        return 1;
    }
    return exit_code;
}
