"""Extract production HS target/scan functions for native test harnesses."""

from pathlib import Path
import sys


def extract_function(source: str, signature: str) -> str:
    search_from = 0
    while True:
        start = source.index(signature, search_from)
        brace = source.find("{", start)
        semicolon = source.find(";", start)
        if brace < 0 or (semicolon >= 0 and semicolon < brace):
            search_from = start + len(signature)
            continue
        break
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise ValueError(f"unterminated function: {signature}")


source = Path("../../ESP32C5/main/main.c").read_text()

channel_start = source.index("static const uint8_t hs_capture_default_channels[] =")
channel_array_end = source.index("};", channel_start) + 2
count_start = source.index(
    "#define HS_CAPTURE_DEFAULT_CHANNEL_COUNT", channel_array_end
)
count_end = source.index("\n\n", count_start)
channel_plan = (
    source[channel_start:channel_array_end] + "\n" + source[count_start:count_end]
)
channel_helper = extract_function(source, "static bool hs_capture_channel_supported(")
ducb_init = extract_function(source, "static void ducb_init(void)")
Path(sys.argv[1], "hs_capture_plan_under_test.inc").write_text(
    channel_plan + "\n" + channel_helper + "\n" + ducb_init + "\n"
)

body = extract_function(source, "int cmd_handshake_scope(int argc,char **argv)")
Path(sys.argv[1], "handshake_scope_under_test.inc").write_text(body + "\n")

scan_start = source.index("static bool hs_scan_event(")
scan_end = source.index("\nstatic int cmd_show_scan_results(", scan_start)
Path(sys.argv[1], "hs_scan_under_test.inc").write_text(
    source[scan_start:scan_end] + "\n"
)
