"""Extract production HS target/scan functions for native test harnesses."""

from pathlib import Path
import sys


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
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
body = extract_function(source, "static int cmd_handshake_scope(int argc,char **argv)")
Path(sys.argv[1], "handshake_scope_under_test.inc").write_text(body + "\n")

scan_start = source.index("static bool hs_scan_event(")
scan_end = source.index("\nstatic int cmd_show_scan_results(", scan_start)
Path(sys.argv[1], "hs_scan_under_test.inc").write_text(
    source[scan_start:scan_end] + "\n"
)
