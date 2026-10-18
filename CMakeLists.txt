cmake_minimum_required(VERSION 3.16)

project(printerctl
    VERSION 0.1.0
    DESCRIPTION "Command-line controller for Marlin-based 3D printers over USB serial"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

# Milestone 13: the first thread in the project (daemon runtime's worker).
find_package(Threads REQUIRED)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/printerctl/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/printerctl/version.h"
    @ONLY
)

add_library(printerctl_config STATIC
    src/config/config.c
)

target_include_directories(printerctl_config
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# M29: PrusaSlicer printer profile / G-code configuration import. Pure
# file-to-text transformation; no serial, daemon or protocol knowledge.
add_library(printerctl_prusa_import STATIC
    src/config/prusa_import.c
)

target_include_directories(printerctl_prusa_import
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(printerctl_prusa_import PUBLIC m)

add_library(printerctl_logging STATIC
    src/logging/logging.c
)

target_include_directories(printerctl_logging
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(printerctl_gcode STATIC
    src/gcode/gcode.c
)

target_include_directories(printerctl_gcode
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(printerctl_gcode PUBLIC m)

add_library(printerctl_macro STATIC
    src/macro/macro.c
)

target_include_directories(printerctl_macro
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(printerctl_macro PUBLIC m)

add_library(printerctl_marlin STATIC
    src/marlin/marlin.c
)

target_include_directories(printerctl_marlin
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(printerctl_serial STATIC
    src/serial/serial.c
)

target_include_directories(printerctl_serial
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(printerctl_record STATIC
    src/record/record.c
)

target_include_directories(printerctl_record
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(printerctl_printer STATIC
    src/printer/printer.c
)

target_include_directories(printerctl_printer
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(printerctl_printer
    PUBLIC printerctl_config printerctl_logging printerctl_marlin printerctl_serial printerctl_gcode
)

add_library(printerctl_core STATIC
    src/core/print_engine.c
)

target_include_directories(printerctl_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(printerctl_core
    PUBLIC printerctl_config printerctl_logging printerctl_printer printerctl_record Threads::Threads
)

# Milestone 13: daemon runtime foundation. Independent of core/printer/
# serial/marlin/gcode/config/logging by design (Agents.md SS10) -- it
# integrates none of them yet.
add_library(printerctl_daemon_ipc STATIC
    src/daemon/ipc.c
)

target_include_directories(printerctl_daemon_ipc
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(printerctl_daemon_worker STATIC
    src/daemon/worker.c
)

target_include_directories(printerctl_daemon_worker
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(printerctl_daemon_worker PUBLIC printerctl_daemon_ipc printerctl_core printerctl_macro)

add_library(printerctl_daemon_runtime STATIC
    src/daemon/runtime.c
)

target_include_directories(printerctl_daemon_runtime
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(printerctl_daemon_runtime
    PUBLIC printerctl_daemon_worker printerctl_daemon_ipc Threads::Threads
)

# Milestone 14: Printerctl Local Protocol v1 (PLP/1) codec. Independent
# of every other printerctl_* library, config and logging included --
# a pure buffer-in/buffer-out codec (Agents.md-style isolation, no
# socket/pipe/print_engine knowledge).
add_library(printerctl_protocol_plp1 STATIC
    src/protocol/plp1.c
)

target_include_directories(printerctl_protocol_plp1
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(printerctl_protocol_plp1_parser STATIC
    src/protocol/plp1_parser.c
)

target_include_directories(printerctl_protocol_plp1_parser
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(printerctl_protocol_plp1_parser PUBLIC printerctl_protocol_plp1)

add_library(printerctl_client STATIC
    src/client/plp_client.c
)

target_include_directories(printerctl_client
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(printerctl_client PUBLIC printerctl_protocol_plp1_parser)

# Milestone 15: local Unix socket transport. This is the first layer that
# knows both daemon runtime snapshots and PLP/1 frames; lower layers stay
# unaware of sockets, protocol dispatch and file descriptors.
add_library(printerctl_daemon_socket STATIC
    src/daemon/socket.c
)

target_include_directories(printerctl_daemon_socket
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(printerctl_daemon_socket
    PUBLIC printerctl_daemon_runtime printerctl_protocol_plp1_parser Threads::Threads
)

add_library(printerctl_cli STATIC
    src/cli/cli.c
)

target_include_directories(printerctl_cli
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/include"
)

target_link_libraries(printerctl_cli PUBLIC printerctl_config printerctl_logging printerctl_gcode printerctl_record printerctl_client printerctl_prusa_import)

add_executable(printerctl
    src/main.c
)

target_link_libraries(printerctl PRIVATE printerctl_cli)

add_executable(printerctld
    src/printerctld_main.c
)

target_include_directories(printerctld
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/include"
)

target_link_libraries(printerctld PRIVATE printerctl_daemon_socket printerctl_config)

enable_testing()
add_subdirectory(tests)
