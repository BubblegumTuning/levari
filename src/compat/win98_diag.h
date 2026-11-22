#ifndef LV_WIN98_DIAG_H
#define LV_WIN98_DIAG_H
/* Era startup forensics (LV_WIN9X win98 pipeline only).
 *
 * Hardware iteration over a FAT32 transfer cannot carry a debugger,
 * and the GUI subsystem owns no stderr: without in-process logging a
 * crash before the first dialog is permanently silent (hardware
 * 2026-11-17 batch 9: "crashes instantly, no error"). This unit
 * installs, as the first thing the process does, an era-safe trace
 * log (ASCII, WriteFile per line) plus an unhandled-exception filter
 * that records exception code / EIP / image base so the RVA can be
 * resolved against the build-win98 disassembly with objdump. */
#if defined(LV_WIN9X) && defined(_WIN32)
void lv98_diag_init(void);
void lv98_tracef(const char *fmt, ...);
#else
#define lv98_diag_init() ((void)0)
#define lv98_tracef(...) ((void)0)
#endif
#endif
