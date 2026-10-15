"""The C5 USB clock workaround must be exact, repeatable, and fail closed."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "ESP32C5/tools/patch_idf_c5_usb_clock.cmake"
BASE = """#include "hal/efuse_hal.h"
void configure_clocks(void) {
#if CONFIG_ESP_CONSOLE_UART_NUM != 0
    clk_gate_config.disable_uart0_clk = true;
#endif
#if CONFIG_ESP_CONSOLE_UART_NUM != 1
    clk_gate_config.disable_uart1_clk = true;
#endif
}
"""


def run(idf_path: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["cmake", "-P", str(PATCH)],
        env=dict(os.environ, IDF_PATH=str(idf_path)),
        text=True,
        capture_output=True,
    )


with tempfile.TemporaryDirectory() as tmp:
    idf = Path(tmp)
    clk = idf / "components/esp_system/port/soc/esp32c5/clk.c"
    clk.parent.mkdir(parents=True)

    # First configure patches only the UART0 block; a repeated configure is a
    # byte-for-byte no-op, and the separate UART1 gate remains unchanged.
    clk.write_text(BASE)
    first = run(idf)
    assert first.returncode == 0, first.stdout + first.stderr
    transformed = clk.read_text()
    assert "PROJECTZERO_C5_USB_UART0_CLOCK_REV1" in transformed
    assert "#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG" in transformed
    assert "if (efuse_hal_chip_revision() > 100)" in transformed
    assert "#else\n    clk_gate_config.disable_uart0_clk = true;\n#endif" in transformed
    assert (
        "#if CONFIG_ESP_CONSOLE_UART_NUM != 1\n    clk_gate_config.disable_uart1_clk = true;\n#endif"
        in transformed
    )
    assert run(idf).returncode == 0
    assert clk.read_text() == transformed

    # A valid upstream revision-aware fix is accepted without adding a second
    # patch, and an unknown or partially changed SDK is rejected.
    upstream = BASE.replace(
        "    clk_gate_config.disable_uart0_clk = true;",
        "#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG\n"
        "    if (efuse_hal_chip_revision() > 100) {\n"
        "        clk_gate_config.disable_uart0_clk = true;\n"
        "    }\n#else\n"
        "    clk_gate_config.disable_uart0_clk = true;\n#endif",
    )
    clk.write_text(upstream)
    assert run(idf).returncode == 0
    assert clk.read_text() == upstream
    clk.write_text(upstream.replace("disable_uart1_clk", "disable_uart1_clock"))
    assert run(idf).returncode != 0

    clk.write_text("unsupported SDK source\n")
    assert run(idf).returncode != 0

    clk.write_text(BASE.replace("disable_uart0_clk", "disable_uart0_clock"))
    assert run(idf).returncode != 0

    clk.write_text(
        BASE.replace(
            "clk_gate_config.disable_uart0_clk = true;",
            "/* PROJECTZERO_C5_USB_UART0_CLOCK_REV1: stale patch */",
        )
    )
    assert run(idf).returncode != 0

print(
    "PASS: C5 USB clock patch transforms once, preserves UART1, accepts upstream fix, and rejects unknown SDK"
)
