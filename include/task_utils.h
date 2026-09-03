#ifndef TASK_UTILS_H
#define TASK_UTILS_H

#include "common.h"

/* Enable or disable a Scheduled Task by full path (e.g. "\\Microsoft\\Windows\\Application Experience\\Microsoft Compatibility Appraiser")
 * via schtasks.exe, run hidden. Returns 0 if the command launched and exited with code 0. */
int task_set_enabled(const char *task_path, int enabled);

/* Run an arbitrary command line hidden and wait for it to exit. Returns the process exit code,
 * or -1 if it could not be launched. */
int run_hidden(char *cmdline);

/* Queries whether a scheduled task is currently Enabled via `schtasks /Query`. Returns 1 if
 * enabled, 0 if disabled, -1 if the task doesn't exist or state couldn't be determined. */
int task_is_enabled(const char *task_path);

#endif
