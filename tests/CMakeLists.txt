add_executable(printerctl_tests
    test_cli.c
)

target_link_libraries(printerctl_tests PRIVATE printerctl_cli)

add_test(NAME printerctl_tests COMMAND printerctl_tests)
