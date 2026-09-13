"""Extract production per-AP client bookkeeping for native tests."""

from pathlib import Path
import sys


def extract(source: str, signature: str) -> str:
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
    raise ValueError(f"unterminated {signature}")


source = Path("../../ESP32C5/main/main.c").read_text()
body = "\n\n".join(
    extract(source, signature)
    for signature in (
        "static int hs_find_client(",
        "static int hs_add_or_update_client(",
    )
)
Path(sys.argv[1], "hs_client_under_test.inc").write_text(body + "\n")
