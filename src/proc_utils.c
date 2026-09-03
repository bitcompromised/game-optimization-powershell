#include "proc_utils.h"
#include <tlhelp32.h>
#include <psapi.h>

static const char *BLOAT_LIST[] = {
    "onedrive.exe",
    "teams.exe",
    "skype.exe",
    "yourphone.exe",
    "phoneexperiencehost.exe",
    "cortana.exe",
    "widgets.exe",
    "widgetservice.exe",
    "gamebarpresencewriter.exe",
    "gamebar.exe",
    "compattelrunner.exe",
    "msteams.exe",
    "spotify.exe",
    "skypeapp.exe",
    "bingwallpaperapp.exe",
    "msedgewebview2.exe",
};
#define BLOAT_LIST_COUNT (sizeof(BLOAT_LIST) / sizeof(BLOAT_LIST[0]))

int proc_is_bloat(const char *name) {
    for (size_t i = 0; i < BLOAT_LIST_COUNT; i++) {
        if (_stricmp(name, BLOAT_LIST[i]) == 0) return 1;
    }
    return 0;
}

/* Core OS/session infrastructure and security software -- never force-closed by a
 * profile switch, even if it wasn't running when the profile was saved. */
static const char *PROTECTED_LIST[] = {
    "system",
    "system idle process",
    "registry",
    "memory compression",
    "smss.exe",
    "csrss.exe",
    "wininit.exe",
    "winlogon.exe",
    "services.exe",
    "lsass.exe",
    "lsaiso.exe",
    "svchost.exe",
    "dwm.exe",
    "explorer.exe",
    "fontdrvhost.exe",
    "sihost.exe",
    "taskhostw.exe",
    "conhost.exe",
    "dllhost.exe",
    "runtimebroker.exe",
    "audiodg.exe",
    "spoolsv.exe",
    "wudfhost.exe",
    "userinit.exe",
    "logonui.exe",
    "ctfmon.exe",
    "shellexperiencehost.exe",
    "startmenuexperiencehost.exe",
    "searchhost.exe",
    "searchapp.exe",
    "searchindexer.exe",
    "msmpeng.exe",
    "nissrv.exe",
    "securityhealthservice.exe",
    "securityhealthsystray.exe",
    "smartscreen.exe",
};
#define PROTECTED_LIST_COUNT (sizeof(PROTECTED_LIST) / sizeof(PROTECTED_LIST[0]))

int proc_is_protected(const char *name) {
    for (size_t i = 0; i < PROTECTED_LIST_COUNT; i++) {
        if (_stricmp(name, PROTECTED_LIST[i]) == 0) return 1;
    }
    return 0;
}

int proc_kill(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return -1;
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok ? 0 : -1;
}

static int snapshot_pids(DWORD *pids, char names[][PROC_NAME_MAX], int max_count) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    int count = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (count >= max_count) break;
            if (pe.th32ProcessID == 0) continue; /* Idle */
            pids[count] = pe.th32ProcessID;
            strncpy(names[count], pe.szExeFile, PROC_NAME_MAX - 1);
            names[count][PROC_NAME_MAX - 1] = '\0';
            count++;
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}

static int get_process_time_100ns(HANDLE h, ULONGLONG *out) {
    FILETIME creation, exit_t, kernel, user;
    if (!GetProcessTimes(h, &creation, &exit_t, &kernel, &user)) return -1;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;  k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;    u.HighPart = user.dwHighDateTime;
    *out = k.QuadPart + u.QuadPart;
    return 0;
}

static int cmp_proc_cpu_desc(const void *a, const void *b) {
    const ProcInfo *pa = (const ProcInfo *)a;
    const ProcInfo *pb = (const ProcInfo *)b;
    if (pa->cpu_percent < pb->cpu_percent) return 1;
    if (pa->cpu_percent > pb->cpu_percent) return -1;
    return 0;
}

int proc_list(ProcInfo *out, DWORD sample_ms) {
    static DWORD pids[PROC_MAX];
    static char names[PROC_MAX][PROC_NAME_MAX];
    static HANDLE handles[PROC_MAX];
    static ULONGLONG t0[PROC_MAX];
    static ULONGLONG t1[PROC_MAX];
    static int has_times[PROC_MAX];

    int count = snapshot_pids(pids, names, PROC_MAX);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD num_cpus = si.dwNumberOfProcessors;
    if (num_cpus == 0) num_cpus = 1;

    for (int i = 0; i < count; i++) {
        handles[i] = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        has_times[i] = 0;
        if (handles[i]) {
            if (get_process_time_100ns(handles[i], &t0[i]) == 0) has_times[i] = 1;
        }
    }

    LARGE_INTEGER freq, start_qpc, end_qpc;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start_qpc);

    Sleep(sample_ms);

    QueryPerformanceCounter(&end_qpc);
    double elapsed_sec = (double)(end_qpc.QuadPart - start_qpc.QuadPart) / (double)freq.QuadPart;
    double elapsed_100ns = elapsed_sec * 10000000.0;

    for (int i = 0; i < count; i++) {
        double cpu_pct = 0.0;
        SIZE_T ws_kb = 0;

        if (handles[i]) {
            if (has_times[i] && get_process_time_100ns(handles[i], &t1[i]) == 0 && elapsed_100ns > 0) {
                double delta = (double)(t1[i] - t0[i]);
                cpu_pct = (delta / (elapsed_100ns * num_cpus)) * 100.0;
                if (cpu_pct < 0) cpu_pct = 0;
                if (cpu_pct > 100.0 * num_cpus) cpu_pct = 100.0 * num_cpus;
            }

            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(handles[i], &pmc, sizeof(pmc))) {
                ws_kb = pmc.WorkingSetSize / 1024;
            }
            CloseHandle(handles[i]);
        }

        out[i].pid = pids[i];
        strncpy(out[i].name, names[i], PROC_NAME_MAX - 1);
        out[i].name[PROC_NAME_MAX - 1] = '\0';
        out[i].working_set_kb = ws_kb;
        out[i].cpu_percent = cpu_pct;
        out[i].flagged = proc_is_bloat(names[i]);
    }

    qsort(out, count, sizeof(ProcInfo), cmp_proc_cpu_desc);
    return count;
}
