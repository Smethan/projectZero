# Native USB capability recovery, 2026-10-11 and 2026-10-15

## Recurrence triage, 2026-10-15

The same uConsole again showed a XIAO (`303a:1001`) enumerated as
`/dev/ttyACM0` while WDG 0.10.4 received no capability or identity reply.
With WDG closed and no serial owner, a bounded console query and a read-only
esptool ROM probe both timed out writing to the USB endpoint. Enumeration alone
therefore did not establish a working firmware console or ROM link. The version
that was installed at the start of this recurrence remains unknown.

Physical BOOT mode identified this XIAO as **ESP32-C5 revision 1.0**. A
checksummed 1.8.5 candidate flashed successfully, and a physical RESET with
BOOT released produced a complete boot log through `BOARD READY`. The USB REPL
printed its prompt, yet the Pi's first `version` write then stalled even with
one reader, no WDG process, and either static DTR setting. This separates the
current failure from a boot/PSRAM abort, a capability parser error, and a
missing serial device.

Espressif [reproduced a revision 1.0 native USB console clock bug](https://github.com/espressif/esp-idf/issues/18089)
and fixed it for revision 1.1 by retaining UART0's source clock when USB
Serial/JTAG is the primary console. The pinned ESP-IDF 6.0.1 `esp32c5/clk.c`
still disables that clock for this profile. The guarded build patch in
`ESP32C5/tools/patch_idf_c5_usb_clock.cmake` applies Espressif's
revision-aware condition to the exact pinned hook and rejects unexpected SDK
source. On the same revision 1.0 XIAO, the image with this guard booted and
answered `version` and `get_capabilities` after a physical reset. That is a
live confirmation that the USB input path now drains. It did not yet complete
the full identity handshake.

The first guarded image exposed a second, independent metadata defect:
`get_device_info` returned console error 1 with no `WDG:` record, while
`get_capabilities` was valid. The pinned release config sets
`CONFIG_APP_RETRIEVE_LEN_ELF_SHA=9`. ESP-IDF's
`esp_app_get_elf_sha256()` is limited by that setting, but the command
required a 16-digit build ID and returned silently when it received only
nine digits. The command now encodes the first eight bytes of the complete
`esp_app_desc_t.app_elf_sha256` into a fixed 16-digit ID, independent of the
display-length setting. Its other identity/channel/app-descriptor failures
emit typed protocol errors. The native test stub models the shipping nine
digit formatter so this mismatch cannot be hidden by the test fixture again.

The locally rebuilt XIAO image with both repairs returned `version`, ready
`get_device_info`, and `get_capabilities` with `c5_jobs_v1=true`, 8 MiB flash,
8 MiB PSRAM, and the exact 16-digit ELF build ID. Ten consecutive WDG
`SerialManager` open/query/close cycles returned all three records with one
stable boot nonce. The corrected standalone WDG diagnostic also reported All
Wardrive supported. These checks used the staged 1.8.5/0.10.5 source and a
locally built image; the published CI image and installed WDG checkout must
still be checked separately.

Esptool's reported `Hard resetting via RTS pin` after this board's ROM write
left the revision 1.0 XIAO in its flasher stub. A no-reset ROM query confirmed
the stub remained active. For manual ROM recovery on this board, do not infer
application boot from esptool's reset message: release BOOT, physically RESET,
then require `BOARD READY` and a complete identity/capability handshake. The
clock-guarded application did permit esptool's normal **entry into** ROM mode;
that does not prove the post-write reset exited it.

Separately, ESP32-C5 GPIO13/14 are native USB D-/D+. The legacy GPS UART
mapping also used GPIO13/14; invoking a GPS-backed mode could steal the USB
pins after startup. Native-USB builds now map the XIAO GPS UART to exposed
D6/TX GPIO11 and D7/RX GPIO12; WROOM retains its original pins. The
Waveshare Zero profile refuses to initialize its unavailable GPS UART. This
pin conflict was **not** the cause of the boot-time stall above because no
GPS command ran before that observation.

The standalone WDG diagnostic had still used raw pySerial line changes and
`flush()`; it could recreate the previously verified stall path even though
normal WDG connections already used the atomic native USB line handling.

Use one serial owner at a time. **Do not run the 0.10.4 standalone diagnostic**;
it still uses raw pySerial and `flush()`. After installing the companion WDG
host fix, close WDG and other monitors, then run the corrected diagnostic
through its `SerialManager` from the installed checkout:

```sh
cd /home/pi/python/WatchDogsGo
.venv/bin/python -m watchdogs.diagnose_firmware --port /dev/ttyACM0
```

Substitute the actual endpoint after checking USB identity. A `Write timeout`
is a transport failure, not a negative capability report. The companion WDG
change bounds metadata writes, avoids `flush()` on console commands, reports
transport and typed firmware metadata errors separately, and retries missing
metadata with backoff. If writes still time out, reconnect only the affected
XIAO USB link;
if that does not restore replies, use physical BOOT/download mode and a known
good cable/host before considering a profile-checked firmware recovery.
Neither a host reboot nor a USB rail cycle recovered the earlier silent-board
incident, so do not treat them as proof of repair. Preserve NVS and unrelated
peripheral state during recovery.

Before calling a repair complete, obtain `version`, `get_capabilities`, and
`get_device_info` replies from the actual patched board, then verify WDG's
reopen and post-update handshake on that board. Repeat the query after a
physical reset and after a fresh serial open. A responsive boot log alone does
not satisfy this gate. The XIAO is the only attached C5 in this investigation;
Waveshare Zero requires its own hardware check. On the tested XIAO, the local
candidate passed this gate after physical reset; repeat it for the published
artifact and installed WDG before calling the release install verified.

## 2026-10-11 repair record

The matching repair versions are **WatchDogsGo 0.10.2** and **projectZero 1.8.1**.
This repair addresses console startup and reconnection. It does not change
radio operations, board flash geometry, partitions, or update methods.

## Diagnosis

The inspected uConsole had one Seeed XIAO ESP32-C5 connected through native
USB Serial/JTAG (`303a:1001`). Its manual full installation of projectZero
1.8.0 had completed and passed esptool's flash hash verification. With one
continuously read serial handle kept open, the board reached `BOARD READY`,
answered `version`, and returned valid capability and device-info records:
8 MiB flash, 8 MiB PSRAM, the XIAO profile, and `c5_jobs_v1`. WDG's parser
accepted both records. A startup/PSRAM crash was not the cause of the observed
failure on this board.

Two independently verified console problems explain the missing discovery:

1. Linux `cdc_acm` can assert both modem lines on open. pySerial 3.5 then applies
   the requested DTR and RTS states separately. On this XIAO/Pi connection, the
   intermediate RTS-only state reproducibly left console queries unanswered.
   Applying the two lines together avoided that intermediate state. This
   change is limited to identified native Serial/JTAG devices on Linux; it is
   not a replacement for esptool's intentional ROM reset sequences.
2. ESP-IDF 6.0.1's `linenoiseProbe()` sends an ANSI status query and consumes
   stdin for up to 500 ms while searching for an escape response. Early ASCII
   metadata commands can be discarded. WDG previously sent its initial
   queries without automatic unanswered-query retries, and did not read the
   console during its boot screen.

The relevant implementation paths are WDG `watchdogs/serial_manager.py`,
`device_session.py`, `multi_device.py`, and `app.py`; firmware
`ESP32C5/tools/patch_idf_linenoise_machine.cmake`. The SDK patch bypasses the
probe only when `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` is enabled. UART keeps the
original probe. Host retries ask only for missing metadata, drain available
records first, and use a bounded interval. Explicit refresh queries a known
C5's identity before refreshing capabilities. Active jobs, external owners,
updater reservations, and selected legacy serial transfers retain ownership.

## Validation boundary

The actual production WDG serial code passed **ten consecutive
open/query/close cycles** on the uConsole's XIAO running 1.8.0. Every cycle
received capability and identity records. All ten replies had the same boot
nonce, so these reconnects did not reboot the board. An earlier diagnostic
using atomic deassertion also passed four reopens; reverting to the original
sequence reproduced missing replies. Recovering the resulting stalled
connection required a controlled USB rail cycle. GPS and LoRa power stayed on.

The firmware SDK harness preloads `get_device_info` and `get_capabilities`
before initialization and verifies that both survive the patched probe and
reach the existing command input loop. It retains the no-per-byte-flush USB
OTA check, normal echo restoration, and idempotent SDK patch checks. Native
protocol tests, three pinned board builds, stack guards, and package geometry
checks are separate software gates; use the release's build logs and manifest
for exact image measurements.

The complete host suite passed **1,272 tests**, the firmware native/protocol
harness passed **25 groups**, and all three profiles built with the pinned
ESP-IDF 6.0.1 container. Stack guards passed 37 checks each for XIAO/WROOM and
31 for Zero. Generated geometry checks confirmed the console profile, flash
bounds, application limits, and NVS preservation for each build.

| Profile | Padded app bytes | Hard app limit | Headroom below limit | Static HP SRAM | Linker HP SRAM remaining |
| --- | ---: | ---: | ---: | ---: | ---: |
| XIAO | 2,297,664 | 4,063,232 | 1,765,568 | 233,895 | 87,033 |
| WROOM | 2,301,520 | 4,063,232 | 1,761,712 | 233,707 | 87,221 |
| Waveshare Zero | 1,910,400 | 3,997,696 | 2,087,296 | 183,931 | 136,997 |

These are measured local 1.8.1 candidate builds. HP SRAM remaining is a linker
measurement, not a running-board free-heap result. Published builds provide
their own logs and checksums. Zero keeps PSRAM disabled and its existing
4 MiB, single-factory-app layout; XIAO/WROOM retain their dual OTA slots.

## Published image and live WDG USB OTA

After release CI passed all three builds, the exact published XIAO 1.8.1
application was installed through **WDG 0.10.2's real GUI USB-OTA path** on
the inspected uConsole. WDG validated the selected board profile and package
geometry, transferred the image, checked its complete hash, rebooted the
board, and reported `Verified v1.8.1 after reboot (valid OTA slot).`

The result screen was closed through its normal cleanup path. WDG released
the endpoint reservation, reopened the same physical USB identity, received
fresh device-info and capability records, and returned to normal polling.
The GUI remained responsive through the transfer and after reconnection.
The board reported XIAO, 8 MiB flash, 8 MiB PSRAM, `c5_jobs_v1`, and zero
reported drops while idle. GPS and SX1262 services remained active; their
power states and the saved WDG settings hash were preserved.

The physically installed published application is **2,297,664 bytes**, SHA-256
`e03ec97df2e05a61fa29b67b25e5bf9e029a78982e447abc34f4a359d0674e58`.
Its manifest identifies source commit
`bc7a254b9763f7f9772d0cf77c48ad31c04841a0`, with a clean source state.
Published WROOM and Zero package hashes, profiles, partitions, flash bounds,
and sizes also passed host verification; they match the sizes above.

This single-XIAO console/update validation does not complete the multi-board hardware
acceptance record. The Waveshare Zero was not attached. Simultaneous RF band
coverage, BLE coexistence, helper transmit silence, controlled target routing,
peer collection during updates, and the one-hour dual-board soak remain open
in [HARDWARE_ACCEPTANCE_MULTI_C5.md](HARDWARE_ACCEPTANCE_MULTI_C5.md).
