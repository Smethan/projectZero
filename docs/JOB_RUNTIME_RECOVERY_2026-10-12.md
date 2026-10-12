# Managed C5 job recovery, 2026-10-12

The host repair is **WatchDogsGo 0.10.3**. Firmware **1.8.2** repaired nonce
admission and sniffer cleanup; the follow-up **1.8.3** candidate addresses
first-start combined radio setup.
The inspected uConsole had one XIAO connected over native USB. The Waveshare
Zero was not attached. This record separates actual device behavior from
software checks and the remaining multi-board acceptance gates.

## Confirmed failures

WDG normalized the firmware's lowercase hexadecimal boot nonce to uppercase
for internal identity comparisons, then serialized that uppercase value in
`job_start`. Firmware through 1.8.1 compared the request with its lowercase
nonce using `strcmp`, rejecting jobs with `invalid_request`. A live A/B check
sent otherwise identical receive-only requests: the normal host encoding
failed; the lowercase request started, received Wi-Fi observations, and
stopped cleanly.

WDG now lowercases its detached wire request while retaining canonical host
identities and stale-boot checks. Firmware 1.8.2 accepts either hexadecimal
case for the same sixteen digits, rejecting changed digits, short values,
and non-hex characters. Tokens and request hashes retain their ownership and
retry rules. The new host regression compiles the production firmware parser
with real cJSON, feeds firmware-emitted metadata through the host scheduler
and encoder, and verifies matching started/stopped records. A deliberately
different nonce and oversized targets/exclusions remain rejected.

After the nonce repair, GUI All Wardrive, Wi-Fi Scan, and BLE Scan collected
observations. Packet Sniffer still appeared to run but could not acknowledge
its stop. Direct serial capture reproduced a **stack-protection panic in
`sys_evt`** when the initial sniffer scan completed. The panic reported
`MEPC=0x4219a410`; the matching 1.8.1 ELF resolves it to newlib's
`_svfprintf_r`. The device rebooted and changed its boot nonce. The Wi-Fi event
callback's managed-log path adds a 1024-byte record buffer to its existing
formatting call chain; the previous event-task configuration was 2304 bytes.

The event-task stack is increased to 4096 bytes. The sniffer channel worker
also receives a named 4096-byte stack allocation for its managed status/output
call chain. Zero admission accounts for that worker stack as part of the
48 KiB job-owned budget, and retains the 32 KiB remaining-heap requirement.
These task allocations and their library callees are separate from the local
frame guards; passing a local-frame check alone did not detect this crash.

Managed sniffer stop also invoked the human-readable result printer instead
of its structured exporter. Cleanup now exports source-bound JSON summary,
AP/probe, and done records using the job token before releasing its resources.
The channel worker finishes its final output and clears its own task handle
before resource release; stop waits up to 1.5 seconds and keeps ownership on
timeout instead of deleting a producer that may hold the output lock.
Failed cleanup/export retains ownership for a bounded retry. Native tests
exercise this ordering and failure path.

The post-update first GUI All Wardrive start on 1.8.2 reproduced a combined
radio startup failure. Its new diagnostic reported
`Serial radio start failed at ble_scan_coex: 519`. In the pinned IDF 6.0.1
NimBLE headers, 519 is the HCI error base (512) plus status `0x07`, controller
Memory Capacity Exceeded. NimBLE initialization had succeeded. A subsequent
direct sequence of Wi-Fi-only, combined Wi-Fi/BLE, BLE-only, and combined-band
jobs all collected observations and acknowledged stops without a reset.

Combined serial receive jobs previously retained STA mode while Wi-Fi-only
and BLE-only jobs switched to NULL mode. The 1.8.3 candidate applies NULL mode
to combined reception before BLE scan setup too. It tracks the actual mode
change, restores the saved mode on normal stop and startup failure, and
retains a failed restore for a checked retry before another serial job.
The first-start physical A/B check passed on the connected XIAO: immediately
after candidate OTA/reboot, normal GUI All Wardrive accepted 91 Wi-Fi and 203
BLE observations, included both Wi-Fi bands, and acknowledged stop without a
reset. The HCI status alone does not establish the controller's precise
allocation mechanism, but applying NULL mode eliminated this reproduced
first-start failure. Wi-Fi and BLE scans then completed, and Packet Sniffer
exported 73 packets with a clean stop and no reset.

The immediate Sniffer-to-passive-HS transition exposed a separate lifecycle
race: `job_stop` emitted `stopped` while its external job supervisor could
remain asleep for another two seconds. The next start was rejected with
`busy_cleanup`. The failure record was preserved before a follow-up cleanup
barrier candidate. Terminal acknowledgements must follow retirement of that
supervisor as well as radio callbacks and export producers.

The final candidate's live GUI sequence passed that transition: Sniffer
exported 107 packets, passive HS sniff stored 124 complete frames, and a
second Sniffer export contained 135 packets. Passive evidence reported two
lost frames and no EAPOL/PMKID or validated exchanges; this establishes frame
transport/storage, not a usable handshake. The supervisor now owns eventual
terminal completion even if a console wait times out, and replacement
admission waits for its last shared-state access to finish. Native tests also
cover failed starts, Zero low-floor cleanup, and delayed retirement followed
by a new start.

The complete final candidate GUI sequence passed all seven receive-only
steps, then preempted background All Wardrive with Sniffer and restored a new
Wi-Fi/BLE background job. Restored coverage accepted 38 Wi-Fi and 36 BLE
observations. The boot nonce stayed unchanged throughout; final stop left the
endpoint idle. Across the sequence, WDG accepted 384 Wi-Fi, 422 Wi-Fi
management and 431 BLE records. The final restored job reported 22 output
losses (zero queue, age or saturation losses); these counters are retained as
observed backpressure, not presented as zero-loss reception.

The final candidate XIAO application was 2,299,184 bytes, SHA-256
`f8ef470e27550449beb940a7fca6583253b168c6c1ab57595593564785a90235`.
All three pinned profile builds and stack/geometry checks passed. The full
host suite passed 1,296 tests with the new production-parser harness, and the
firmware native harness passed all 27 groups. Release workflow and exact
published-image validation are recorded separately after publication.

## Host recovery and display

An ESP soft reset can leave its native USB endpoint open. After an unanswered
managed stop, WDG now requests fresh identity with a throttled read-only probe
on that endpoint. A verified changed nonce retires the previous job/capture
state and requests fresh capabilities. A same-boot reply does not release a
possibly running owner. Update reservations, external legacy ownership and
selected serial transfers suppress these probes. Interrupted foreground jobs
require a new user start.

The board/foreground/background health text is moved from the map overlay to
**WARDRIVE settings → USB C5 devices and roles**, where band, channel, job,
health and drop details can be inspected without masking the main status line.

## Acceptance boundary

Builds use the pinned ESP-IDF 6.0.1 image, disposable source copies, native
protocol tests, and stack/geometry checks. Package manifests and logs publish
the exact application sizes, flash layout and checksums. Radio/task runtime
checks use the connected XIAO; a linker RAM report is not a physical free-heap
measurement on the Zero.

The dual-board soak, simultaneous USB/RF operation, Zero runtime heap and
helper transitions remain in
[HARDWARE_ACCEPTANCE_MULTI_C5.md](HARDWARE_ACCEPTANCE_MULTI_C5.md).

## Published software and update validation

Both fork release workflows passed. WDG **0.10.3** is source commit
`ac9d7e09f6e15e7930d2f847e1c3f01e694d5877`; firmware **1.8.2** is
`6bbd6e07bfde294d0597e71c0835e470a8fd28e1`. The complete host suite passed
**1,296 tests**, the native firmware harness passed **27 groups**, and
**193 targeted tests** passed on the uConsole's ARM64/Python 3.13 runtime,
including the compiled host/firmware interoperability test. The published
packages passed host checksums, profiles, partition geometry, physical flash
bounds and NVS-region checks.

| Published profile | Padded app bytes | Hard app limit | Headroom below limit | Static HP SRAM | Linker HP SRAM remaining | Local frame guards |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| XIAO | 2,298,576 | 4,063,232 | 1,764,656 | 233,895 | 87,033 | 42 |
| WROOM | 2,302,432 | 4,063,232 | 1,760,800 | 233,707 | 87,221 | 42 |
| Waveshare Zero | 1,911,232 | 3,997,696 | 2,086,464 | 183,931 | 136,997 | 36 |

These RAM values are linker measurements. Zero retains no PSRAM and its
ROM-only factory layout; XIAO/WROOM retain dual-slot USB OTA.

The exact published XIAO binary, SHA-256
`cc1f50bbea52b85972903edc11a875fb15dcb9f5842c543fad4ca2a63814eb0a`,
was installed through WDG's normal GUI USB-OTA path. It reported
`Verified v1.8.2 after reboot (valid OTA slot).` WDG released the endpoint
reservation, received a fresh nonce and capabilities, and returned to its
normal reader while the GUI remained responsive. The saved settings hash
was preserved.
