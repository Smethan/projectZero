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
if(NOT _header MATCHES "void linenoiseSetMachineMode")
  string(REPLACE "void linenoiseSetDumbMode(int set);"
    "void linenoiseSetDumbMode(int set);\nvoid linenoiseSetMachineMode(bool enabled);"
    _header "${_header}")
  if(NOT _header MATCHES "void linenoiseSetMachineMode")
    message(FATAL_ERROR "Unsupported IDF linenoise header")
  endif()
  file(WRITE "${_ln_dir}/linenoise.h" "${_header}")
endif()
