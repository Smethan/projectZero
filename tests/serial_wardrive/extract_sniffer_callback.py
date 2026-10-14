"""Compile the production Packet Sniffer callback against a bounded queue shim."""
from pathlib import Path
import sys

source = Path("../../ESP32C5/main/main.c").read_text()
start = source.index("#define SNIFFER_FRAME_SLOTS 8")
end = source.index("} sniffer_frame_t;", start) + len("} sniffer_frame_t;")
callback = source.index(
    "static void sniffer_promiscuous_callback(void *buf, wifi_promiscuous_pkt_type_t type) {",
    end,
)
brace = source.index("{", callback)
depth = 0
callback_end = None
for pos in range(brace, len(source)):
    if source[pos] == "{":
        depth += 1
    elif source[pos] == "}":
        depth -= 1
        if depth == 0:
            callback_end = pos + 1
            break
if callback_end is None:
    raise RuntimeError("Sniffer callback body is incomplete")
Path(sys.argv[1], "sniffer_frame_type.inc").write_text(source[start:end] + "\n")
Path(sys.argv[1], "sniffer_callback_under_test.inc").write_text(
    source[callback:callback_end]
)
