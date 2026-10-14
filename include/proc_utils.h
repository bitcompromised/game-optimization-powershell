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
    int affinity_cores;   /* logical processors this process is allowed to run on, 0 if unknown */
    DWORD_PTR affinity_mask; /* the exact mask, 0 if it couldn't be read */
    int flagged;
} ProcInfo;

/* Takes a snapshot of running processes, samples CPU usage over `sample_ms`
 * (this call blocks for roughly that long), and fills `out` (capacity PROC_MAX).
 * Returns the number of processes captured. Results are sorted by cpu_percent desc. */
int proc_list(ProcInfo *out, DWORD sample_ms);

/* Same snapshot without blocking: CPU% is measured against the previous call instead of a
 * sleep, so the first call reports 0 for everything. Keeps one history, so it belongs to a
 * single caller (the process page's refresh timer). Results are unsorted. */
int proc_list_delta(ProcInfo *out);

/* Full path of a process's executable. Returns 0 on success. */
int proc_image_path(DWORD pid, char *out, size_t out_size);

/* Returns 1 if `name` (case-insensitive, e.g. "OneDrive.exe") is a known low-value
 * background/telemetry process that's commonly safe to close for a gaming session. */
int proc_is_bloat(const char *name);

/* Returns 1 if `name` (case-insensitive) is a core OS/session process (or security
 * software) that must never be force-closed, regardless of profile contents. */
int proc_is_protected(const char *name);

/* Terminates a process by PID. Returns 0 on success. */
int proc_kill(DWORD pid);

#endif
