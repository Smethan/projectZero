"""The SDK RX patch must be repeatable and reject unknown/stale input."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
patch = root / 'ESP32C5/tools/patch_idf_usb_ota_buffer.cmake'
hook = 'usb_serial_jtag_driver_config_t usb_serial_jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();'
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'components/console/esp_console_repl_chip.c'
    source.parent.mkdir(parents=True)
    env = dict(os.environ, IDF_PATH=tmp)
    def run():
        return subprocess.run(['cmake', '-P', str(patch)], env=env, capture_output=True)
    source.write_text(hook)
    assert run().returncode == 0
    patched = source.read_text()
    assert 'rx_buffer_size = 8192' in patched
    assert run().returncode == 0 and source.read_text() == patched
    source.write_text('unrecognized SDK')
    assert run().returncode != 0
    source.write_text(patched.replace('8192', '256'))
    assert run().returncode != 0
print('PASS: USB RX patch repeats unchanged and rejects unknown SDK / stale buffer size')
