# Multi-device C5 software validation

The matching versions are **WatchDogsGo 0.10.0** and **projectZero 1.8.0**.
The implementation passed the software checks below on 2026-10-08, with host
and native checks repeated before publication on 2026-10-09. **Physical
acceptance is NOT RUN.** Build success does not establish radio performance,
USB behavior under load, runtime heap fragmentation, or update reliability.

## Software checks

| Check | Result |
| --- | --- |
| Complete WatchDogsGo suite | 1,232 tests passed |
| Firmware native/protocol harness | 25 test groups passed |
| WROOM, XIAO and Waveshare Zero builds | All passed with pinned ESP-IDF 6.0.1 |
| Local stack-frame guards | WROOM 37, XIAO 37, Zero 31 passed |
| Generated packages | All three passed host hash, profile, partition, flash-bound and NVS checks |
| Native firmware records parsed by WDG | Metadata, lifecycle and terminal fixtures passed for all three profiles |
| ESP image validation | C5 chip, version 1.8.0, 8/8/4 MiB flash, image checksum and hash verified |

The fixtures cover device/job identity, lost acknowledgements, compensation,
leases, source-isolated capture chunks, target/exclusion capacity, helper
transitions, allocation failures, country/channel errors, portal cleanup,
profile rejection and handshake exchange validation. They do not substitute
for hardware observations.

## Measured local builds

Application sizes are the actual padded binaries. HP SRAM remaining is a linker
measurement, not free heap measured on a running board. These measurements refer
to the validated local builds; release builds publish their own build and stack
logs alongside their packages.

| Profile | App bytes | Hard release cap | Headroom below cap | Static HP SRAM | Linker HP SRAM remaining |
| --- | ---: | ---: | ---: | ---: | ---: |
| WROOM | 2,301,520 | 4,063,232 | 1,761,712 | 233,707 | 87,221 |
| XIAO | 2,297,952 | 4,063,232 | 1,765,280 | 233,895 | 87,033 |
| Waveshare Zero | 1,910,624 | 3,997,696 | 2,087,072 | 183,931 | 136,997 |

Zero uses 4 MiB flash, no PSRAM, size optimization with assertions retained,
and disabled optional Wi-Fi IRAM optimizations. Its admission contract limits
job-owned internal allocations, including worker stacks, to 48 KiB and requires
at least 32 KiB internal heap after admission and radio initialization. Native
tests exercise each allocation-stage failure and 700 transitions using the
target ABI sizes. Hardware heap fragmentation and task high-water marks remain
unmeasured.

## Build and package provenance

The [release workflow](../.github/workflows/fork-release.yml) builds each profile
in a disposable source copy with this pinned container:

```text
espressif/idf:v6.0.1@sha256:efc19fae2f52fc6873630c668da26aa834139a063b5fb73a46ebc0dbd217b587
```

The existing bounded console-receive patch is applied to that SDK, so its
version string reports `v6.0.1-dirty`. No device is attached to these builds.
Package format 2 manifests contain generated partition geometry, component
offsets/sizes/hashes, board/console/update profile, source commit/dirty state,
and a build-source SHA256. `SHA256SUMS` covers the release ZIPs, application
binaries and logs. Use the release package's manifest and logs for the exact
published artifact measurements.

Full installs write the bootloader at `0x2000`, partition table at `0x8000`,
initial metadata at `0xF000`, and application at `0x20000`. They preserve NVS at
`0x9000` through `0xEFFF`. XIAO/WROOM retain dual application slots and USB OTA;
Zero has one factory application and supports ROM flashing only.

## Hardware acceptance

All physical gates remain open in
[HARDWARE_ACCEPTANCE_MULTI_C5.md](HARDWARE_ACCEPTANCE_MULTI_C5.md), including
simultaneous USB enumeration and 2.4/5 GHz reception, Wi-Fi/BLE coexistence,
Zero standalone jobs, helper transmit silence, controlled target routing,
disconnect/lease cleanup, updating one device while its peer collects,
repeated foreground/helper transitions and the one-hour dual-board soak.

For operation and settings, see
[WatchDogsGo's multi-device guide](https://github.com/Smethan/WatchDogsGo/blob/v0.10.0/docs/MULTI_C5.md).
The [serial protocol](SERIAL_WARDRIVE_PROTOCOL.md) defines the job interface.
