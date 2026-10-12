"""Exercise the pinned SDK probe and patched input loop with machine traffic."""
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text()
start = source.index('static bool machine_mode;')
end = source.index('static void sanitize(', start)
implementation = source[start:end]
probe_start = source.index('int linenoiseProbe(void) {')
probe_end = source.index('static int linenoiseRaw(', probe_start)
probe = source[probe_start:probe_end]
preamble = r'''
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1
#define BACKSPACE 127
#define CTRL_H 8
#define UNIT_SEP 31
static int dumbmode, flushes;
static FILE *sink;
#undef stdout
#define stdout sink
static const char *input;
static int input_position;
static void linenoiseSetDumbMode(int set) { dumbmode = set; }
static ssize_t read_func(int fd, void *buf, size_t n) {
    (void)fd; assert(n == 1); assert(input[input_position]);
    *(char *)buf = input[input_position++]; return 1;
}
static void flushWrite(void) { ++flushes; }
'''
tests = r'''
int main(void) {
    sink = tmpfile(); assert(sink);
    char line[6000], output[8192] = {0};
    memset(line, 'a', 5580); line[5580] = '\n'; line[5581] = 0;
    input = line;
    linenoiseSetMachineMode(true);
    linenoiseSetMachineMode(true); /* reconnect must not replace saved mode */
    assert(dumbmode == 1);
    assert(linenoiseDumb(output, sizeof(output), ">") == 5580);
    assert(!memcmp(output, line, 5580));
    assert(flushes == 0 && ftell(sink) == 0);
    linenoiseSetMachineMode(false); assert(dumbmode == 0);
    input = "abc\bD\n"; input_position = 0;
    assert(linenoiseDumb(output, sizeof(output), ">") == 3);
    assert(!memcmp(output, "abD", 3) && flushes > 0 && ftell(sink) > 0);

    /* Metadata sent immediately after USB enumeration must survive REPL setup. */
    static const char metadata[] = "get_device_info\r\nget_capabilities\r\n";
    char command[128] = {0};
    input = metadata; input_position = 0; dumbmode = 0;
    long prior_output = ftell(sink);
    assert(linenoiseProbe() == -2);
    assert(dumbmode == 1 && input_position == 0 && ftell(sink) == prior_output);
    int n = linenoiseDumb(command, sizeof(command), ">");
    command[n] = 0;
    assert(!strcmp(command, "get_device_info"));
    memset(command, 0, sizeof(command));
    n = linenoiseDumb(command, sizeof(command), ">");
    command[n] = 0;
    assert(!strcmp(command, "get_capabilities"));

    fclose(sink); puts("PASS: USB probe preserves preloaded metadata; machine-mode OTA input has no per-byte flush; normal echo restored");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.c'; binary = Path(tmp) / 'test'
    src.write_text(preamble + probe + implementation + tests)
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
