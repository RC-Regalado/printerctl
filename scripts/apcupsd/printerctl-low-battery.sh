#!/usr/bin/env bash
#
# printerctl-low-battery.sh — notifies printerctl that APCUPSD detected a
# low-battery / imminent-shutdown condition, so any print streaming right
# now stops gracefully and records where it stopped.
#
# Full documentation: docs/scripts/printerctl-low-battery.md
#
# This script deliberately does very little. It never touches the serial
# port and never writes any of printerctl's internal files itself -- it
# only asks the `printerctl` binary to act, exactly as an operator could
# from a terminal:
#
#   printerctl power low-battery
#
# That command looks up the printerctl process currently streaming a
# print (if any) and delivers it a SIGPWR signal, which that process
# turns into a graceful cancellation: no new G-code is transmitted, the
# G-code file and the serial port are closed normally, and an
# interrupted-session record is written. No print running is not an
# error -- this script, and the command it runs, are both safe to call
# whether or not a print happens to be active, and safe to call more
# than once (APCUPSD may retry, or fire more than one hook for the same
# event).
#
# Wire this script into APCUPSD's event hooks that fire on a low-battery
# / imminent-shutdown condition (commonly `doshutdown`, and optionally
# `onbattery` for an earlier warning) -- see
# /etc/apcupsd/apccontrol on the host running apcupsd.

set -Eeuo pipefail

# How long to wait for printerctl to react before giving up. Configurable
# so a very slow or very fast host can adjust it without editing the
# script.
readonly TIMEOUT_SECONDS="${PRINTERCTL_LOW_BATTERY_TIMEOUT:-10}"

# The printerctl binary to run. Configurable for hosts where it is not
# on PATH under its default name (a custom install prefix, a wrapper
# script, etc).
readonly PRINTERCTL_BIN="${PRINTERCTL_BIN:-printerctl}"

log() {
    # Prefer syslog (what a UPS event script normally reports through);
    # fall back to stderr so a failure to log is never silent.
    logger -t printerctl-low-battery -- "$*" 2>/dev/null || echo "printerctl-low-battery: $*" >&2
}

if ! command -v "$PRINTERCTL_BIN" >/dev/null 2>&1; then
    log "error: '$PRINTERCTL_BIN' not found in PATH; cannot notify printerctl"
    exit 3
fi

# errexit is disabled around this one call so its exit status can be
# inspected directly, rather than relying on how `if` treats the exit
# status of a failed condition (POSIX defines that as 0 when there is no
# `else`, which is not what "did this fail, and how" needs here).
set +e
timeout "$TIMEOUT_SECONDS" "$PRINTERCTL_BIN" power low-battery
status=$?
set -e

if [ "$status" -eq 0 ]; then
    log "notified printerctl of a low-battery condition"
    exit 0
fi

if [ "$status" -eq 124 ]; then
    log "error: timed out after ${TIMEOUT_SECONDS}s waiting for printerctl to react"
    exit 2
fi

log "error: printerctl reported a failure (exit code $status) while handling the low-battery notification"
exit 1
