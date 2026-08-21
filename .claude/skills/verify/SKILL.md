---
name: verify
description: Configure, build and run the full CTest suite for printerctl with both GCC and Clang, the dual-compiler verification this project requires before any milestone counts as done. Use when asked to verify, build, or test a change, or before closing out a milestone.
allowed-tools: Bash
---

# Verify

`Agents.md` closes every milestone with "Automated tests are executed" —
and this project's history (`.claude/settings.local.json`,
`docs/milestones/*.md`) shows that always means **both** GCC and Clang,
not one or the other. A change that only builds cleanly on one compiler
is not verified. This skill runs that exact matrix.

## Steps

Run these in order. Stop and report as soon as one step fails — do not
run later steps against a compiler that failed to build.

```bash
cmake -S . -B build-verify-gcc -DCMAKE_C_COMPILER=gcc -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-verify-gcc
ctest --test-dir build-verify-gcc --output-on-failure

cmake -S . -B build-verify-clang -DCMAKE_C_COMPILER=clang
cmake --build build-verify-clang
ctest --test-dir build-verify-clang --output-on-failure
```

If `$ARGUMENTS` mentions a clean/fresh rebuild, add `--clean-first` to
both `cmake --build` calls instead of reusing the existing build
directories.

These two directories are reused across runs (configure is a no-op when
already configured, and the build is incremental) — never delete or
reconfigure `build/` or any `build-m3-*` directory; those belong to the
developer's own working builds and to permissions already recorded in
`.claude/settings.local.json`. This skill owns `build-verify-gcc` and
`build-verify-clang` exclusively.

## Reporting

Report a short two-line result, one per compiler (`GCC: pass` /
`GCC: fail — <what failed>`). On failure, quote only the first
compiler error or the failing test name(s) from `ctest`'s
`--output-on-failure` output — not the full raw log. On success, a
one-line "both compilers pass, N/N tests" is enough; do not enumerate
every passing test.
