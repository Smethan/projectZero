# Smethan fork firmware

## 1.8.5 — Native USB boot and capability recovery — 2026-10-15

- Avoid initializing PSRAM twice when ESP-IDF already initialized it during
  boot. Let a XIAO with absent PSRAM reach the restricted recovery console,
  and reject a packaged XIAO build that lacks the required boot profile.
- Start the normal console after PSRAM and NVS, before display and SD setup.
  Advertise `degraded/initializing` with no scan capacity during that short
  phase, reject operational commands until board setup is complete, then
  advertise normal readiness. Preserve the Zero's existing no-PSRAM path.
- Return a typed error if device metadata would exceed the protocol line.
  Exercise the shipped XIAO and Zero operation catalogs and startup transition
  in the native protocol harness.

Pair this source with WatchDogsGo 0.10.5. At source preparation on 2026-10-15,
the current XIAO had not replied to a USB console or ROM probe, and a Waveshare
Zero was not attached. The physical reconnect, exact-image, and WDG runtime
checks must be recorded before claiming the repair is verified on hardware.

## 1.8.4 — Startup recovery, truthful radio jobs and capture cleanup — 2026-10-14 (source candidate; hardware gate pending)

- Make XIAO/WROOM PSRAM setup transactional and report degraded startup when
  memory or NVS initialization fails. Keep a restricted recovery console
  available without erasing NVS, expose bounded v3 firmware/build/boot
  diagnostics, and advertise no scan operations while degraded. Preserve the
  established zero-valued v3 limit keys so older WDG parsers can still read
  the recovery identity. Preserve the Zero's no-PSRAM startup path.
- Return checked radio channel-apply results with requested/actual channel,
  radio mode, band and SDK status. Managed Sniffer, active HS Capture and
  Packet Sniffer starts now report pending readiness until their radio and
  workers are usable; stop and replacement waits for worker retirement and
  successful restoration, retaining cleanup ownership on failure.
- Consolidate Sniffer setup and channel hopping in its worker, export the
  bounded result snapshot before release, and add packet/AP/drop telemetry.
  Stamp active HS frames at callback receipt with a 64-bit monotonic clock and
  serialize the full PCAPNG timestamp. Keep passive HS Sniff receive-only.
- Validate legacy frame lengths before parsing EAPOL/PMKID data, deliver
  PMKIDs by value, reset HCCAPX session state, and move legacy Sniffer frame
  processing to a bounded PSRAM queue/worker. Make serializer append failures
  retryable without falsely advancing handshake progress.
- Give manual PCAPNG capture cooperative start/stop and writer ownership,
  checked `.partial` finalization, and separate received/written/drop/failure
  counts. Track NimBLE port/task/sync state, and move delayed portal work out
  of the Wi-Fi event callback.

The projectZero native source harness passed. XIAO, WROOM and 4 MB Zero
profiles have each passed a final pinned ESP-IDF 6.0.1 build. This remains a source candidate, not a
published or exact-image-verified release. No live-device test or flashing was
performed. Cold-boot/reconnect, controlled active/passive RF capture, BLE
coexistence, file-integrity and long-uptime hardware acceptance remain open.

## 1.8.3 — Repair first combined Wi-Fi/BLE receive startup — 2026-10-12

- Put combined serial Wi-Fi/BLE receive jobs into `WIFI_MODE_NULL` before
  starting BLE discovery, matching the existing Wi-Fi-only and BLE-only
  receive paths. The first GUI All Wardrive start on the inspected XIAO
  returned NimBLE HCI status 519 (controller Memory Capacity Exceeded);
  combined jobs previously retained the STA mode during setup.
- Track the successful Wi-Fi mode change and restore the saved mode after
  normal stop or failed startup independently of job type. Retain failed
  restoration for a checked retry before accepting another serial start.
- Complete external-job supervisor cleanup before acknowledging a host stop,
  and wait for supervisor retirement before admitting replacement jobs. This
  repairs the reproduced Sniffer-to-passive-HS `busy_cleanup` rejection after
  the preceding job had already acknowledged `stopped`.
- Extend the production-hook native tests for combined setup, failure cleanup
  and restoration retries, plus immediate external-job replacement. Keep
  board layouts, update methods and NVS intact.

Physical and software evidence is recorded in
[the job recovery record](JOB_RUNTIME_RECOVERY_2026-10-12.md). This follow-up
uses **WDG 0.10.3**, including its boot-nonce compatibility repair, source-tagged
radio diagnostics and device health details in C5 settings.

## 1.8.2 — Restore managed jobs and sniffer cleanup — 2026-10-12

- Accept uppercase or lowercase presentation of the same 16-digit hexadecimal
  boot nonce. WDG 0.10.2 uppercased firmware metadata before sending it back,
  causing the previous case-sensitive parser to reject managed starts. WDG
  0.10.3 also sends lowercase for compatibility with older job firmware.
  Different, short and non-hex nonces remain rejected.
- Repair the reproduced XIAO Packet Sniffer stack-protection reset in the
  Wi-Fi event task when its discovery scan completes. Increase that task's
  stack to 4096 bytes and the channel worker's stack to 4096 bytes; include the
  latter in Zero's job admission budget and expand local stack guards.
- Export managed sniffer results as source-bound JSON using the job token,
  before resource release and terminal stop. Keep resources owned when
  cleanup/export needs a retry. Log the exact failed radio setup stage and
  SDK/NimBLE result through the existing managed diagnostic path.
- Add cross-process host/production-parser interoperability coverage, exact
  nonce rejection tests, extracted radio-hook fault/cleanup coverage and an
  extracted sniffer-export lifecycle test. Preserve existing flash layouts,
  update methods and NVS regions.

See [the job recovery record](JOB_RUNTIME_RECOVERY_2026-10-12.md) for the
reproduced failures and acceptance boundaries. The repair is paired with
**WDG 0.10.3**, which also moves device health text into the C5 settings view
and detects a changed boot after an unanswered managed stop without resuming
the interrupted foreground job.

## 1.8.1 — Preserve native USB discovery commands at startup — 2026-10-11

- Skip the ESP-IDF ANSI terminal probe only for the native USB Serial/JTAG
  console and select its dumb input mode directly. The original probe consumed
  early ASCII host commands while waiting for an escape-sequence response,
  which could discard WDG's first identity/capability requests.
- Extend the pinned ESP-IDF 6.0.1 console harness to preload both metadata
  commands before REPL initialization and verify that both reach command input.
  Keep the existing machine-mode OTA input checks and normal terminal echo
  restoration. UART console profiles retain their original probe behavior.
- Pair with **WDG 0.10.2**, which fixes the separately reproduced Linux native
  USB DTR/RTS reconnect problem and retries lost discovery requests. The
  inspected XIAO's existing 1.8.0 image was booting successfully; successful
  flashing alone did not prove the host console connection was healthy.

See [USB_CONSOLE_RECOVERY.md](USB_CONSOLE_RECOVERY.md) for the diagnosis and
the distinction between live XIAO evidence and outstanding dual-board RF gates.

## 1.8.0 — USB C5 job ownership and Waveshare Zero profile — 2026-10-08

- Add opt-in `c5_jobs_v1` identity, boot nonce, immutable requests, token status,
  lease renewal, terminal retry cache, source-scoped observations and captures.
  One managed radio job owns a board; conflicts and legacy radio owners cannot
  be preempted implicitly. Cleanup acknowledgements follow callback/worker drain.
- Register the existing Wi-Fi/BLE operation families through the same job
  interface. Apply assigned channels, dwell and target descriptors directly;
  enforce exclusions on each board, and check channel/band transitions.
- Add a native-USB, no-PSRAM Waveshare Zero profile with a 4 MiB flash layout,
  factory app at `0x20000`, bounded operation resources and ROM-only updates.
  Preserve XIAO/WROOM dual-slot USB OTA and existing release asset names.
- Stream managed handshake evidence to the host for source-isolated artifact
  validation. Passive Wi-Fi helpers disable BLE and use receive-only Wi-Fi mode.
- Produce profile metadata, generated partition geometry, component offsets,
  hashes and source fingerprints. Enforce release image limits and local stack
  checks in disposable pinned ESP-IDF 6.0.1 builds.

Software validation and measured packages are recorded in
[the multi-C5 build report](MULTI_C5_VALIDATION.md). Physical acceptance remains unrun in
[HARDWARE_ACCEPTANCE_MULTI_C5.md](HARDWARE_ACCEPTANCE_MULTI_C5.md).

## 1.7.14 — Live OUI notification hints alongside wardrive batches

- Advertise `wardrive_live_oui_v1` and accept a validated optional
  `live_oui=HHHHHH,...` filter on both v2 wardrive start commands. WDG supplies
  the prefixes; firmware has no Flock/Axon labels or signature policy.
- Emit bounded `live_oui` records from the worker during collection, after the
  full observation has entered the normal ten-second batch. Match eligible
  Wi-Fi transmitters/BSSIDs and public BLE addresses only, never receivers or
  random/private BLE addresses. Callbacks remain queue-only.
- Attempt at most 32 distinct hints per batch, with no retry or evidence loss
  when serial output fails. Preserve existing batch records, standalone scan
  behavior and old-host/old-firmware compatibility through negotiation.
- Add native coverage for filter validation, callback isolation, eligibility,
  deduplication, bounds, failed delivery, cleanup and early worker ordering.
  Extend local stack-frame checks to the batch/hint path. Synthetic timing and
  build checks do not constitute physical RF/USB latency validation.

## 1.7.13 — Packet Sniffer intelligence and acknowledged Evil Twin lifecycle

- Add fresh per-session Packet Sniffer counters for total packets, BSSID-keyed
  client links and packets, and unique requester/SSID probe pairs. Remove the
  traffic-volume-triggered channel hop so the existing timer is the sole dwell
  owner. `start_sniffer_all` explicitly clears stale selected-network scope.
- Add `show_sniffer_intel TOKEN`, a bounded `SNIFF:` JSON transaction with
  summary, AP, probe and terminal records. Preserve the completed sample after
  stop and advertise `sniffer_intel_v1`, `sniffer_scope_v1` and
  `target_intel_v1`.
- Extend `hs_scan` AP rows with `clients`, `packets` and `probes`, and its final
  record with sample availability, total packets, total probes and age. Metrics
  are copied from the latest completed sniffer window; the scan token and
  existing all-or-nothing publication rules still apply.
- Add `clear_html` so a host can explicitly restore the built-in portal after a
  custom upload. Emit Evil Twin success only after its deauthentication task is
  running, clean up portal/credential state on every startup failure, and pin
  credential verification to the selected target BSSID.
- Remove submitted passwords and raw form bodies from serial logs, securely
  clear candidate credentials after failure/stop/save, and retain verified SD
  storage under the existing on-device policy.

Native harnesses cover the structured metrics, token/count bounds, HST
publication, timer-owned hopping, portal reset, BSSID-pinned verification,
redacted logs and startup cleanup. RF client attribution, dual-band dwell,
portal association and credential verification remain physically unvalidated.

## 1.7.12 — PCAPNG capture context and WPA-Sec packaging

- Emit new active handshake artifacts as PCAPNG only. Serial storage announces
  `CAPTURE_FORMAT: PCAPNG`, transfers one bounded PCAPNG block, and retains
  HCCAPX only for a validated exchange. SD storage writes `.pcapng`; no new
  handshake path writes a duplicate classic `.pcap`.
- Use radiotap IEEE 802.11 link type 127 with explicit microsecond timestamps,
  FCS-stripped frames and per-packet channel/RSSI. Retain beacon,
  authentication, association and every observed M1-M4 packet for one
  AP/station/replay exchange. PMKID and partial association evidence use the
  same format without being mislabeled as a validated EAPOL pair.
- Delay an SD commit for up to two seconds after the first M1+M2 or M2+M3 pair,
  while continuing to retain M3/M4; commit immediately once all four messages
  arrive. Serial storage already waits for capture shutdown and includes every
  retained exchange message at that point.
- Convert the older selected-network handshake serializer and the general
  `start_pcap radio|net` command to PCAPNG-only files. Radio captures include
  radiotap channel/RSSI, while net captures use the Ethernet interface type.
  The on-device WPA-Sec uploader now sends `.pcapng` captures.
- Advertise `hs_capture_pcapng_v1`. WDG 0.9.38 validates block framing and
  declared length, rebases boot-relative timestamps to wall time, saves only
  PCAPNG for new captures and prefers it for WPA-Sec upload.

Native tests cover PCAPNG section/interface/packet blocks, radiotap metadata,
FCS stripping, serial transaction failure, context retention and existing
capture ownership rules. Wireshark tools and hcxtools 7.1.2 accept the generated
files and recover their interface, channel, RSSI and 802.11 frame types. These
are synthetic/build checks; a file can only contain authentication,
association, probe and EAPOL traffic that the radio actually heard, and RF
completeness still needs field validation.

## 1.7.11 — Whitelist exclusions for active HS capture

- Advertise `hs_capture_exclusions_v1` and add
  `start_handshake_scope <sd|serial> all-except <BSSID,...>` for both active
  capture destinations. The form accepts 1-32 unique unicast BSSIDs without a
  network-scan token.
- Store the exclusion set as immutable capture-session state. Excluded AP
  frames are rejected in the promiscuous receive path, all channels remain
  eligible for the remaining all-nearby capture, and the same exclusion is
  checked again immediately before each targeted deauthentication request.
- Keep `all` and scan-token selected capture compatible with firmware 1.7.9
  clients. Malformed, duplicate, multicast, zero, empty, or oversized exclusion
  lists fail closed with `invalid_targets` before radio mode changes.

Native tests cover boundary-sized exclusion lists, frame filtering, channel
behavior, command validation, cleanup, and the final pre-deauthentication scope
check. RF behavior still requires field validation on the XIAO/uConsole setup.

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
