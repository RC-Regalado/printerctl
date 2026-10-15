add_executable(printerctl_tests
    test_cli.c
)

target_link_libraries(printerctl_tests PRIVATE printerctl_cli)

add_test(NAME printerctl_tests COMMAND printerctl_tests)

add_executable(printerctl_config_tests
    test_config.c
)

target_link_libraries(printerctl_config_tests PRIVATE printerctl_config)

add_test(NAME printerctl_config_tests COMMAND printerctl_config_tests)

add_executable(printerctl_gcode_tests
    test_gcode.c
)

target_link_libraries(printerctl_gcode_tests PRIVATE printerctl_gcode)

add_test(NAME printerctl_gcode_tests COMMAND printerctl_gcode_tests)

add_executable(printerctl_macro_tests
    test_macro.c
)

target_link_libraries(printerctl_macro_tests PRIVATE printerctl_macro)

add_test(NAME printerctl_macro_tests COMMAND printerctl_macro_tests)

add_executable(printerctl_marlin_tests
    test_marlin.c
)

target_link_libraries(printerctl_marlin_tests PRIVATE printerctl_marlin m)

add_test(NAME printerctl_marlin_tests COMMAND printerctl_marlin_tests)

add_executable(printerctl_serial_tests
    test_serial.c
)

target_link_libraries(printerctl_serial_tests PRIVATE printerctl_serial)

add_test(NAME printerctl_serial_tests COMMAND printerctl_serial_tests)

add_executable(printerctl_printer_tests
    test_printer.c
)

target_link_libraries(printerctl_printer_tests PRIVATE printerctl_printer)

add_test(NAME printerctl_printer_tests COMMAND printerctl_printer_tests)

add_executable(printerctl_record_tests
    test_record.c
)

target_link_libraries(printerctl_record_tests PRIVATE printerctl_record)

add_test(NAME printerctl_record_tests COMMAND printerctl_record_tests)

add_executable(printerctl_core_tests
    test_print_engine.c
)

target_link_libraries(printerctl_core_tests PRIVATE printerctl_core)

add_test(NAME printerctl_core_tests COMMAND printerctl_core_tests)

add_executable(printerctl_logging_tests
    test_logging.c
)

target_link_libraries(printerctl_logging_tests PRIVATE printerctl_logging)

add_test(NAME printerctl_logging_tests COMMAND printerctl_logging_tests)

add_executable(printerctl_daemon_ipc_tests
    test_daemon_ipc.c
)

target_link_libraries(printerctl_daemon_ipc_tests PRIVATE printerctl_daemon_ipc)

add_test(NAME printerctl_daemon_ipc_tests COMMAND printerctl_daemon_ipc_tests)

add_executable(printerctl_daemon_runtime_tests
    test_daemon_runtime.c
)

target_link_libraries(printerctl_daemon_runtime_tests PRIVATE printerctl_daemon_runtime)

add_test(NAME printerctl_daemon_runtime_tests COMMAND printerctl_daemon_runtime_tests)

add_executable(printerctl_plp1_frame_tests
    test_plp1_frame.c
)

target_link_libraries(printerctl_plp1_frame_tests PRIVATE printerctl_protocol_plp1)

add_test(NAME printerctl_plp1_frame_tests COMMAND printerctl_plp1_frame_tests)

add_executable(printerctl_plp1_payload_tests
    test_plp1_payloads.c
)

target_link_libraries(printerctl_plp1_payload_tests PRIVATE printerctl_protocol_plp1)

add_test(NAME printerctl_plp1_payload_tests COMMAND printerctl_plp1_payload_tests)

add_executable(printerctl_plp1_parser_tests
    test_plp1_parser.c
)

target_link_libraries(printerctl_plp1_parser_tests PRIVATE printerctl_protocol_plp1_parser)

add_test(NAME printerctl_plp1_parser_tests COMMAND printerctl_plp1_parser_tests)

add_executable(printerctl_client_tests
    test_plp_client.c
)

target_link_libraries(printerctl_client_tests PRIVATE printerctl_client Threads::Threads)

add_test(NAME printerctl_client_tests COMMAND printerctl_client_tests)

add_executable(printerctl_daemon_socket_tests
    test_daemon_socket.c
)

target_link_libraries(printerctl_daemon_socket_tests PRIVATE printerctl_daemon_socket)

add_test(NAME printerctl_daemon_socket_tests COMMAND printerctl_daemon_socket_tests)

add_executable(printerctld_tests
    test_printerctld.c
)

target_compile_definitions(printerctld_tests
    PRIVATE
        TEST_PRINTERCTLD_PATH="$<TARGET_FILE:printerctld>"
        TEST_PRINTERCTL_PATH="$<TARGET_FILE:printerctl>"
)

target_link_libraries(printerctld_tests PRIVATE printerctl_protocol_plp1)

add_dependencies(printerctld_tests printerctld printerctl)

add_test(NAME printerctld_tests COMMAND printerctld_tests)

add_executable(printerctl_daemon_print_tests
    test_daemon_print.c
)

target_link_libraries(printerctl_daemon_print_tests PRIVATE printerctl_daemon_runtime)

add_test(NAME printerctl_daemon_print_tests COMMAND printerctl_daemon_print_tests)

# Milestone 25: deterministic build/dependency gate. Deliberately links
# against NOTHING but the C runtime -- it must not itself gain a
# dependency on any printerctl_* library, since it inspects the final
# printerctl binary from the outside (via nm) rather than by linking
# against it.
add_executable(printerctl_build_boundary_tests
    test_build_boundaries.c
)

target_compile_definitions(printerctl_build_boundary_tests
    PRIVATE
        TEST_PRINTERCTL_PATH="$<TARGET_FILE:printerctl>"
)

add_dependencies(printerctl_build_boundary_tests printerctl)

add_test(NAME printerctl_build_boundary_tests COMMAND printerctl_build_boundary_tests)
