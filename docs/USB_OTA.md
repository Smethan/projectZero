# Application-mode USB OTA (1.7.6+)

Install firmware 1.7.6 once using the existing Wi-Fi updater. WDG's USB OTA
method then downloads a verified release on the host and writes only the inactive
application partition. No ROM bootloader entry, esptool, SD card, partition-table
write, bootloader write or full-chip erase is involved.

Commands and responses are newline-framed. Responses start `UOTA:` and carry
JSON version 1, kind, board, SHA256, size, offset, active flag, slot and a bounded
error identifier. Every protocol output write is at most 64 bytes because the
SDK's console TX ring is 256 bytes. Console output locking covers the whole line;
all transport waits are bounded. The host paces requests and waits for each ACK.

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
