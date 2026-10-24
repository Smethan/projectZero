"""Compile the production startup function with fault-injecting SDK shims."""

from pathlib import Path
import sys

source = Path("../../ESP32C5/main/main.c").read_text()
country_start = source.index("static esp_err_t wifi_apply_us_country(void) {")
wifi_start = source.index("static esp_err_t wifi_init_ap_sta(void) {", country_start)
wifi_end = source.index("\nstatic void wifi_get_24ghz_channel_bounds", wifi_start)
Path(sys.argv[1], "country_under_test.inc").write_text(source[country_start:wifi_start])
Path(sys.argv[1], "wifi_init_under_test.inc").write_text(source[wifi_start:wifi_end])
