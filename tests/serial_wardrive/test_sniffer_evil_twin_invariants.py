"""Guard the Packet Sniffer intelligence and Evil Twin lifecycle contract."""

from pathlib import Path


source = Path("../../ESP32C5/main/main.c").read_text()


def function(signature: str) -> str:
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


start_all = function("static int cmd_start_sniffer_all(")
begin_session = function("static void sniffer_begin_session(void)")
callback = function("static void sniffer_promiscuous_callback(")
export = function("static int cmd_show_sniffer_intel(")
stop_operations = function("static int stop_operations(bool reset_wifi)")
sniffer_wait = function("static ZERO_STACK_NOINLINE bool sniffer_channel_wait_quiescent(void)")
sniffer_worker = function("static void sniffer_channel_task(void *pvParameters)")
hs_complete = function("static void hs_scan_complete(bool success)")
verify = function("static void verify_password(")
start_twin = function("static int cmd_start_evil_twin(")
abort_twin = function("static void evil_twin_abort_start(")
clear_html = function("static int cmd_clear_html(")
login = function("static esp_err_t login_handler(")
save = function("static esp_err_t save_handler(")
get = function("static esp_err_t get_handler(")

# Every UI sniffer run explicitly clears stale selected scope and begins a
# fresh metric window while preserving only AP identities for later matching.
assert start_all.index("g_selected_count = 0") < start_all.index("cmd_start_sniffer(")
for reset in (
    "sniffer_packet_counter = 0",
    "probe_request_count = 0",
    "client_count = 0",
    "packet_count = 0",
):
    assert reset in begin_session
assert "sniffer_session_valid = true" in begin_session

# Channel dwell is timer-owned. Traffic volume cannot cause biased/racy hops.
assert "sniffer_channel_hop()" not in callback
assert callback.count("packet_count++") >= 3

# The token-bound export and HST picker both carry BSSID-keyed client/packet
# metrics. Probe counts are explicitly derived by exact requested SSID.
for field in ('"clients"', '"packets"', '"probes"'):
    assert field.replace('"', '\\"') in export
    assert field.replace('"', '\\"') in hs_complete
assert "sniffer_probe_count_for_ssid" in export
assert "sniffer_probe_count_for_ssid" in hs_complete
assert "sniffer_active || sniffer_scan_phase" in export
assert "sniffer_active||sniffer_scan_phase" in function(
    "static int cmd_hs_scan(")
assert "hs_scan_done_event(&hs_scan_snapshot)" in hs_complete

# Managed stop never kills the serial-producing worker. It retains the owner
# on timeout, and the worker clears its handle only after its final log write.
sniffer_stop = stop_operations[
    stop_operations.index("// Stop sniffer cooperatively"):
    stop_operations.index("// Stop sniffer_dog if active")
]
assert "sniffer_channel_wait_quiescent()" in sniffer_stop
assert "return 1;" in sniffer_stop
assert "vTaskDelete(sniffer_channel_task_handle)" not in sniffer_stop
assert "waited_ms < 1500" in sniffer_wait
assert "waited_ms += 50" in sniffer_wait
assert sniffer_worker.index('MY_LOG_INFO(TAG, "Sniffer channel task ending")') < sniffer_worker.index(
    "sniffer_channel_task_handle = NULL;"
) < sniffer_worker.index("vTaskDelete(NULL);")

# Verification is pinned to the selected AP and candidate secrets never reach
# serial logs. Allocation/start failures retire portal and credential state.
assert "missing candidate or target" in verify
assert verify.index("Malloc error for password") < verify.index("Stopping deauth attack")
assert "target_bssids[0].bssid" in verify
assert "sta_config.sta.bssid_set = 1" in verify
assert "Attempting BSSID-pinned verification" in verify
assert "Received POST data: %s" not in login
assert "Portal password received: %s" not in login
assert '"Password: %s"' not in get
assert '"Password: %s"' not in save
assert "evil_twin_clear_credentials(true)" in abort_twin
assert "httpd_stop(portal_server)" in abort_twin
assert "esp_wifi_set_mode(WIFI_MODE_STA)" in abort_twin
task_create = start_twin.rindex("xTaskCreate(")
success = start_twin.index("Evil Twin started successfully.", task_create)
assert task_create < success
assert start_twin.count("evil_twin_abort_start(") >= 7
assert "Portal is already active. Use 'stop' first." in start_twin
assert "for (int i = 0; selection_valid && i < g_selected_count; i++)" in start_twin
assert ".channel = g_scan_results[g_selected_indices[0]].primary" in start_twin

# Choosing the built-in portal must remove a previously uploaded custom page.
assert "custom_portal_html = NULL" in clear_html
assert '.command = "clear_html"' in source

print("PASS: sniffer snapshot metrics and Evil Twin lifecycle/security invariants")
