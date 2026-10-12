# Managed C5 job recovery, 2026-10-12

The repair versions are **WatchDogsGo 0.10.3** and **projectZero 1.8.2**.
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
Failed cleanup/export retains ownership for a bounded retry. Native tests
exercise this ordering and failure path.

One initial combined-radio start reported `radio_start_failed`, but subsequent
direct Wi-Fi, BLE, combined-band checks and a fresh-boot GUI start succeeded.
That individual setup failure has no confirmed substep cause. Firmware now
logs the failing radio setup stage and its SDK/NimBLE return code, and WDG
retains this bounded, source-tagged diagnostic in its terminal and log.

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
