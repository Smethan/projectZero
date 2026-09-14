# Smethan fork firmware

## 1.7.10 — Batched All Wardrive transport

- Add `start_wardrive_batch_serial` and
  `start_wardrive_wifi_batch_serial`. They collect Wi-Fi management and BLE
  observations for ten seconds, deduplicate them in a worker-owned table, pause
  collection while reporting that table, then start the next window without
  tearing down the radios. The Wi-Fi-only command is the backend for WDG's host
  BLE mode. Existing v1 streaming commands and HS modes are unchanged.
- Prefer a 256-entry PSRAM table and fall back to a checked 128-entry internal
  allocation. Callbacks retain their nonblocking static queue and perform no
  serial I/O or allocation. Equal observations keep the strongest RSSI and the
  most recent capture time; full tables and expired/unsent records increment
  the existing drop counter.
- Add protocol-v2 `started`, two-second `heartbeat`, `batch_start`,
  `batch_results`, observation, `batch_done`, `status`, `stopped`, and `error`
  records. Control records use a 250 ms deadline and repeat the same sequence
  number up to three times. `wardrive_status <session>` recovers a missed start,
  phase, or stop acknowledgment. The 15-second host lease remains in force.
- Keep the console in machine mode for the batch session so prompts and command
  echo cannot compete with framed output. Stop aborts reporting between records,
  emits a retried structured stop, restores console mode and then lets the
  existing global cleanup completion provide an independent fallback.

Native transport tests cover batch deduplication, phase/heartbeat framing,
allocation cleanup, lease expiry and v1 compatibility. Both ESP32-C5 variants
build under the pinned ESP-IDF 6.0.1 image and pass stack guards. The combined
ESP Wi-Fi-sniffer/BLE mode still uses a coexistence combination Espressif marks
as unstable; the separate host-BLE mode avoids that combination. RF behavior
and timeout recovery still require a field soak on the XIAO/uConsole hardware.

## 1.7.9 — Optional active HS targets and capture isolation

- Advertise `hs_capture_targets_v1` and add `hs_scan <token>` plus
  `start_handshake_scope <sd|serial> all|<token> <BSSID,...>`. Both existing
  active destinations retain all-nearby operation. A selected run accepts at
  most 16 BSSIDs from one completed five-minute scan, then limits promiscuous
  reception, D-UCB channel hopping and active deauthentication to that immutable
  BSSID/channel set. Neither destination requires GPS; `sd` still requires a
  mounted ESP32 SD card.
- Fail closed on malformed, duplicate, missing, open/WEP, stale-token or
  unsupported-channel targets. `HST:` JSON carries binary-safe SSIDs, contiguous
  AP sequence numbers and explicit scan/capture errors; an invalid selected
  request never becomes an all-nearby attack.
- Give each asynchronous scan event one owner. The scan handler retrieves and
  frees the ESP-IDF AP list exactly once, suppresses duplicate legacy CSV output,
  bounds serial output, and publishes a snapshot only after all rows are sent.
  `stop` waits for a cancelled scan's completion event, and new scans remain
  blocked until that event drains, preventing stale events from completing a
  newer token.
- Isolate EAPOL state by AP, station and normalized replay exchange. Only a
  matching M1+M2 or M2+M3 pair produces a complete HCCAPX/result; PMKID-bearing
  association context and partial PCAP evidence can still be preserved without
  being labeled a complete exchange. The live `HSC:` PMKID/M1-M4 counters remain
  packet sightings rather than validation. The Wi-Fi callback only bounded-copies
  eligible frames; a task owns parsing, storage and the 32-entry PSRAM exchange
  table, with a 512-byte stored-frame limit.
- Repair the SD HCCAPX reset order so the captured message-pair state is read
  before any serializer reset. Storage/result messages now describe files that
  were actually emitted, keep one exchange/AP per artifact, and retain usable
  PCAP data when no matching HCCAPX exists. Serial artifacts announce
  `CAPTURE_KIND: VALID|PMKID|PARTIAL` before any block and emit their sole
  SSID/AP commit line only after every required block succeeds. One stdout lock
  covers the full artifact so normal logs cannot split its framing.

Native tests cover strict target parsing, 802.11 BSSID direction mapping,
scan-token lifecycle and cancellation races, serial loss/deadlines, storage
startup failures, exchange matching and artifact construction. Both ESP32-C5
release variants build with stack guards. These are synthetic/build checks;
targeted capture and RF completeness were not physically validated for this
release.

## 1.7.8 — Faster native USB OTA

Measured on the uConsole: **53.33 seconds** for a complete published-release
USB update, versus **445.65 seconds** through the old receiver (about **8.4x
faster**). Deliberate serial close/lost-ACK recovery finished in **60.23 seconds**.
Both booted a verified valid slot with no pending transfer. See
[hardware validation](https://github.com/Smethan/projectZero/blob/main/docs/USB_OTA_VALIDATION_2026-09-13.md) for method and limits.

- XIAO native USB advertises 4 KiB base64 blocks to WDG 0.9.24+, replacing
  256-byte paced hex chunks. Each block still has an image identity, exact
  offset, CRC32 and acknowledgement. Full-image SHA256, ESP image validation,
  inactive-slot writes and durable 4 KiB resume checkpoints remain required.
- A guarded patch to the pinned SDK expands native USB RX buffering to 8 KiB;
  the console accepts 8 KiB lines. The receiver remains quiet during OTA.
  Block storage is static, avoiding a large REPL stack allocation.
- Final verification yields every 64 KiB instead of every 1 KiB, preserving
  scheduling opportunities while removing thousands of deliberate delays.
- Older WDG versions retain the 256-byte protocol. WROOM keeps that paced path.
  Installing 1.7.8 from 1.7.7 still uses the old speed once.
- Tests cover all final block lengths, strict base64 rejection, CRC, duplicate
  ACK recovery, legacy-to-fast resume, simulated reboot/sector recovery,
  NVS/write/hash failures, SDK input behavior and stack limits.
- No handshake capture or hotspot changes are included.

## 1.7.7 — Prevent USB OTA command truncation

- USB OTA temporarily disables console input echo and per-character flushes.
  The standard console otherwise blocks its reader long enough to overrun the
  256-byte receive ring, truncating firmware chunks before CRC validation.
- Normal console behavior returns on abort, final validation, or reboot.
  Chunk CRC, durable checkpoints, whole-image SHA256, inactive-slot-only writes,
  and boot validation are unchanged.
- Install this version using Wi-Fi OTA before using WDG's USB OTA option.

## 1.7.6 — Capture progress transport repair and resumable USB OTA

- Split serial protocol output into 64-byte writes. ESP-IDF's console USB TX
  ring is 256 bytes and rejects a single request larger than the ring. Short
  status messages succeeded while larger HSC/HS Sniff packet lines failed,
  producing gaps/drops and empty PMKID/M1-M4 displays. Full lines remain locked
  against interleaving, with bounded waiting on a disconnected host.
- Add application-mode USB OTA commands (`uota_status`, `uota_begin`,
  `uota_chunk`, `uota_finish`, `uota_abort`). The inactive app slot receives
  CRC-checked 256-byte chunks; duplicate retries are compared with flash and
  acknowledged without rewriting. Save 4 KiB checkpoints in NVS for reboot
  recovery, erase only the unfinished tail on resume, and verify full SHA256,
  app project and ESP-IDF image validity before changing the boot selection.
- Installing this release once by Wi-Fi OTA enables subsequent USB OTA updates.
  USB OTA does not change bootloader, partition table, board profile or SD files.
  An unfinished transfer blocks conflicting commands until finished/aborted.
- Offline tests model the real USB ring limit, duplicate/lost acknowledgments,
  checkpoint recovery, checksum errors, flash/NVS failures and boot validation.


## 1.7.5 — Capture memory and startup recovery

Addresses the memory hazards found while investigating HS Sniff reboots on
1.7.4. The reported device panic was unavailable, so a successful field test
is still needed to confirm resolution of that specific crash.

- HS Sniff and HS Capture progress now use bounded pools of packet buffers,
  allocated only while running. PSRAM is preferred when available, with a
  checked internal-RAM fallback. Callback and worker stacks hold pointers
  instead of full 2304-byte packets. No allocation or logging occurs in the
  packet callbacks. Pool exhaustion drops progress records rather than waiting.
- Removes the permanent 9280-byte HS Capture progress queue. Including the new
  pool metadata, the XIAO build reclaims 8888 bytes of static internal RAM.
  The HS Sniff worker stack is reduced from 10240 to 6144 bytes after removing
  the large local frame. The PCAP and serial record formats are unchanged.
- Serial capture preparation stops conflicting operations without performing
  another full Wi-Fi teardown after WDG's explicit stop. Wi-Fi is initialized
  before allocating the capture pool/task. Explicit stop retains its radio
  reset behavior.
- Wi-Fi initialization returns allocation/API errors and cleans up partial
  startup instead of aborting. Capture allocation failures produce a session
  error. Startup diagnostics report free internal RAM, its largest contiguous
  block, and free PSRAM; these are occasional logs, not recurring packet logs.
- Adds allocation/ownership/retry regression tests and compiler stack-frame
  limits for both capture callbacks and workers in the release workflow.

All Wardrive keeps its current continuous Wi-Fi channel hopping and reduced
BLE scan duty cycle. Ten-second output batches alone would not remove radio
contention and would require more buffering/delay (current records expire at
two seconds). Alternating Wi-Fi/BLE windows would be a separate scheduling
change with capture gaps. The existing host-BLE option remains available.

Use the **XIAO ESP32-C5** package for the XIAO board. Existing WDG 0.9.20 is
compatible; this release does not require a WDG update or an SD card for HS Sniff.

## 1.7.4 — HS Capture progress stream

Both existing HS Capture variants now send a bounded `HSC:` progress stream
containing copies of handshake/context frames successfully appended to their
PCAP. WDG 0.9.18 uses these for live per-AP/client PMKID and M1–M4 displays.
The observer does not change capture targeting, injection, packet selection,
SD saving or the no-SD final PCAP/HCCAPX dump. Progress can drop independently
of the capture; its counters expose that limitation. It is stopped before the
base64 file transfer. Channel/RSSI are not available from this observer.

The stream uses v1 session/sequence and 240-byte hex chunks, like passive HS
capture, with `HSC:` instead of `WDG:`. Frame records omit channel/RSSI. Status
records include `storage: sd|serial`, `wifi_count` (queued progress frames),
`ble_count: 0` and `drops`. A four-frame queue and nonblocking stdout locking
ensure a slow/unavailable host does not block the radio callback. Existing
firmware without this stream can still report coarse M1–M4 sightings to WDG.

## 1.7.3 — Wi-Fi serial capture for uConsole BLE

Adds `start_wardrive_wifi_serial <session>` and the
`wardrive_wifi_serial_v1` capability. This retains continuous 2.4/5 GHz Wi-Fi
management observations, heartbeats and the host lease, but does not start BLE
discovery. It uses Wi-Fi NULL mode and restores the previous mode on stop.
WDG can now use the uConsole's Bluetooth for All Wardrive, avoiding the C5's
documented unstable Wi-Fi sniffer/BLE coexistence combination. This is a
mitigation, not a hardware-confirmed diagnosis of every heartbeat timeout.

The existing combined command and passive HS Sniff remain available. Upgrade
WDG as well to select the new Wi-Fi-only command and collect host BLE records.

This release includes All Wardrive and passive HS Sniff over serial, without
GPS or SD on the ESP32. Use current Smethan/WatchDogsGo main for the host UI.

Choose the **xiao ZIP** for XIAO ESP32-C5 with native USB Serial/JTAG, or the
unsuffixed ZIP for the standard ESP32-C5 UART board. Each ZIP includes the
bootloader, partition table, initial OTA data, application and a board manifest.
SHA256SUMS covers the release downloads. The standalone application binaries
are intended for the matching board's onboard OTA updater.

Firmware now reports the same version in its console and IDF image metadata.
Version 1.7.2 also aligns the onboard OTA project-name check with the actual
LOCOSP-derived application, so subsequent fork updates are accepted.
Stable/tagged onboard OTA follows Smethan/projectZero and selects the matching
board application. The old mutable development-binary channel is disabled.

GitHub builds and protocol tests validate the source; RF reception and flashing
on each individual board still require hardware testing.
