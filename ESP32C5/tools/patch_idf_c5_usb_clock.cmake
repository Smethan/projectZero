# ESP32-C5 revision 1.0 needs the UART0 clock to accept USB Serial/JTAG CDC host writes.
# ESP-IDF v6.0.1 disables it whenever UART0 is not the configured console,
# which can leave USB CDC enumerated while host writes stall. This file is included
# only by the ESP32-C5 project and patches only the pinned C5 clock source.
set(_clk "$ENV{IDF_PATH}/components/esp_system/port/soc/esp32c5/clk.c")
if(NOT EXISTS "${_clk}")
  message(FATAL_ERROR "C5 USB clock safeguard: expected ESP-IDF source is missing: ${_clk}")
endif()

file(READ "${_clk}" _src)
set(_marker "PROJECTZERO_C5_USB_UART0_CLOCK_REV1")
set(_assignment "clk_gate_config.disable_uart0_clk = true;")
set(_patched_block
    "#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG\n    /* ${_marker}: C5 rev 1.0 USB Serial/JTAG uses the UART0 clock. */\n    if (efuse_hal_chip_revision() > 100) {\n        ${_assignment}\n    }\n#else\n    ${_assignment}\n#endif")

string(REGEX MATCHALL "${_marker}" _markers "${_src}")
list(LENGTH _markers _marker_count)
if(_marker_count GREATER 0)
  # Repeated CMake configure must leave the source unchanged and verify the
  # complete intended conditional, rather than trusting a marker by itself.
  string(FIND "${_src}" "${_patched_block}" _patched_at)
  string(FIND "${_src}" "#if CONFIG_ESP_CONSOLE_UART_NUM != 1\n    clk_gate_config.disable_uart1_clk = true;\n#endif" _uart1_hook)
  if(NOT _marker_count EQUAL 1 OR _patched_at LESS 0 OR _uart1_hook LESS 0)
    message(FATAL_ERROR "C5 USB clock safeguard: marked clk.c has an unexpected or incomplete patch")
  endif()
  return()
endif()

if(NOT _src MATCHES "#include \"hal/efuse_hal.h\"")
  message(FATAL_ERROR "C5 USB clock safeguard: clk.c lacks the efuse revision API include")
endif()

# A newer ESP-IDF may already carry the same revision guard. Accept only the
# full expected conditional block inside the exact UART0 hook, not a coincident
# efuse check elsewhere in clk.c.
set(_upstream_block
    "#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG\n    if (efuse_hal_chip_revision() > 100) {\n        ${_assignment}\n    }\n#else\n    ${_assignment}\n#endif")
set(_upstream_hook "#if CONFIG_ESP_CONSOLE_UART_NUM != 0\n${_upstream_block}\n#endif")
string(FIND "${_src}" "${_upstream_hook}" _upstream_at)
if(NOT _upstream_at LESS 0)
  string(FIND "${_src}" "#if CONFIG_ESP_CONSOLE_UART_NUM != 1\n    clk_gate_config.disable_uart1_clk = true;\n#endif" _uart1_hook)
  if(_uart1_hook LESS 0)
    message(FATAL_ERROR "C5 USB clock safeguard: upstream fix is present but the UART1 hook is unexpected")
  endif()
  message(STATUS "C5 USB clock safeguard: upstream revision-aware USB clock fix already present")
  return()
endif()

# Require the exact v6.0.1 hook and its expected single UART0 clock disable.
# The adjacent UART1 block is deliberately untouched.
set(_hook "#if CONFIG_ESP_CONSOLE_UART_NUM != 0\n    ${_assignment}\n#endif")
string(FIND "${_src}" "${_hook}" _hook_at)
string(REPLACE "${_hook}" "__PROJECTZERO_C5_UART0_HOOK__" _hook_probe "${_src}")
string(FIND "${_hook_probe}" "${_hook}" _duplicate_hook_at)
if(_hook_at LESS 0 OR NOT _duplicate_hook_at LESS 0)
  message(FATAL_ERROR "C5 USB clock safeguard: unsupported ESP-IDF clk.c; expected the v6.0.1 UART0/UART1 clock hooks")
endif()
string(REPLACE "${_hook}" "#if CONFIG_ESP_CONSOLE_UART_NUM != 0\n${_patched_block}\n#endif" _patched_src "${_src}")
if(_patched_src STREQUAL _src)
  message(FATAL_ERROR "C5 USB clock safeguard: exact UART0 clock hook was not transformed")
endif()

# Verify there is exactly one marker, the complete conditional, and the same
# UART1 gate as before writing the SDK checkout.
string(REGEX MATCHALL "${_marker}" _patched_markers "${_patched_src}")
list(LENGTH _patched_markers _patched_marker_count)
string(FIND "${_patched_src}" "${_patched_block}" _verified_block)
string(FIND "${_patched_src}" "#if CONFIG_ESP_CONSOLE_UART_NUM != 1\n    clk_gate_config.disable_uart1_clk = true;\n#endif" _uart1_hook)
if(NOT _patched_marker_count EQUAL 1 OR _verified_block LESS 0 OR _uart1_hook LESS 0)
  message(FATAL_ERROR "C5 USB clock safeguard: transformed clk.c failed verification")
endif()

file(WRITE "${_clk}" "${_patched_src}")
message(STATUS "C5 USB clock safeguard: added rev 1.0 USB Serial/JTAG UART0 clock guard")
