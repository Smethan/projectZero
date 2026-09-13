#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
output="$(mktemp /tmp/wdg-fw-test.XXXXXX)"
trap 'rm -f "$output"' EXIT
for source in test_serial.c test_hs_monitor.c; do
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wno-unused-function -I stubs -I ../../ESP32C5/main -I ../../ESP32C5/components/pcap_serializer/include "$source" -o "$output"
    "$output"
done
