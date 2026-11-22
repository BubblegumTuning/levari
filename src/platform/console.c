/* Console re-attach for the GUI-subsystem exes (refactor item 5).
   Compiled on Windows targets only; see platform.h for the contract. */
#include "platform.h"

#ifdef _WIN32
#include <stdio.h>

void lv_console_reattach(void) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}
#endif
