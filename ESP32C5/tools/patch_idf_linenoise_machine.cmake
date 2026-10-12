# USB OTA uses the normal command parser but must not echo+fsync every byte:
# doing so stalls the reader long enough to overflow the 256-byte USB RX ring.
set(_ln_dir "$ENV{IDF_PATH}/components/console/linenoise")
file(READ "${_ln_dir}/linenoise.c" _src)
file(READ "${_ln_dir}/linenoise.h" _header)
if(NOT _src MATCHES "void linenoiseSetMachineMode")
  string(FIND "${_src}" "static int linenoiseDumb(" _start)
  string(FIND "${_src}" "static void sanitize(" _end)
  if(_start LESS 0 OR _end LESS _start)
    message(FATAL_ERROR "Unsupported IDF linenoise: cannot install USB OTA machine mode")
  endif()
  math(EXPR _length "${_end} - ${_start}")
  string(SUBSTRING "${_src}" ${_start} ${_length} _old)
  set(_new "${_old}")
  foreach(_echo IN ITEMS
      "    fputs(prompt, stdout);\n    flushWrite();"
      "                fputs(\"\\x08 \", stdout); /* Windows CMD: erase symbol under cursor */\n                flushWrite();"
      "        fputc(c, stdout); /* echo */\n        flushWrite();"
      "    fputc('\\n', stdout);\n    flushWrite();")
    string(FIND "${_new}" "${_echo}" _found)
    if(_found LESS 0)
      message(FATAL_ERROR "Unsupported IDF linenoise echo path; USB OTA patch refused")
    endif()
    string(REPLACE "${_echo}" "if (!machine_mode) {\n${_echo}\n}" _new "${_new}")
  endforeach()
  set(_mode [=[
static bool machine_mode;
static int machine_saved_dumb;
void linenoiseSetMachineMode(bool enabled) {
    if (enabled && !machine_mode) {
        machine_saved_dumb = dumbmode;
        dumbmode = 1;
    } else if (!enabled && machine_mode) {
        dumbmode = machine_saved_dumb;
    }
    machine_mode = enabled;
}

]=])
  string(REPLACE "${_old}" "${_mode}${_new}" _src "${_src}")
  file(WRITE "${_ln_dir}/linenoise.c" "${_src}")
endif()

# USB Serial/JTAG is a machine transport, not an ANSI terminal. The IDF
# linenoiseProbe() sends ESC[5n and consumes stdin while waiting for an ESC
# response. A host that sends its initial metadata commands as soon as the
# device enumerates can have those ASCII commands discarded by that probe.
# Skip only that probe on this console profile and force the already-supported
# dumb input path. Preserve the normal probe for UART and other console types.
if(NOT _src MATCHES "PROJECTZERO_USB_SERIAL_JTAG_SKIP_PROBE")
  string(FIND "${_src}" "int linenoiseProbe(void) {" _probe_start)
  string(FIND "${_src}" "static int linenoiseRaw(" _probe_end)
  if(_probe_start LESS 0 OR _probe_end LESS _probe_start)
    message(FATAL_ERROR "Unsupported IDF linenoise: cannot locate linenoiseProbe()")
  endif()
  math(EXPR _probe_length "${_probe_end} - ${_probe_start}")
  string(SUBSTRING "${_src}" ${_probe_start} ${_probe_length} _probe_old)
  set(_probe_new "${_probe_old}")
  string(REPLACE "int linenoiseProbe(void) {\n"
    "int linenoiseProbe(void) {\n#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG\n    /* PROJECTZERO_USB_SERIAL_JTAG_SKIP_PROBE: preserve early host commands. */\n    linenoiseSetDumbMode(1);\n    return -2;\n#else\n"
    _probe_new "${_probe_new}")
  if(_probe_new STREQUAL _probe_old)
    message(FATAL_ERROR "Unsupported IDF linenoiseProbe signature; refusing to alter USB input behavior")
  endif()
  string(REPLACE "    return 0;\n}\n\n"
    "    return 0;\n#endif\n}\n\n"
    _probe_new "${_probe_new}")
  string(FIND "${_probe_new}" [=[    return 0;
#endif
}]=] _probe_tail)
  if(_probe_tail LESS 0)
    message(FATAL_ERROR "Unsupported IDF linenoiseProbe ending; refusing to alter USB input behavior")
  endif()
  string(REPLACE "${_probe_old}" "${_probe_new}" _src "${_src}")
  if(NOT _src MATCHES "PROJECTZERO_USB_SERIAL_JTAG_SKIP_PROBE")
    message(FATAL_ERROR "Failed to install USB Serial/JTAG probe bypass")
  endif()
  file(WRITE "${_ln_dir}/linenoise.c" "${_src}")
endif()

if(NOT _header MATCHES "void linenoiseSetMachineMode")
  string(REPLACE "void linenoiseSetDumbMode(int set);"
    "void linenoiseSetDumbMode(int set);\nvoid linenoiseSetMachineMode(bool enabled);"
    _header "${_header}")
  if(NOT _header MATCHES "void linenoiseSetMachineMode")
    message(FATAL_ERROR "Unsupported IDF linenoise header")
  endif()
  file(WRITE "${_ln_dir}/linenoise.h" "${_header}")
endif()
