#!/usr/bin/env python3
"""Reject large local stack frames in capture callbacks and their workers.

Requires an ESP-IDF build with -DCMAKE_C_FLAGS=-fstack-usage. This checks local
frames, not the complete runtime call chain; hardware high-water checks remain
necessary. In particular, never restore a 2304-byte local packet here.
"""
import argparse
from pathlib import Path

LIMITS = {
    ("serial_wardrive.c", "hs_wifi_cb"): 256,
    ("serial_wardrive.c", "worker"): 512,
    ("hs_monitor.c", "observe"): 256,
    ("hs_monitor.c", "worker"): 512,
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
