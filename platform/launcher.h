// The machine launcher: a native window listing the saved machines (vmstore.h) with New, Edit,
// Duplicate, Delete and Start. Call platform_ui_init() first (it sets up the host's application
// object the same way the emulator window will).
#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <stdbool.h>
#include "vmstore.h"

// Shows the launcher until the user starts a machine (true, *out filled) or quits (false).
// dir: machine directory (NULL = default); select: name to select at first (NULL = first).
// BEEMU_LAUNCHER_SNAPSHOT=file.png renders the window to a PNG and returns false without
// waiting (BEEMU_LAUNCHER_SNAPSHOT_EDIT=file.png also renders the edit sheet of the selection).
// BEEMU_LAUNCHER_AUTOSTART=1 starts the selected machine after half a second (for tests).
bool platform_launcher(const char *dir, const char *select, vm_config_t *out);

#endif
