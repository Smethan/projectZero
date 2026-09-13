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
    ("serial_wardrive.c", "worker"): 512,
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
    ("main.c", "cmd_handshake_scope"): 256,
    ("hs_exchange.c", "hsx_ingest"): 256,
    ("hs_exchange.c", "hsx_set_ap_ssid"): 128,
    ("hs_exchange.c", "hsx_build_pcap"): 128,
    ("hs_exchange.c", "parse_eapol"): 128,
    ("hs_exchange.c", "build_hccapx_candidate"): 256,
}


def check(build):
    found = {}
    for report in Path(build).rglob("*.su"):
        for line in report.read_text().splitlines():
            location, size, kind = line.split("\t")
            source, _, _, function = location.rsplit(":", 3)
            key = (Path(source).name, function)
            if key in LIMITS:
                if key in found:
                    raise ValueError(f"duplicate stack report for {key}")
                found[key] = (int(size), kind)
    for key, limit in LIMITS.items():
        if key not in found:
            raise ValueError(f"missing stack report for {key}; build with -fstack-usage")
        size, kind = found[key]
        if kind != "static" or size > limit:
            raise ValueError(f"{key}: {size} bytes ({kind}); maximum {limit} static bytes")
        print(f"PASS: {key[0]}:{key[1]} local stack {size} bytes (limit {limit})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    check(parser.parse_args().build)
