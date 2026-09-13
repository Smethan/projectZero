#pragma once
#include <stdbool.h>
static bool machine_mode;
static void linenoiseSetMachineMode(bool enabled) { machine_mode=enabled; }
