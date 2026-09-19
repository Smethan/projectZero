"""Guard callback ownership, teardown, storage, and PSRAM invariants.

The native exchange/artifact harnesses exercise the data paths.  These checks
pin down the concurrency and checked-I/O relationships that depend on ESP-IDF
types and are therefore easiest to regress accidentally in main.c.
"""

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


callback = function("static void hs_sniffer_promiscuous_cb(")
queue_frame = function("static bool hs_queue_frame(")
process = function("static void hs_process_queued_frame(")
close = function("static void hs_capture_close(void)")
cleanup = function("static void handshake_cleanup(void)")
stop = function("static int stop_operations(bool reset_wifi)")
write_checked = function("static bool hs_write_checked(")
save_sd = function("static bool hs_save_ap_to_sd(")
build_artifact = function("static hs_artifact_kind_t hs_build_ap_artifact(")
complete_exchange = function("static hsx_entry_t *hs_complete_exchange(")
psram_init = function("static bool init_psram_buffers(void)")
scan_complete = function("static void hs_scan_complete(bool success)")
attack_task = function("static void handshake_attack_task_sniffer(void)")

# The driver callback only validates/routes packets and copies them into bounded
# queues.  Parsing, serializer growth, exchange state, AP state, storage, and
# output remain owned by the capture task.
for forbidden in (
    "malloc(",
    "calloc(",
    "realloc(",
    "free(",
    "pcap_serializer_",
    "hccapx_serializer_",
    "hsx_ingest(",
    "hs_process_queued_frame(",
    "hs_ap_targets[",
    "hs_clients[",
    "serial_output",
    "printf(",
    "MY_LOG_",
):
    assert forbidden not in callback, forbidden
assert "hs_queue_frame(" in callback and "hs_queue_client(" in callback
assert callback.count("atomic_fetch_add(&hs_capture_producers, 1)") == 1
assert callback.count("atomic_fetch_sub(&hs_capture_producers, 1)") == 1
assert callback.index("atomic_fetch_add(&hs_capture_producers, 1)") < callback.index(
    "atomic_load(&hs_capture_accepting)"
)
assert callback.rindex("atomic_fetch_sub(&hs_capture_producers, 1)") > callback.index(
    "done:"
)
sig_len = callback.index("unsigned sig_len = pkt->rx_ctrl.sig_len")
short_fcs = callback.index("if (sig_len < 4)", sig_len)
strip_fcs = callback.index("size_t len = sig_len - 4U", short_fcs)
scope_check = callback.index("hst_frame_allowed(&handshake_scope, frame, len)")
classification = callback.index("hs_capture_kind(frame, len)")
assert sig_len < short_fcs < strip_fcs < scope_check < classification
assert callback.count("hs_queue_frame(pkt, len,") == 4
assert "copy->len = (uint16_t)len" in queue_frame
assert "memcpy(copy->data, pkt->payload, len)" in queue_frame
assert "hsx_ingest_radio(" in process and "hs_capture_append_for_progress(" in process

# The immutable scope is enforced again immediately before active injection.
# This protects all-except capture even if unrelated AP/client state somehow
# survives long enough to reach the task-owned deauthentication loop.
scope_filter = attack_task.index("hst_contains(&handshake_scope, ap->bssid)")
deauth_send = attack_task.index("hs_send_targeted_deauth(", scope_filter)
assert scope_filter < deauth_send

# Stop accepting before unregistering/disabling the callback.  Wait for every
# producer, drain its published copies, then destroy queues/pool.  Cleanup may
# only serialize or reset shared state after that quiescence point.
accept_off = close.index("atomic_store(&hs_capture_accepting, false)")
unregister = close.index("esp_wifi_set_promiscuous_rx_cb(NULL)")
disable = close.index("esp_wifi_set_promiscuous(false)")
wait = close.index("while (atomic_load(&hs_capture_producers))")
drain = close.index("hs_capture_drain()")
delete_hint = close.index("vQueueDelete(hs_hint_queue)")
delete_pool = close.index("capture_pool_close(&hs_frame_pool)")
assert accept_off < unregister < disable < wait < drain < delete_hint < delete_pool

owner_on = cleanup.index("atomic_store(&handshake_cleanup_active, true)")
quiesce = cleanup.index("hs_capture_close()")
progress_stop = cleanup.index("hsm_stop()")
serial_dump = cleanup.index("handshake_dump_serial()")
serializer_reset = cleanup.index("pcap_serializer_deinit()")
state_reset = cleanup.index("hsx_reset(hs_exchange_state)")
task_release = cleanup.index("handshake_attack_task_handle = NULL")
owner_off = cleanup.index("atomic_store(&handshake_cleanup_active, false)")
assert owner_on < quiesce < progress_stop < serial_dump < serializer_reset
assert serializer_reset < state_reset < task_release < owner_off
assert cleanup.count("atomic_store(&handshake_cleanup_active, false)") == 1

# stop_operations waits for the task-owned drain and reports failure rather
# than deleting the task while it owns a serializer or capture buffer.
hs_stop_begin = stop.index("// Stop handshake attack task if running")
hs_stop_end = stop.index("// Stop channel view monitor", hs_stop_begin)
hs_stop = stop[hs_stop_begin:hs_stop_end]
assert "handshake_cleanup_active" in hs_stop
assert "handshake_attack_task_handle = NULL" not in hs_stop
assert "vTaskDelete(handshake_attack_task_handle)" not in hs_stop
assert hs_stop.index("handshake_attack_active = false") < hs_stop.index(
    "for (int i = 0; i < 1200"
)
assert 'return 1;' in hs_stop

# File success is only possible after an exact write, flush, fsync, and close.
# Any failure removes the partial path.  For a VALID capture, the PCAPNG is also
# removed if its paired HCCAPX cannot be committed, and success text follows
# both checked writes.
assert write_checked.index("fwrite(data, 1, size, file) == size") < write_checked.index(
    "fflush(file) == 0"
)
assert write_checked.index("fflush(file) == 0") < write_checked.index(
    "fsync(fileno(file)) == 0"
)
assert write_checked.index("fsync(fileno(file)) == 0") < write_checked.index(
    "fclose(file)"
)
assert write_checked.index("fclose(file)") < write_checked.index("unlink(path)")
assert "if (!ok) unlink(path)" in write_checked

pcapng_write = save_sd.index("hs_write_checked(pcapng_path")
hccapx_write = save_sd.index("hs_write_checked(hccapx_path")
remove_pcapng = save_sd.index("unlink(pcapng_path)", hccapx_write)
valid_claim = save_sd.index('printf("HANDSHAKE IS COMPLETE AND VALID')
return_success = save_sd.rindex("return true")
assert pcapng_write < hccapx_write < remove_pcapng < valid_claim < return_success
assert "hsx_build_pcapng_with_context(" in build_artifact
assert "size <= 60" in build_artifact
assert "entry->hccapx.essid_len" in complete_exchange

# Large exchange state and the reusable PCAPNG artifact workspace must remain
# in PSRAM, never as locals on the 12 KiB capture task stack.
assert "hs_exchange_state = heap_caps_calloc(1, sizeof(hsx_state_t), MALLOC_CAP_SPIRAM)" in psram_init
assert "hs_artifact_buffer = heap_caps_malloc(HSX_ARTIFACT_MAX, MALLOC_CAP_SPIRAM)" in psram_init

# A scan releases its busy flags before the terminal record, but copies token
# and count locally first so an immediate replacement cannot relabel that
# record through the shared snapshot.
copy_token = scan_complete.index("memcpy(token,hs_scan_snapshot.token")
copy_count = scan_complete.index("snapshot_count=hs_scan_snapshot.count")
release = scan_complete.rindex("hs_scan_release_event_owner()")
done = scan_complete.rindex('hs_scan_event(token,"scan_done"')
assert copy_token < copy_count < release < done
assert "hs_scan_snapshot.count" not in scan_complete[release:done]

print("PASS: HS callback quiescence, task ownership, checked storage and PSRAM invariants")
