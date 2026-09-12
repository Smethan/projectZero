#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
output="$(mktemp /tmp/wdg-fw-test.XXXXXX)"
trap 'rm -f "$output"' EXIT
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wno-unused-function -I stubs -I ../../ESP32C5/main test_serial.c -o "$output"
"$output"
