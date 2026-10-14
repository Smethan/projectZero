"""Extract the production PCAP stop barrier for native fault-injection tests."""
from pathlib import Path
import sys

source = Path("../../ESP32C5/main/main.c").read_text()
start = source.index("\nstatic bool pcap_capture_stop(void) {") + 1
end = source.index("\nstatic void pcap_arp_spoof_task(", start)
Path(sys.argv[1], "pcap_stop_under_test.inc").write_text(source[start:end])
