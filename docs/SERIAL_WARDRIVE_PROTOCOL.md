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

## Passive handshake/PMKID serial extension

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
