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

add_library(printerctl_marlin STATIC
    src/marlin/marlin.c
)

target_include_directories(printerctl_marlin
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(printerctl_cli STATIC
    src/cli/cli.c
)

target_include_directories(printerctl_cli
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/include"
)

target_link_libraries(printerctl_cli PUBLIC printerctl_config printerctl_logging printerctl_gcode)

add_executable(printerctl
    src/main.c
)

target_link_libraries(printerctl PRIVATE printerctl_cli)

enable_testing()
add_subdirectory(tests)
