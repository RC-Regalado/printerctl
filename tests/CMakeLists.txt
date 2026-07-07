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

add_executable(printerctl_logging_tests
    test_logging.c
)

target_link_libraries(printerctl_logging_tests PRIVATE printerctl_logging)

add_test(NAME printerctl_logging_tests COMMAND printerctl_logging_tests)
