# Serial wardrive protocol v1

Commands are newline-terminated console commands. Capture is BLE discovery (not Bluetooth Classic) and Wi-Fi management reception. No GPS command or SD card is needed. GPS, timestamps, files and classification belong to the host.

## Commands and ownership

- `get_capabilities`: one `WDG:` JSON capability record. Check `wardrive_serial_v1: true`; do not infer support from the ordinary firmware version.
- `start_wardrive_serial TOKEN`: TOKEN must match `[A-Za-z0-9_-]{1,32}`. Stops old operations before starting the collectors. `started` means both collectors were successfully initialized. A second start while active is rejected.
- `wardrive_keepalive TOKEN`: renew the lease every five seconds. A missing renewal for 15 seconds ends the session.
- `stop`: cancels capture, disables callbacks, discards queued observations into the drop count, restores Wi-Fi configuration, emits `stopped`, then finishes the existing global stop command. Wait for `All operations stopped.` before starting another console operation. During capture, other registered main console operations are rejected as busy.

Every machine line begins `WDG:` and contains compact JSON. Firmware emits a leading newline to recover from prompts/partial writes. Non-prefixed console messages remain ordinary logs. Maximum framed record is 1024 bytes. Binary SSID/advertising bytes are lowercase hex, never interpolated text. Host display normalizes control characters; the raw protocol log preserves exact bytes.

## Records

Capabilities: `v:1`, `kind:"capabilities"`, `wardrive_serial_v1:true`, `bands:["wifi24","wifi5","ble"]`, `wifi_mgmt:true`, `ble_raw_ad:true`, `ble_extended:false`, `max_line:1024`.

All session records carry `v:1`, `kind`, `session`, and strictly increasing `seq`. Sequence gaps can reflect rejected/dropped transmissions. The host rejects stale sessions, duplicate sequences, unsupported versions and malformed records.

| Kind | Additional fields |
|---|---|
| started / stats / stopped | uptime_ms, wifi_count, ble_count, drops |
| error | message (plus uptime/counters) |
| wifi | capture_ms, age_ms, mac (BSSID), ssid_hex (0–32 bytes), channel, rssi, auth |
| wifi_mgmt | capture_ms, age_ms, mac (transmitter/address 2), receiver (address 1), bssid (null for probe requests), frame_type:0, subtype (4 probe request, 5 probe response, 8 beacon), ssid_present, ssid_hex, channel, rssi |
| ble | capture_ms, age_ms, mac, addr_type, event, rssi, data_hex, truncated:false |

BLE address is printed in conventional order; address type follows NimBLE (0 public, 1 random, 2 public identity, 3 random identity). Legacy GAP events use event 4 for scan response. Advertising and scan-response data are separate records; data contains the whole available AD structure, including all manufacturer/service fields. Legacy advertising is supported; extended advertising is not. The transport allocates 62 bytes for AD data; legacy packets fit without truncation. Host matching joins records only for the same address/type within three seconds.

Wi-Fi authentication currently reports OPEN or PRIVACY from the capability bit; it does not infer WPA generation from incomplete evidence. Only beacons/probe responses become AP inventory/WiGLE rows. A wildcard probe request is not an AP row. Missing SSID and present empty SSID are distinct on wifi_mgmt.

Example observation:

```json
{"v":1,"kind":"wifi","session":"demo","seq":2,"capture_ms":12500,"age_ms":10,"mac":"00:11:22:33:44:55","ssid_hex":"43616665","channel":6,"rssi":-60,"auth":"OPEN"}
```

## Bounds and timing

- 64 fixed-size queued observations, 128-entry evidence throttling cache. Unchanged evidence is limited to approximately one emission per second; new payload/event/channel evidence bypasses this. Cache collisions reduce throttling rather than suppress distinct evidence.
- Radio callbacks copy and queue only: no serial writes, allocation, GPS or SD access.
- One worker emits records; transport writes have a 100 ms budget. A partial frame is counted as a drop and framing recovers at the next newline.
- Queued observations older than two seconds are dropped. Stats are emitted every two seconds, including when no devices are around. Wi-Fi counters include received management frames; BLE counters include received advertisements before throttling. They are not unique-device counts.
- Wi-Fi channels rotate at a 160 ms target dwell over 2.4 and 5 GHz channel lists; unsupported country/driver channels can be skipped by the driver. BLE uses the existing coexistence scan function (40 ms window per 160 ms interval, active scanning for scan responses).
- A static queue stays allocated across sessions. Stop disables collectors and waits for outstanding queue copies before resetting it. No record from a session is intentionally emitted after its stopped event.
- Capture time is firmware uptime, not UTC. WDG estimates observation time as host receipt time minus age_ms and selects a valid host fix no older than three seconds at that time. Serial transit adds residual uncertainty; this is not clock synchronization or camera localization.

A missing `started` or heartbeat is an error, not proof of successful dual capture. ESP-IDF describes C5 sniffer/BLE coexistence as supported with unstable performance: [ESP-IDF 6.0.1 coexistence](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32c5/api-guides/coexist.html). Hardware validation remains necessary.

Firmware 1.7.3 adds `wardrive_wifi_serial_v1: true` to capabilities and
`start_wardrive_wifi_serial <session>`. It uses the same Wi-Fi records, channel
hopping, two-second stats, 15-second host lease and stop protocol, with BLE
discovery disabled. The host collects BLE separately. `ble_count` is zero for
this session; it must not be interpreted as a count of host BLE observations.

## Passive handshake/PMKID serial extension

Firmware 1.7.5 uses a session-owned eight-buffer pool for passive capture and a
separate four-buffer pool for HS Capture progress. Pools prefer PSRAM and fall
back to checked internal allocations. Queue entries are pointers, and buffers
remain owned by their producer/consumer until returned; this removes full-frame
copies from task stacks. These pools are released on stop or failed startup.
The ordinary Wi-Fi/BLE observation queue remains static.

Passive startup reports `kind: error` with `radio_prepare_failed`,
`capture_allocation_failed`, `task_allocation_failed`, or `radio_start_failed`
as appropriate. The existing host error/stop handling remains compatible.
`capture_memory` console logs at startup/failure report heap headroom; they are
not additional protocol records and do not renew the host lease.

`hs_sniff_serial_v1: true` advertises `start_hs_sniff_serial TOKEN`. This selects
a separate passive Wi-Fi capture mode using the same owner, session, keepalive,
status and stop protocol. It uses WIFI_MODE_NULL, no BLE scanning or Wi-Fi
transmission commands, and requires neither GPS nor SD. It captures unencrypted
EAPOL plus association/reassociation requests/responses, beacons and probe
responses for RSN PMKID and SSID context.

`hs_packet` records carry `packet` (monotonic per-session frame ID), `offset`,
`total`, `capture_ms`, `age_ms`, `channel`, `rssi`, and `data_hex`. Raw 802.11
packets have FCS removed. Maximum total is 2304 bytes. Chunks are 240 bytes except
the final chunk; offsets start at zero and increase by 240. Each chunk gets a
new session sequence number. Hosts discard incomplete or nonconsecutive frames.
No HS records are sent by All Wardrive, and no BLE/inventory records are sent
by HS Sniff. `wifi_count` counts queued raw frames in this mode; `ble_count` is 0.

The eight-frame queue is allocated for the session and released on cleanup.
The worker uses a 10 KB stack. Repeat beacons/probe responses with identical
BSSID/tagged fields are limited to one per ten seconds. The ordinary two-second
age limit applies. Stop disables reception, drains for up to 1.5 seconds plus
one in-progress frame, counts remaining drops, frees the queue and emits stopped.
WDG writes PCAP and identifies visible PMKIDs on the uConsole; receiving EAPOL
does not by itself establish handshake completeness.

## Active HS target-selection extension

Firmware 1.7.9 advertises `hs_capture_targets_v1: true`. This extension adds an
optional scan-and-select scope to both existing active capture destinations. It
does not require GPS. `sd` still requires a mounted ESP32 SD card; `serial`
keeps the existing base64 PCAP/HCCAPX transfer for host-side storage.

### Network snapshot

`hs_scan TOKEN` starts an asynchronous nearby-network scan. `TOKEN` must match
`[A-Za-z0-9_-]{1,32}`. The ESP32 must be idle, with no scan cancellation still
draining. Machine records use a separate `HST:` prefix and JSON version 1:

| Kind | Fields |
| --- | --- |
| `scan_started` | `scan`, `count`, `error` |
| `ap` | `scan`, `seq`, `bssid`, `ssid_hex`, `channel`, `rssi`, `auth` |
| `scan_done` | `scan`, `count`, `error` |
| `scan_error` | `scan`, `count`, `error` |

`ssid_hex` is zero to 32 binary-safe bytes; `auth` is the ESP-IDF numeric Wi-Fi
authentication value. AP sequence numbers start at 1 and must be contiguous.
The terminal `scan_done.count` must equal the number of AP rows. A host accepts
the snapshot only after the matching token has one `scan_started`, every row in
order and `scan_done`.

The firmware retrieves the driver's AP list once per scan event and emits at
most 64 APs. It suppresses the ordinary CSV copy for this scan. Each serial
write is bounded to 250 ms and the whole result has a three-second output
budget. Failure, cancellation, a partial serial result or a lost terminal record
revokes readiness. `stop` waits for the asynchronous scan-done event before it
acknowledges cancellation; while that event drains, another scan is rejected.
This prevents a delayed event from completing a newer token.

### Capture scope

```
start_handshake_scope <sd|serial> all
start_handshake_scope <sd|serial> TOKEN BSSID[,BSSID...]
```

`all` starts the original all-nearby sniffer/D-UCB/deauthentication behavior and
does not need a scan. The selected form accepts 1 to 16 unique unicast BSSIDs
from the matching completed snapshot. The snapshot expires after five minutes.
Before installing the capture callback, firmware resolves every BSSID and
channel and rejects the whole request if any BSSID is missing, open/WEP, on an
unsupported channel, duplicated or malformed. The resulting BSSID/channel set
is immutable for the run. Promiscuous reception, D-UCB hopping and active
deauthentication are then limited to that set.

WDG also refuses BSSIDs on its host whitelist. Direct protocol clients must
apply their own policy before issuing an active capture command.

Startup errors are single `HST:` records with `kind:"capture_error"`, `storage`
(`sd` or `serial`) and one of `invalid_targets`, `busy`, `sd_required`,
`scan_expired`, `target_unavailable`, `target_channel` or `start_failed`.
Validation is fail closed: an invalid selected request never falls back to
all-nearby capture.

### Serial artifact framing

The `serial` destination emits one bounded artifact per AP after the callback is
disabled and its queues are drained. Each artifact is ordered as follows:

```text
CAPTURE_KIND: VALID|PMKID|PARTIAL
--- PCAP BEGIN ---
<base64 PCAP lines>
--- PCAP END ---
PCAP_SIZE: <bytes>
--- HCCAPX BEGIN ---       # VALID only
<base64 HCCAPX lines>      # VALID only
--- HCCAPX END ---         # VALID only
SSID: <ssid>  AP: <BSSID>
```

`CAPTURE_KIND` always precedes the binary blocks. The final `SSID`/`AP` line is
the sole commit record; firmware omits it if any preceding serial write fails.
Firmware owns the stdout lock across the whole sequence so unrelated logs cannot
interleave. A host must discard an uncommitted partial sequence and reset its
block state at the next `CAPTURE_KIND` or `PCAP BEGIN`. `PMKID` and `PARTIAL`
carry PCAP only. `VALID` carries a same-exchange PCAP plus HCCAPX. The active
artifact PCAP is bounded by `HSX_PCAP_MAX` at 2,136 bytes: its 24-byte global
header plus no more than four 512-byte frames and their 16-byte record headers.

The Wi-Fi callback only validates routing and copies eligible context/EAPOL
frames into an eight-frame bounded pool; it does not parse exchanges, allocate,
write files or use the serial console. The capture task owns those operations
and drains the pool before output or reset. Per-exchange state lives in PSRAM,
tracks at most 32 AP/station/replay entries and stores frames up to 512 bytes.
When full, it may replace the oldest incomplete entry but does not evict a
completed entry for new traffic.

`HSC:` PMKID and M1-M4 progress remains observation telemetry. Its counts do
not certify a matching exchange. Firmware 1.7.9 isolates saved EAPOL artifacts
by AP, station and normalized replay exchange before labeling them complete or
valid; a row of progress sightings is still not that validation result.

The scan/scope lifecycle, bounds, frame filtering, serial failures and exchange
matching have native synthetic coverage. Both ESP32-C5 release variants build,
but this release has not been physically RF-validated for targeted capture.
