"""Guard asynchronous scan ownership and cancellation in production main.c."""

from pathlib import Path


source = Path("../../ESP32C5/main/main.c").read_text()


def function(signature: str) -> str:
    start = 0
    while True:
        start = source.index(signature, start)
        after = start + len(signature)
        while source[after].isspace():
            after += 1
        if source[after] == "{":
            break
        start = after
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise AssertionError(f"unterminated {signature}")


start_scan = function(
    "static esp_err_t start_background_scan(uint32_t min_time, uint32_t max_time)"
)
cancel_scan = function(
    "static bool cancel_background_scan_and_wait(uint32_t timeout_ms)"
)
release_owner = function("static void hs_scan_release_event_owner(void)")
hs_complete = function("static void hs_scan_complete(bool success)")
stop_operations = function("static int stop_operations(bool reset_wifi)")
scan_done = source[
    source.index("        case WIFI_EVENT_SCAN_DONE: {") :
    source.index("            // Update OLED with scan results", source.index("        case WIFI_EVENT_SCAN_DONE: {"))
]

# No scan-stop call may bypass the one cancellation owner.
assert source.count("esp_wifi_scan_stop(") == 1

# Replacement scans refuse the barrier before mutating active state or asking
# the SDK to start. HST output/cleanup owners are also early busy gates.
barrier = start_scan.index("if (g_scan_cancel_pending)")
running = start_scan.index("g_scan_in_progress = true;")
sdk_start = start_scan.index("esp_wifi_scan_start(")
assert barrier < running < sdk_start
assert "hs_scan_output_active" in start_scan
assert "handshake_cleanup_active" in start_scan
assert "g_scan_cancel_pending = false" not in start_scan

# Cancellation raises the barrier first. The no-scan branch may retire it
# immediately; an active scan asks the SDK to stop and waits for the event
# handler-owned release, returning failure rather than clearing it on timeout.
raise_barrier = cancel_scan.index("g_scan_cancel_pending = true;")
check_running = cancel_scan.index("if (!g_scan_in_progress)")
request_stop = cancel_scan.index("esp_wifi_scan_stop(")
wait_barrier = cancel_scan.index("&& g_scan_cancel_pending")
timeout_check = cancel_scan.index("if (g_scan_cancel_pending)", wait_barrier)
timeout_return = cancel_scan.index("return false;", timeout_check)
assert raise_barrier < check_running < request_stop < wait_barrier < timeout_check < timeout_return
assert cancel_scan.count("g_scan_cancel_pending = false;") == 1
assert cancel_scan.index("g_scan_cancel_pending = false;") < request_stop

# The event consumes scan records and copies the immutable HST snapshot before
# completion. There is a single owner retirement path and no post-terminal
# flag write that could erase a fast second scan.
assert scan_done.index("esp_wifi_scan_get_ap_records(") < scan_done.index("hs_scan_snapshot.records")
assert scan_done.index("hs_scan_snapshot.records") < scan_done.index("hs_scan_complete(")
assert "g_scan_in_progress = false" not in scan_done
assert "g_scan_cancel_pending = false" not in scan_done
assert scan_done.count("hs_scan_release_event_owner();") == 1  # non-HST branch

assert release_owner.index("g_scan_in_progress=false") < release_owner.index("g_scan_cancel_pending=false")
assert release_owner.index("g_scan_cancel_pending=false") < release_owner.index("hs_scan_output_active,false")

# AP rows retain ownership. Success and every error path release ownership
# immediately before its terminal event, so a capture started on scan_done is
# accepted and the event handler cannot later clobber it.
token_copy = hs_complete.index("memcpy(token,hs_scan_snapshot.token")
count_copy = hs_complete.index("snapshot_count=hs_scan_snapshot.count")
ap_loop = hs_complete.index("for(unsigned i=0;i<snapshot_count;i++)")
success_release = hs_complete.rindex("hs_scan_release_event_owner();")
done_emit = hs_complete.rindex('hs_scan_event(token,"scan_done"')
assert token_copy < count_copy < ap_loop < success_release < done_emit
assert "hs_scan_snapshot.count" not in hs_complete[success_release:done_emit]
assert hs_complete.count("hs_scan_release_event_owner();") == 4

# Explicit stop routes through the same barrier and acknowledges success only
# after the helper reports that the scan-done owner has retired.
call_cancel = stop_operations.index("cancel_background_scan_and_wait(2000)")
success_log = stop_operations.index('"Background scan stopped."', call_cancel)
assert call_cancel < success_log

print("PASS: one scan-cancel owner, immutable HST retirement and fast replacement safety")
