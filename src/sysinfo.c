#include "sysinfo.h"
#include "reg_utils.h"
#include "ps_utils.h"
#include <powrprof.h>

static void collect_cpu(SysInfo *out) {
    if (reg_get_string(HKEY_LOCAL_MACHINE,
                       "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                       "ProcessorNameString", out->cpu_name, sizeof(out->cpu_name)) != 0) {
        strcpy(out->cpu_name, "Unknown CPU");
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    out->cpu_threads = si.dwNumberOfProcessors;
    out->cpu_cores = si.dwNumberOfProcessors;

    DWORD len = 0;
    GetLogicalProcessorInformation(NULL, &len);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION *buf =
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(len);
        if (buf && GetLogicalProcessorInformation(buf, &len)) {
            DWORD count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
            DWORD cores = 0;
            for (DWORD i = 0; i < count; i++) {
                if (buf[i].Relationship == RelationProcessorCore) cores++;
            }
            if (cores > 0) out->cpu_cores = cores;
        }
        free(buf);
    }
}

static void collect_memory(SysInfo *out) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        out->ram_total_mb = ms.ullTotalPhys / (1024 * 1024);
        out->ram_available_mb = ms.ullAvailPhys / (1024 * 1024);
        out->ram_load_percent = ms.dwMemoryLoad;
    }
}

static void collect_os(SysInfo *out) {
    const char *key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    if (reg_get_string(HKEY_LOCAL_MACHINE, key, "ProductName", out->os_name, sizeof(out->os_name)) != 0) {
        strcpy(out->os_name, "Windows");
    }
    if (reg_get_string(HKEY_LOCAL_MACHINE, key, "DisplayVersion",
                       out->os_display_version, sizeof(out->os_display_version)) != 0) {
        out->os_display_version[0] = '\0';
    }
    if (reg_get_string(HKEY_LOCAL_MACHINE, key, "CurrentBuildNumber",
                       out->os_build, sizeof(out->os_build)) != 0) {
        out->os_build[0] = '\0';
    }

    /* ProductName still reports "Windows 10" on Windows 11; build >= 22000 is the real test. */
    long build = atol(out->os_build);
    if (build >= 22000 && strstr(out->os_name, "Windows 10")) {
        char fixed[128];
        _snprintf(fixed, sizeof(fixed), "Windows 11%s", out->os_name + strlen("Windows 10"));
        fixed[sizeof(fixed) - 1] = '\0';
        strncpy(out->os_name, fixed, sizeof(out->os_name) - 1);
        out->os_name[sizeof(out->os_name) - 1] = '\0';
    }
}

static void collect_disk(SysInfo *out) {
    char win_dir[MAX_PATH];
    if (GetWindowsDirectoryA(win_dir, sizeof(win_dir)) == 0) return;

    char root[4] = { win_dir[0], ':', '\\', '\0' };
    ULARGE_INTEGER free_avail, total, total_free;
    if (GetDiskFreeSpaceExA(root, &free_avail, &total, &total_free)) {
        out->disk_total_gb = total.QuadPart / (1024ULL * 1024 * 1024);
        out->disk_free_gb = total_free.QuadPart / (1024ULL * 1024 * 1024);
    }
}

static void collect_uptime(SysInfo *out) {
    ULONGLONG ms = GetTickCount64();
    ULONGLONG total_minutes = ms / 60000ULL;
    out->uptime_hours = total_minutes / 60;
    out->uptime_minutes = total_minutes % 60;
}

static void collect_power_plan(SysInfo *out) {
    strcpy(out->power_plan, "Unknown");

    GUID *active = NULL;
    if (PowerGetActiveScheme(NULL, &active) != ERROR_SUCCESS || !active) return;

    DWORD size = 0;
    if (PowerReadFriendlyName(NULL, active, NULL, NULL, NULL, &size) == ERROR_SUCCESS && size > 0) {
        UCHAR *buf = (UCHAR *)malloc(size);
        if (buf && PowerReadFriendlyName(NULL, active, NULL, NULL, buf, &size) == ERROR_SUCCESS) {
            WideCharToMultiByte(CP_ACP, 0, (WCHAR *)buf, -1,
                                out->power_plan, sizeof(out->power_plan) - 1, NULL, NULL);
            out->power_plan[sizeof(out->power_plan) - 1] = '\0';
        }
        free(buf);
    }
    LocalFree(active);
}

static void collect_gpu(SysInfo *out) {
    const char *script =
        "$__result = (Get-CimInstance Win32_VideoController | "
        "Where-Object { $_.Name } | Select-Object -First 1 -ExpandProperty Name)\r\n";
    if (ps_capture_text(script, out->gpu_name, sizeof(out->gpu_name)) != 0 || out->gpu_name[0] == '\0') {
        strcpy(out->gpu_name, "Unknown GPU");
    }
}

void sysinfo_collect(SysInfo *out) {
    memset(out, 0, sizeof(*out));
    collect_cpu(out);
    collect_memory(out);
    collect_os(out);
    collect_disk(out);
    collect_uptime(out);
    collect_power_plan(out);
    collect_gpu(out);
    out->elevated = is_running_elevated();
}
