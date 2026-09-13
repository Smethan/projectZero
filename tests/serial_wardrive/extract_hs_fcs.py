"""Extract the production active-HS callback/worker path for native FCS tests."""

from pathlib import Path
import sys


def extract(source: str, signature: str) -> str:
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


source = Path("../../ESP32C5/main/main.c").read_text()
signatures = (
    "static void hs_sanitize_ssid(",
    "static int hs_find_ap(",
    "static int hs_add_or_update_ap(",
    "static int hs_find_client(",
    "static int hs_add_or_update_client(",
    "static hsx_entry_t *hs_complete_exchange(",
    "static hs_artifact_kind_t hs_build_ap_artifact(",
    "static bool hs_context_seen_or_add(",
    "static bool hs_queue_frame(",
    "static void hs_queue_client(",
    "static void hs_sniffer_promiscuous_cb(",
    "static void hs_capture_append_for_progress(",
    "static bool hs_parse_ap_context(",
    "static void hs_copy_context(",
    "static void hs_process_queued_frame(",
    "static unsigned hs_capture_drain(",
    "static bool hs_capture_open(void)",
    "static void hs_capture_close(void)",
)
body = "\n\n".join(extract(source, signature) for signature in signatures)
Path(sys.argv[1], "hs_fcs_under_test.inc").write_text(body + "\n")
