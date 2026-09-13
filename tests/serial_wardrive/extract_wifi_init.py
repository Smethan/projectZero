"""Compile the production startup function with fault-injecting SDK shims."""
from pathlib import Path
import sys

source = Path('../../ESP32C5/main/main.c').read_text()
start = source.index('static esp_err_t wifi_init_ap_sta(void) {')
end = source.index('\nstatic esp_err_t wifi_apply_extended_country', start)
Path(sys.argv[1], 'wifi_init_under_test.inc').write_text(source[start:end])
