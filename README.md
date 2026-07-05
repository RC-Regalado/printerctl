# printerctl

Command-line application written in C17 that controls 3D printers running
Marlin Firmware over USB serial. See `Agents.md`, `architecture.md` and
`roadmap.md` for project background, architecture and the development plan.

## Status

This repository currently implements Milestone 0 (project bootstrap): a
buildable CMake skeleton exposing `--help` and `--version` only. Printer
commands (`print`, `pause`, `resume`, `cancel`, `temp`, `position`, `info`,
`send`, `home`) are not implemented yet; see `roadmap.md` for the milestone
plan and `docs/milestones/milestone-0.md` for what this milestone covers.

## Requirements

- CMake >= 3.16
- A C17-capable compiler: GCC or Clang
- Linux (development target: Arch Linux)

## Build

### GCC

```bash
cmake -S . -B build -DCMAKE_C_COMPILER=gcc
cmake --build build
```

### Clang

```bash
cmake -S . -B build-clang -DCMAKE_C_COMPILER=clang
cmake --build build-clang
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

Use `build-clang` instead of `build` if you configured with Clang.

## Run

```bash
./build/printerctl --help
./build/printerctl --version
```

## Versioning

The project version is declared once, in `CMakeLists.txt`, via
`project(printerctl VERSION ...)`. It is exposed to C code through a header
generated at configure time (`include/printerctl/version.h`, produced from
`include/printerctl/version.h.in`), so there is no second place where the
version number needs to be kept in sync manually.
