# Multi-device C5 hardware acceptance record

Status at 2026-10-08: **all physical gates NOT RUN**. Source and native test
coverage do not establish USB behavior under load, RF performance, transmit
silence, update continuity, or sustained uConsole operation. Software checks
for firmware 1.8.0 / WatchDogsGo 0.10.0 passed: 1,232 host tests, 25 native
groups and all three pinned builds/stack/package checks. Actual sizes and
software evidence are in [the validation report](MULTI_C5_VALIDATION.md).

Record the exact board revision, firmware and host versions, USB endpoint,
configuration, start/end time, observations, and artifact paths for every gate.
Keep target-routing and handshake experiments on a controlled network with
authorized equipment.

| Gate | Acceptance evidence | Status |
| --- | --- | --- |
| Both USB devices | XIAO and Waveshare Zero enumerate together; device and boot identities remain distinct across reconnects. | **NOT RUN** |
| Concurrent 2.4/5 GHz capture | XIAO remains on the assigned 2.4 GHz plan while Zero captures assigned 5 GHz channels; per-device records and channel attribution remain separate. | **NOT RUN** |
| BLE preset | Default split Wi-Fi/BLE and dedicated-C5-BLE presets complete repeated start/stop cycles; separately check the host-BLE option and record coexistence loss, scanner stability, and restoration gaps. | **NOT RUN** |
| Waveshare Zero jobs | Run supported observation and job lifecycle operations; verify no PSRAM assumption, target capacity 8, disconnect lease cleanup, and ROM-only update capability reporting. | **NOT RUN** |
| Helper receive silence | During helper handshake receive on target channels, verify the helper emits no Wi-Fi/BLE transmissions; record sniffer or monitor evidence and BLE-off state for Wi-Fi receive mode. | **NOT RUN** |
| Target routing | On a controlled network, select distinct targets across supported channels; verify each immutable BSSID/channel/auth/SSID tuple routes only to its assigned device and captures stay job-isolated. | **NOT RUN** |
| Unplug and lease cleanup | Unplug each device during active work; verify host watchdog/status/stop behavior, firmware cleanup at lease expiry, and that reconnect does not automatically restart work. | **NOT RUN** |
| Update while peer collects | Update one selected device while another continues collecting; verify only the selected USB endpoint is reserved and peer records keep arriving. | **NOT RUN** |
| Repeated foreground/helper transitions | Repeat passive preemption, one/two-device foreground groups, helper reception, clean stop and background restoration; record leaks, channel gaps and task high-water marks. | **NOT RUN** |
| One-hour dual-Wardrive soak | Run both devices for one hour with repeated screen transitions, group/global stop, and restarts; record heartbeat gaps, drops, memory/thermal state, file integrity, and cleanup. | **NOT RUN** |

Do not mark a gate passed from simulated records, host tests, native firmware
harnesses, build success, manifest validation, or firmware size checks alone.
