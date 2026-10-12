"""Extract the production managed sniffer stop callback for native testing."""
from pathlib import Path
import sys


source = Path("../../ESP32C5/main/main.c").read_text()


def extract(signature: str) -> str:
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
    raise ValueError(f"unterminated {signature}")


body = "\n\n".join(
    extract(signature)
    for signature in (
        "static ZERO_STACK_NOINLINE bool sniffer_channel_wait_quiescent(void)",
        "static int zero_job_stop_operation(const char *token)",
    )
)
Path(sys.argv[1], "sniffer_stop_under_test.inc").write_text(body + "\n")
