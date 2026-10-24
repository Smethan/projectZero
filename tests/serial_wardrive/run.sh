#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
test_dir="$(mktemp -d /tmp/wdg-fw-test.XXXXXX)"
trap 'rm -rf "$test_dir"' EXIT
python3 extract_wifi_init.py "$test_dir"
python3 extract_sw_radio_hooks.py "$test_dir"
python3 extract_channel_hops.py "$test_dir"
python3 extract_handshake_scope.py "$test_dir"
python3 extract_hs_artifact.py "$test_dir"
python3 extract_hs_client.py "$test_dir"
python3 extract_hs_ap.py "$test_dir"
python3 extract_hs_fcs.py "$test_dir"
python3 extract_zero_resources.py "$test_dir"
python3 extract_sniffer_stop.py "$test_dir"
python3 extract_pcap_stop.py "$test_dir"
python3 extract_sniffer_callback.py "$test_dir"
python3 test_device_info_catalog.py
cc -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../../ESP32C5/components/frame_analyzer/include test_legacy_frame_safety.c \
    ../../ESP32C5/components/frame_analyzer/frame_analyzer_parser.c -o "$test_dir/test-legacy-frames"
ASAN_OPTIONS=detect_leaks=0 "$test_dir/test-legacy-frames"
python3 test_legacy_sniffer_worker.py
cc -std=c11 -I stubs -I ../../ESP32C5/components/frame_analyzer/include \
    -I ../../ESP32C5/components/hccapx_serializer/include test_legacy_hccapx_reset.c \
    ../../ESP32C5/components/frame_analyzer/frame_analyzer_parser.c \
    -o "$test_dir/test-legacy-hccapx"
"$test_dir/test-legacy-hccapx"
for source in test_serial.c test_hs_monitor.c test_capture_pool.c test_wifi_init.c test_country.c test_hs_targets.c test_hs_scan_lifecycle.c test_hs_exchange.c test_hs_artifact.c test_hs_client.c test_hs_ap.c test_hs_fcs.c test_pcapng_serializer.c; do
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wno-unused-function -I "$test_dir" -I stubs -I ../../ESP32C5/main -I ../../ESP32C5/components/pcap_serializer/include "$source" -o "$test_dir/test"
    "$test_dir/test"
done
cc -std=c11 -Wno-unused-function -I "$test_dir" test_sw_radio_hooks.c -o "$test_dir/test-radio-hooks"
"$test_dir/test-radio-hooks"
for xiao_us_144 in 0 1; do
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -DPROJECTZERO_XIAO_US_CH144=$xiao_us_144 \
        -Wno-unused-function -I "$test_dir" -I stubs -I ../../ESP32C5/main \
        test_handshake_scope.c -o "$test_dir/test-handshake-scope"
    "$test_dir/test-handshake-scope"
    cc -std=c11 -DPROJECTZERO_XIAO_US_CH144=$xiao_us_144 -Wno-unused-function \
        -I "$test_dir" test_channel_hops.c -o "$test_dir/test-channel-hops"
    "$test_dir/test-channel-hops"
done
cc -std=c11 -I "$test_dir" test_zero_resources.c -o "$test_dir/test-zero-resources"
"$test_dir/test-zero-resources"
cc -std=c11 -I "$test_dir" test_sniffer_stop.c -o "$test_dir/test-sniffer-stop"
"$test_dir/test-sniffer-stop"
cc -std=c11 -I "$test_dir" test_sniffer_callback.c -o "$test_dir/test-sniffer-callback"
"$test_dir/test-sniffer-callback"
cc -std=c11 -I "$test_dir" test_pcap_stop.c -o "$test_dir/test-pcap-stop"
"$test_dir/test-pcap-stop"
cc -std=c11 -I "$test_dir" test_portal_cleanup.c -o "$test_dir/test-portal-cleanup"
"$test_dir/test-portal-cleanup"
for zero_profile in 0 1; do
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -DCONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO=$zero_profile \
        -Wno-unused-function -I ../vendor/cjson -I stubs -I ../../ESP32C5/main \
        test_jobs.c ../vendor/cjson/cJSON.c -lm -o "$test_dir/test-jobs"
    "$test_dir/test-jobs"
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -DCONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO=$zero_profile \
        -DCONFIG_SPIRAM=$((1-zero_profile)) \
        -Wno-unused-function -I ../vendor/cjson -I stubs -I ../../ESP32C5/main \
        test_device_info.c ../vendor/cjson/cJSON.c -lm -o "$test_dir/test-device-info"
    "$test_dir/test-device-info"
done
cc -std=c11 -D_POSIX_C_SOURCE=200809L -DCONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO=0 \
    -DCONFIG_SPIRAM=1 -DCONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=1 \
    -Wno-unused-function -I ../vendor/cjson -I stubs \
    -I ../../ESP32C5/main test_jobs.c ../vendor/cjson/cJSON.c -lm -o "$test_dir/test-jobs-psram"
"$test_dir/test-jobs-psram"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wno-unused-function -Wno-deprecated-declarations \
    -I stubs -I ../../ESP32C5/main test_usb_ota.c -lcrypto -o "$test_dir/test-usb"
"$test_dir/test-usb"
python3 test_usb_buffer_patch.py
python3 test_c5_usb_clock_patch.py
python3 test_scan_cancel_barrier.py
python3 test_hs_capture_invariants.py
python3 test_sniffer_evil_twin_invariants.py
