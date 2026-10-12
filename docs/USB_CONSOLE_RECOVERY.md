# Native USB capability recovery, 2026-10-11

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
