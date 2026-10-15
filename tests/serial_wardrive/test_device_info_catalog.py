"""Keep the device-info size fixture aligned with production registrations."""

from pathlib import Path
import re


HERE = Path(__file__).resolve().parent
MAIN = HERE.parents[1] / "ESP32C5" / "main"
fixture = (HERE / "test_device_info.c").read_text()
match = re.search(r"shipped_operations\[\]\s*=\s*\{([^}]+)\}", fixture, re.S)
assert match, "device_info fixture operation list missing"
expected = re.findall(r'"([a-z0-9_]+)"', match.group(1))
assert expected, "device_info fixture operation list empty"

registered = []
for path in sorted(MAIN.glob("*.c")):
    source = path.read_text()
    registered.extend(
        re.findall(r'\bsw_register_job_operation(?:_ex)?\(\s*"([a-z0-9_]+)"', source)
    )

assert registered == expected, (
    f"production operations {registered!r} differ from device_info fixture "
    f"{expected!r}; update the fixture and verify the 1000-byte line bound"
)
print("PASS: device_info size fixture matches production job registrations")
