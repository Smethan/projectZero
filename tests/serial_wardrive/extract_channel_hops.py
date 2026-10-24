"""Extract production C5 channel-hop helpers for native fault injection."""

from pathlib import Path
import sys


source = Path("../../ESP32C5/main/main.c").read_text()


def definition(signature: str) -> str:
    search_from = 0
    while True:
        start = source.index(signature, search_from)
        brace = source.index("{", start)
        if source.find(";", start, brace) < 0:
            break
        search_from = start + len(signature)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise AssertionError(f"unterminated {signature}")


def array(signature: str) -> str:
    start = source.index(signature)
    return source[start : source.index(";", start) + 1]


parts = [
    array("static const int dual_band_channels[]"),
    "static const int dual_band_channels_count = sizeof(dual_band_channels) / sizeof(dual_band_channels[0]);",
    array("static const uint8_t wdp_ch_5_dfs[]"),
]
for signature in (
    "static bool wdp_ducb_remove_channel(int channel_idx)",
    "static bool wdp_ducb_apply_channel(int channel_idx)",
    "static bool monitor_apply_channel(const char *mode, uint8_t requested)",
    "static void monitor_failure_cleanup(const char *mode)",
    "static bool monitor_enable_promiscuous(const char *mode,",
    "static void sniffer_dog_channel_hop(void)",
    "static void deauth_detector_channel_hop(void)",
):
    parts.append(definition(signature))

Path(sys.argv[1], "channel_hops_under_test.inc").write_text("\n\n".join(parts) + "\n")
