"""Extract production AP registration/SD-skip policy for native tests."""

from pathlib import Path
import sys


def extract(source: str, signature: str) -> str:
    # Do not mistake main.c's forward declarations for definitions.
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
body = "\n\n".join(
    extract(source, signature)
    for signature in (
        "static void hs_sanitize_ssid(",
        "static int hs_find_ap(",
        "static int hs_add_or_update_ap(",
    )
)
Path(sys.argv[1], "hs_ap_under_test.inc").write_text(body + "\n")
