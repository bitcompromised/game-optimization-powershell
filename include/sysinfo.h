#ifndef SYSINFO_H
#define SYSINFO_H

#include "common.h"

typedef struct {
    char cpu_name[128];
    DWORD cpu_cores;            /* physical cores */
    DWORD cpu_threads;          /* logical processors */
    char gpu_name[256];

    DWORDLONG ram_total_mb;
    DWORDLONG ram_available_mb;
    DWORD ram_load_percent;

    char os_name[128];
    char os_display_version[32]; /* e.g. "24H2" */
    char os_build[32];

    ULONGLONG disk_total_gb;
    ULONGLONG disk_free_gb;

    ULONGLONG uptime_hours;
    ULONGLONG uptime_minutes;   /* remainder within the hour */

    char power_plan[128];
    int elevated;
} SysInfo;

/* Fills `out` with a snapshot of current system information. The GPU name is looked up via
 * PowerShell (a few hundred ms); everything else is a native API/registry read. */
void sysinfo_collect(SysInfo *out);

#endif
