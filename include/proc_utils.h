#ifndef PROC_UTILS_H
#define PROC_UTILS_H

#include "common.h"

#define PROC_MAX 512
#define PROC_NAME_MAX 260

typedef struct {
    DWORD pid;
    char name[PROC_NAME_MAX];
    SIZE_T working_set_kb;
    double cpu_percent;
    int flagged;
} ProcInfo;

/* Takes a snapshot of running processes, samples CPU usage over `sample_ms`
 * (this call blocks for roughly that long), and fills `out` (capacity PROC_MAX).
 * Returns the number of processes captured. Results are sorted by cpu_percent desc. */
int proc_list(ProcInfo *out, DWORD sample_ms);

/* Returns 1 if `name` (case-insensitive, e.g. "OneDrive.exe") is a known low-value
 * background/telemetry process that's commonly safe to close for a gaming session. */
int proc_is_bloat(const char *name);

/* Returns 1 if `name` (case-insensitive) is a core OS/session process (or security
 * software) that must never be force-closed, regardless of profile contents. */
int proc_is_protected(const char *name);

/* Terminates a process by PID. Returns 0 on success. */
int proc_kill(DWORD pid);

#endif
