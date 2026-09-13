#pragma once
#include <stdbool.h>
/* Commands run serially on the existing console task. */
void uota_register(bool (*prepare)(void), void (*restart)(void));
bool uota_busy(void);
