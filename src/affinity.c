#include "affinity.h"
#include "proc_utils.h"
#include <tlhelp32.h>

const char *const AFFINITY_VALORANT_EXES[] = {
    "VALORANT-Win64-Shipping.exe",   /* the game itself */
    "VALORANT.exe",                  /* the shim that launches it */
};
const int AFFINITY_VALORANT_EXE_COUNT =
    (int)(sizeof(AFFINITY_VALORANT_EXES) / sizeof(AFFINITY_VALORANT_EXES[0]));

/* Processes that keep the full core mask no matter what. proc_is_protected() already covers
 * the OS session and Defender; this adds the things a *game* specifically needs to stay
 * responsive -- the anti-cheat, the launcher that talks to it, the GPU/audio driver hosts and
 * the input stack. Squeezing any of these onto two cores hurts the thing we're optimizing. */
static const char *EXEMPT_LIST[] = {
    "vgc.exe",                    /* Vanguard service */
    "vgtray.exe",
    "riotclientservices.exe",
    "riotclientux.exe",
    "riotclientuxrender.exe",
    "riotclientcrashhandler.exe",
    "easyanticheat.exe",
    "easyanticheat_eos.exe",
    "beservice.exe",
    "bedaisy.exe",
    "nvcontainer.exe",            /* NVIDIA driver / control panel container */
    "nvdisplay.container.exe",
    "nvsphelper64.exe",
    "amdow.exe",                  /* AMD driver hosts */
    "radeonsoftware.exe",
    "atieclxx.exe",
    "igfxem.exe",                 /* Intel graphics helper */
    "rtkauduservice64.exe",       /* Realtek audio */
    "ravcpl64.exe",
    "voicemeeter.exe",
    "obs64.exe",                  /* capture -- dropping frames here is very visible */
};
#define EXEMPT_LIST_COUNT (sizeof(EXEMPT_LIST) / sizeof(EXEMPT_LIST[0]))

static int is_exempt(const char *name) {
    for (size_t i = 0; i < EXEMPT_LIST_COUNT; i++) {
        if (_stricmp(name, EXEMPT_LIST[i]) == 0) return 1;
    }
    return 0;
}

static int matches_game(const char *name, const char *const *game_exes, int count) {
    for (int i = 0; i < count; i++) {
        if (_stricmp(name, game_exes[i]) == 0) return 1;
    }
    return 0;
}

static int popcount_ptr(DWORD_PTR mask) {
    int n = 0;
    while (mask) {
        n += (int)(mask & 1);
        mask >>= 1;
    }
    return n;
}

/* The affinity mask of every logical processor this process's group can use. Reading it from
 * our own process (rather than assuming (1<<n)-1) keeps it correct if the app was itself
 * launched with a restricted mask or on a machine with a sparse mask. */
static DWORD_PTR system_mask(void) {
    DWORD_PTR proc_mask = 0, sys_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) || sys_mask == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (DWORD_PTR)si.dwActiveProcessorMask;
    }
    return sys_mask;
}

int affinity_cpu_count(void) {
    int n = popcount_ptr(system_mask());
    return n > 0 ? n : 1;
}

int affinity_background_core_count(void) {
    int total = affinity_cpu_count();
    int background = total / 8;          /* ~12% of the machine, scaled up on big CPUs */
    if (background < 2) background = 2;
    if (background > total - 2) background = total - 2;
    return background;
}

/* The lowest `count` set bits of the system mask. Core 0 and its SMT sibling already absorb
 * most driver DPC/ISR work, which makes them the least attractive cores for a game -- so
 * that's where background work gets parked. */
static DWORD_PTR background_mask(int count) {
    DWORD_PTR sys = system_mask();
    DWORD_PTR mask = 0;
    int taken = 0;
    for (int bit = 0; bit < (int)(sizeof(DWORD_PTR) * 8) && taken < count; bit++) {
        DWORD_PTR b = (DWORD_PTR)1 << bit;
        if (sys & b) { mask |= b; taken++; }
    }
    return mask;
}

DWORD_PTR affinity_focus_mask(void) {
    if (affinity_cpu_count() < 4) return 0;
    return background_mask(affinity_background_core_count());
}

int affinity_is_game(const char *name, const char *const *game_exes, int game_exe_count) {
    return matches_game(name, game_exes, game_exe_count);
}

int affinity_count_pinned(void) {
    DWORD_PTR focus = affinity_focus_mask();
    if (!focus) return 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    int pinned = 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4 || proc_is_protected(pe.szExeFile)) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            DWORD_PTR mask = 0, sys = 0;
            if (GetProcessAffinityMask(h, &mask, &sys) && mask == focus) pinned++;
            CloseHandle(h);
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pinned;
}

/* Sets the game's priority class. Vanguard (and most kernel anti-cheats) strip write access
 * from handles to the protected game process, so a failure here is expected rather than a
 * bug -- the caller reports it and points at the persistent launch-priority tweak instead. */
static int set_game_priority(DWORD pid, DWORD priority_class) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) return -1;
    BOOL ok = SetPriorityClass(h, priority_class);
    CloseHandle(h);
    return ok ? 1 : -1;
}

/* Shared walk for both focus and reset: applies `target_mask` to every ordinary process. With
 * `only_from` non-zero, only processes currently carrying exactly that mask are changed -- the
 * reset uses it so it undoes our pins without flattening affinity other apps chose themselves. */
static void apply_mask_to_background(DWORD_PTR target_mask, DWORD_PTR only_from,
                                     const char *const *game_exes, int game_exe_count,
                                     AffinityResult *out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    DWORD self = GetCurrentProcessId();

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (!Process32First(snap, &pe)) { CloseHandle(snap); return; }

    do {
        if (pe.th32ProcessID <= 4 || pe.th32ProcessID == self) continue;

        if (matches_game(pe.szExeFile, game_exes, game_exe_count)) {
            if (!out->game_found) {
                out->game_found = 1;
                out->game_pid = pe.th32ProcessID;
                strncpy(out->game_name, pe.szExeFile, sizeof(out->game_name) - 1);
                out->game_name[sizeof(out->game_name) - 1] = '\0';
            }
            continue;   /* never touch the game's own affinity */
        }

        if (proc_is_protected(pe.szExeFile) || is_exempt(pe.szExeFile)) {
            out->skipped++;
            continue;
        }

        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, pe.th32ProcessID);
        if (!h) { out->denied++; continue; }

        DWORD_PTR current = 0, sys = 0;
        int have_mask = GetProcessAffinityMask(h, &current, &sys) ? 1 : 0;
        if (have_mask && current == target_mask) {
            CloseHandle(h);
            continue;   /* already where we want it */
        }
        if (only_from && (!have_mask || current != only_from)) {
            CloseHandle(h);
            continue;   /* not one of ours */
        }

        if (SetProcessAffinityMask(h, target_mask)) out->adjusted++;
        else out->denied++;
        CloseHandle(h);
    } while (Process32Next(snap, &pe));

    CloseHandle(snap);
}

int affinity_focus_game(const char *const *game_exes, int game_exe_count, AffinityResult *out) {
    memset(out, 0, sizeof(*out));
    out->total_cores = affinity_cpu_count();

    /* Below four logical processors there is nothing meaningful to split: the background set
     * would be most of the machine. */
    if (out->total_cores < 4) return -1;

    out->background_cores = affinity_background_core_count();
    apply_mask_to_background(background_mask(out->background_cores), 0,
                             game_exes, game_exe_count, out);

    if (out->game_found) {
        out->game_priority = set_game_priority(out->game_pid, HIGH_PRIORITY_CLASS);
    }
    return 0;
}

int affinity_reset(const char *const *game_exes, int game_exe_count, AffinityResult *out) {
    memset(out, 0, sizeof(*out));
    out->total_cores = affinity_cpu_count();
    out->background_cores = out->total_cores;

    apply_mask_to_background(system_mask(), affinity_focus_mask(), game_exes, game_exe_count, out);

    if (out->game_found) {
        out->game_priority = set_game_priority(out->game_pid, NORMAL_PRIORITY_CLASS);
    }
    return 0;
}
