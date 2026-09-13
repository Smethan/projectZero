#pragma once
#include <stdbool.h>
/* Best-effort progress only; capture itself does not depend on telemetry. */
bool hsm_start(bool serial_storage);
void hsm_stop(void);
