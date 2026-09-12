# Smethan fork firmware

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
