# Application-mode USB OTA (1.7.7+)

Install firmware 1.7.7 once using the existing Wi-Fi updater. WDG's USB OTA
method then downloads a verified release on the host and writes only the inactive
application partition. No ROM bootloader entry, esptool, SD card, partition-table
write, bootloader write or full-chip erase is involved.

Commands and responses are newline-framed. Responses start `UOTA:` and carry
JSON version 1, kind, board, SHA256, size, offset, active flag, slot and a bounded
error identifier. Every protocol output write is at most 64 bytes because the
SDK's console TX ring is 256 bytes. Console output locking covers the whole line;
all transport waits are bounded. The host waits for each ACK. Legacy requests
are paced; negotiated large blocks use one unpaced write.

- `uota_status`: report current/resumable image and offset, plus compiled board.
- `uota_begin <size> <sha256>`: begin or resume that image. A different pending
  image requires explicit abort first. Running/boot slot must match and be VALID.
  Stop radios before allocating the OTA write handle. Conflicting console commands
  are rejected while the handle is active.
- `uota_chunk <sha256> <offset> <crc32-decimal> <lowercase-hex>`: at most 256 bytes,
  aligned offsets and exact expected chunk lengths. CRC32 matches zlib. Accept an
  in-order chunk once; compare retransmissions with flash and re-ACK without
  rewriting. Acknowledged offset is the next byte to send.
- `uota_finish <sha256>`: only after all bytes arrived. Read back and SHA256 the
  entire application; check project descriptor and `esp_ota_end` image validation;
  then select the inactive slot, report applied and reboot normally.
- `uota_abort <sha256>`: close the pending handle and remove its resume metadata.
  It does not change the running/boot selection or erase the running firmware.

Every complete 4096-byte boundary is committed to NVS after its flash write.
USB disconnect without reboot retains the live offset. Following a reboot,
metadata is accepted only for the inactive slot with matching address, bounds,
format and image hash. Resume erases from the saved sector boundary through the
unfinished image tail and calls `esp_ota_resume`. Thus a torn final sector is
resent, and a partially written image cannot become the boot target. Checksum,
flash and NVS errors remain failures, never successful completion.

This is integrity checking of a user-selected fork release, not a new firmware
signature or access-control mechanism. The host must verify the GitHub release
manifest/hash and board before sending it. Physical console access still permits
firmware updates, as with the pre-existing USB/OTA tools.

Native tests link OpenSSL for an independent SHA256 implementation (`libssl-dev`
on Debian/Ubuntu). Tests cover block retries, checkpoint recovery, erase boundaries,
SHA mismatch, flash/NVS failure, pending boot validation and explicit abort.

The build applies a checked patch to the pinned ESP-IDF console: USB OTA uses
its normal command parser with input echo disabled until finish/abort. This
prevents per-character USB flushes from overrunning the RX ring. Ordinary
terminal behavior is restored afterward; no SDK patch is needed on the host.


## Fast native USB path (1.7.8 / WDG 0.9.24)

XIAO `UOTA:` responses add `block_size:4096`, `encoding:"base64"`, and
`line_size:8192`. WDG negotiates these exact values from each `ready` reply;
unknown/missing values and WROOM use the original format.

`uota_block <sha256> <offset> <crc32-decimal> <base64>` transfers up to 4096
firmware bytes. A typical command is about 5.6 KiB, versus sixteen roughly
600-byte commands for the same image data previously. The build patches the
pinned native USB REPL's RX ring to 8192 bytes and the application sets the
console line limit to 8192. The checked patch rejects unknown or stale SDK
patterns. Both block commands and older hex commands remain available.

Only exact sector-completing lengths are accepted: 4096 bytes at a sector
boundary, the remainder of a sector when resuming a legacy chunk transfer,
or the remaining image bytes at the end. Base64 validation rejects whitespace,
invalid alphabet/padding and noncanonical tails. Decoding uses a fixed 4 KiB
buffer, never a large local stack frame. Duplicate comparisons use 256-byte
scratch storage. Integrity and checkpoints are shared by both formats.

The host retains a single outstanding block, rather than pipelining uncertain
writes. After two consecutive large-block failures it falls back to the
256-byte format for this run, preserving the receiver's saved offset. A fresh
run may negotiate fast mode again. Reconnect line clearing covers the entire
8192-byte command limit. No port-number fallback or network reconfiguration.

Final SHA256 reads still check the entire image, but the task yields every
64 KiB instead of each KiB. The first upgrade from 1.7.7 must use that older
receiver's speed; subsequent USB transfers can use the fast receiver. Timing
requires a hardware measurement, not inference from block sizes or tests.
