"""Extract production artifact framing helpers for the native harness."""

from pathlib import Path
import sys


def extract(source: str, signature: str) -> str:
    # main.c declares several helpers near the top of the translation unit.
    # Skip prototypes and extract the occurrence whose opening brace precedes
    # its terminating semicolon.
    search_from = 0
    while True:
        start = source.index(signature, search_from)
        brace = source.index("{", start)
        semicolon = source.find(";", start, brace)
        if semicolon < 0:
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
    raise ValueError(f"unterminated {signature}")


source = Path("../../ESP32C5/main/main.c").read_text()
functions = [
    "static void hs_sanitize_ssid(",
    "static bool hs_serial_printf_locked(",
    "static bool dump_base64_serial_locked(",
    "static bool hs_dump_ap_serial(",
]
body = "\n\n".join(extract(source, name) for name in functions)
Path(sys.argv[1], "hs_artifact_under_test.inc").write_text(body + "\n")
