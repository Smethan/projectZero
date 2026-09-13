"""Exercise the actual patched SDK input loop with a full USB OTA-sized line."""
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text()
start = source.index('static bool machine_mode;')
end = source.index('static void sanitize(', start)
implementation = source[start:end]
preamble = r'''
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#define BACKSPACE 127
#define CTRL_H 8
#define UNIT_SEP 31
static int dumbmode, flushes;
static FILE *sink;
#undef stdout
#define stdout sink
static const char *input;
static int input_position;
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
    fclose(sink); puts("PASS: actual SDK USB OTA input has no per-byte flush; normal echo restored");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.c'; binary = Path(tmp) / 'test'
    src.write_text(preamble + implementation + tests)
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
