"""Extract the production serial radio start/stop hooks for native fault tests."""
from pathlib import Path
import sys

source=Path('../../ESP32C5/main/main.c').read_text()
start=source.index('static wifi_band_mode_t sw_saved_band;')
hop=source.index('\nvoid sw_radio_hop(',start)
Path(sys.argv[1],'sw_radio_start_under_test.inc').write_text(source[start:hop])
stop=source.index('\nbool sw_radio_stop(void) {',hop)+1
end=source.index('\nstatic bool hs_radio_snapshot',stop)
Path(sys.argv[1],'sw_radio_stop_under_test.inc').write_text(source[stop:end])
