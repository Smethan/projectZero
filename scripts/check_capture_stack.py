#!/usr/bin/env python3
"""Reject large local stack frames in capture callbacks and their workers.

Requires an ESP-IDF build with -DCMAKE_C_FLAGS=-fstack-usage. This checks local
frames, not the complete runtime call chain; hardware high-water checks remain
necessary. In particular, never restore a 2304-byte local packet here.
"""

import argparse
from pathlib import Path

LIMITS = {
    ("usb_ota.c", "finish_cmd"): 2048,
    ("usb_ota.c", "chunk_cmd"): 768,
    ("usb_ota.c", "block_cmd"): 256,
    ("usb_ota.c", "write_block"): 768,
    ("serial_wardrive.c", "hs_wifi_cb"): 256,
    ("serial_wardrive.c", "wifi_cb"): 256,
    ("serial_wardrive.c", "sw_ble"): 256,
    ("serial_wardrive.c", "live_oui_emit"): 640,
    ("serial_wardrive.c", "batch_worker_loop"): 512,
    ("serial_wardrive.c", "worker"): 512,
    ("serial_wardrive.c", "job_start"): 640,
    ("serial_wardrive.c", "job_event_for"): 1200,
    ("serial_wardrive.c", "device_info"): 2048,
    ("serial_wardrive.c", "external_job_supervisor"): 256,
    ("serial_wardrive.c", "sw_job_emit_lifecycle"): 1200,
    ("serial_wardrive.c", "sw_job_emit_data"): 1152,
    ("serial_wardrive.c", "sw_job_emit_legacy_line"): 1152,
    ("hs_monitor.c", "observe"): 256,
    ("hs_monitor.c", "worker"): 512,
    ("main.c", "hs_sniffer_promiscuous_cb"): 256,
    ("main.c", "hs_process_queued_frame"): 256,
    ("main.c", "hs_capture_drain"): 256,
    ("main.c", "hs_capture_close"): 128,
    ("main.c", "hs_build_ap_artifact"): 128,
    ("main.c", "hs_save_ap_to_sd"): 512,
    ("main.c", "hs_serial_printf_locked"): 384,
    ("main.c", "dump_base64_serial_locked"): 256,
    ("main.c", "hs_dump_ap_serial"): 256,
    ("main.c", "handshake_cleanup"): 128,
    ("main.c", "hs_scan_complete"): 768,
    ("main.c", "cmd_handshake_scope"): 320,
    ("main.c", "zero_job_start_operation"): 2048,
    ("hs_exchange.c", "hsx_ingest"): 256,
    ("hs_exchange.c", "hsx_set_ap_ssid"): 128,
    ("hs_exchange.c", "hsx_build_pcap"): 128,
    ("hs_exchange.c", "parse_eapol"): 128,
    ("hs_exchange.c", "build_hccapx_candidate"): 256,
}


# These implementations are compiled out on the no-PSRAM ROM-only profile.
ZERO_REMOVED = {
    ("usb_ota.c", "write_block"),
    ("main.c", "hs_build_ap_artifact"), ("main.c", "hs_save_ap_to_sd"),
    ("main.c", "hs_serial_printf_locked"), ("main.c", "dump_base64_serial_locked"),
    ("main.c", "hs_dump_ap_serial"),
}


def check(build):
    config = Path(build).parent / "sdkconfig"
    zero = config.exists() and "CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO=y" in config.read_text()
    limits = dict(LIMITS)
    if zero:
        for key in ZERO_REMOVED:
            limits.pop(key, None)
    found = {}
    for report in Path(build).rglob("*.su"):
        for line in report.read_text().splitlines():
            location, size, kind = line.split("\t")
            source, _, _, function = location.rsplit(":", 3)
            # -Os may split a static function into .part/.isra/.constprop clones.
            # Check the largest emitted frame for the function, including clones.
            key = (Path(source).name, function.split(".")[0])
            if key in limits:
                prior = found.get(key)
                if prior is None or int(size) > prior[0]:
                    found[key] = (int(size), kind)
                if kind != "static":
                    raise ValueError(f"unbounded stack report for {key}: {kind}")
    for key, limit in limits.items():
        if key not in found:
            raise ValueError(
                f"missing stack report for {key}; build with -fstack-usage"
            )
        size, kind = found[key]
        if kind != "static" or size > limit:
            raise ValueError(
                f"{key}: {size} bytes ({kind}); maximum {limit} static bytes"
            )
        print(f"PASS: {key[0]}:{key[1]} local stack {size} bytes (limit {limit})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    check(parser.parse_args().build)
