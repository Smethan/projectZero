#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
test_dir="$(mktemp -d /tmp/wdg-fw-test.XXXXXX)"
trap 'rm -rf "$test_dir"' EXIT
python3 extract_wifi_init.py "$test_dir"
python3 extract_handshake_scope.py "$test_dir"
python3 extract_hs_artifact.py "$test_dir"
python3 extract_hs_client.py "$test_dir"
python3 extract_hs_ap.py "$test_dir"
python3 extract_hs_fcs.py "$test_dir"
python3 extract_zero_resources.py "$test_dir"
for source in test_serial.c test_hs_monitor.c test_capture_pool.c test_wifi_init.c test_country.c test_hs_targets.c test_handshake_scope.c test_hs_scan_lifecycle.c test_hs_exchange.c test_hs_artifact.c test_hs_client.c test_hs_ap.c test_hs_fcs.c test_pcapng_serializer.c; do
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wno-unused-function -I "$test_dir" -I stubs -I ../../ESP32C5/main -I ../../ESP32C5/components/pcap_serializer/include "$source" -o "$test_dir/test"
    "$test_dir/test"
done
cc -std=c11 -I "$test_dir" test_zero_resources.c -o "$test_dir/test-zero-resources"
"$test_dir/test-zero-resources"
cc -std=c11 -I "$test_dir" test_portal_cleanup.c -o "$test_dir/test-portal-cleanup"
"$test_dir/test-portal-cleanup"
for zero_profile in 0 1; do
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -DCONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO=$zero_profile \
        -Wno-unused-function -I ../vendor/cjson -I stubs -I ../../ESP32C5/main \
        test_jobs.c ../vendor/cjson/cJSON.c -lm -o "$test_dir/test-jobs"
    "$test_dir/test-jobs"
done
cc -std=c11 -D_POSIX_C_SOURCE=200809L -DCONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO=0 \
    -DCONFIG_SPIRAM=1 -Wno-unused-function -I ../vendor/cjson -I stubs \
    -I ../../ESP32C5/main test_jobs.c ../vendor/cjson/cJSON.c -lm -o "$test_dir/test-jobs-psram"
"$test_dir/test-jobs-psram"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wno-unused-function -Wno-deprecated-declarations \
    -I stubs -I ../../ESP32C5/main test_usb_ota.c -lcrypto -o "$test_dir/test-usb"
"$test_dir/test-usb"
python3 test_usb_buffer_patch.py
python3 test_scan_cancel_barrier.py
python3 test_hs_capture_invariants.py
python3 test_sniffer_evil_twin_invariants.py
