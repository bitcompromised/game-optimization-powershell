#include "profile.h"
#include "config.h"
#include "svc_monitor.h"
#include "svc_utils.h"

static void profile_get_dir(char *out, size_t out_size) {
    char base[MAX_PATH];
    config_get_dir(base, sizeof(base));
    _snprintf(out, out_size, "%s\\profiles", base);
    out[out_size - 1] = '\0';
    CreateDirectoryA(out, NULL);
}

int profile_name_is_valid(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len >= PROFILE_NAME_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

static void profile_get_path(const char *name, char *out, size_t out_size) {
    char dir[MAX_PATH];
    profile_get_dir(dir, sizeof(dir));
    _snprintf(out, out_size, "%s\\%s.profile", dir, name);
    out[out_size - 1] = '\0';
}

static int cmp_str(const void *a, const void *b) {
    return _stricmp((const char *)a, (const char *)b);
}

int profile_list(char names[][PROFILE_NAME_MAX], int max_count) {
    char dir[MAX_PATH];
    profile_get_dir(dir, sizeof(dir));

    char search[MAX_PATH];
    _snprintf(search, sizeof(search), "%s\\*.profile", dir);
    search[sizeof(search) - 1] = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    int count = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (count >= max_count) break;

            char name[MAX_PATH];
            strncpy(name, fd.cFileName, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            char *dot = strrchr(name, '.');
            if (dot) *dot = '\0';

            strncpy(names[count], name, PROFILE_NAME_MAX - 1);
            names[count][PROFILE_NAME_MAX - 1] = '\0';
            count++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    if (count > 1) qsort(names, count, PROFILE_NAME_MAX, cmp_str);
    return count;
}

int profile_save(const char *name) {
    if (!profile_name_is_valid(name)) return -1;

    char path[MAX_PATH];
    profile_get_path(name, path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# GameOptimizer profile snapshot\n");

    static ProcInfo procs[PROC_MAX];
    int count = proc_list(procs, 50);

    static char seen[PROC_MAX][PROC_NAME_MAX];
    int seen_count = 0;

    for (int i = 0; i < count; i++) {
        int dup = 0;
        for (int j = 0; j < seen_count; j++) {
            if (_stricmp(seen[j], procs[i].name) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        strncpy(seen[seen_count], procs[i].name, PROC_NAME_MAX - 1);
        seen[seen_count][PROC_NAME_MAX - 1] = '\0';
        seen_count++;
        fprintf(f, "process=%s\n", procs[i].name);
    }

    for (int i = 0; i < KNOWN_SERVICE_COUNT; i++) {
        DWORD state;
        if (svc_query_state(KNOWN_SERVICES[i].service_name, &state) == 0 && state == SERVICE_RUNNING) {
            fprintf(f, "service=%s\n", KNOWN_SERVICES[i].service_name);
        }
    }

    fclose(f);
    return 0;
}

int profile_delete(const char *name) {
    if (!profile_name_is_valid(name)) return -1;
    char path[MAX_PATH];
    profile_get_path(name, path, sizeof(path));
    return (DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND) ? 0 : -1;
}

int profile_compute_diff(const char *name, ProfileDiff *diff) {
    if (!profile_name_is_valid(name)) return -1;

    char path[MAX_PATH];
    profile_get_path(name, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    static char saved_procs[PROC_MAX][PROC_NAME_MAX];
    int saved_proc_count = 0;
    static char saved_services[PROFILE_MAX_SERVICES][64];
    int saved_service_count = 0;

    char line[300];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "process") == 0 && saved_proc_count < PROC_MAX) {
            strncpy(saved_procs[saved_proc_count], val, PROC_NAME_MAX - 1);
            saved_procs[saved_proc_count][PROC_NAME_MAX - 1] = '\0';
            saved_proc_count++;
        } else if (strcmp(key, "service") == 0 && saved_service_count < PROFILE_MAX_SERVICES) {
            strncpy(saved_services[saved_service_count], val, 63);
            saved_services[saved_service_count][63] = '\0';
            saved_service_count++;
        }
    }
    fclose(f);

    memset(diff, 0, sizeof(*diff));

    static ProcInfo procs[PROC_MAX];
    int count = proc_list(procs, 50);
    DWORD self_pid = GetCurrentProcessId();

    for (int i = 0; i < count; i++) {
        if (procs[i].pid == self_pid) continue;

        int in_profile = 0;
        for (int j = 0; j < saved_proc_count; j++) {
            if (_stricmp(saved_procs[j], procs[i].name) == 0) { in_profile = 1; break; }
        }
        if (in_profile) continue;

        if (proc_is_protected(procs[i].name)) {
            diff->procs_protected_count++;
            continue;
        }

        if (diff->procs_to_close_count < PROC_MAX) {
            diff->procs_to_close[diff->procs_to_close_count] = procs[i];
            diff->procs_to_close_count++;
        }
    }

    for (int i = 0; i < KNOWN_SERVICE_COUNT; i++) {
        DWORD state;
        if (svc_query_state(KNOWN_SERVICES[i].service_name, &state) != 0 || state != SERVICE_RUNNING) continue;

        int in_profile = 0;
        for (int j = 0; j < saved_service_count; j++) {
            if (_stricmp(saved_services[j], KNOWN_SERVICES[i].service_name) == 0) { in_profile = 1; break; }
        }
        if (in_profile) continue;

        if (diff->services_to_stop_count < PROFILE_MAX_SERVICES) {
            strncpy(diff->services_to_stop[diff->services_to_stop_count], KNOWN_SERVICES[i].service_name, 63);
            diff->services_to_stop[diff->services_to_stop_count][63] = '\0';
            diff->services_to_stop_count++;
        }
    }

    return 0;
}

int profile_execute_diff(const ProfileDiff *diff, ProfileApplyResult *result) {
    memset(result, 0, sizeof(*result));

    for (int i = 0; i < diff->procs_to_close_count; i++) {
        if (proc_kill(diff->procs_to_close[i].pid) == 0) {
            result->processes_closed++;
        } else {
            result->processes_failed++;
        }
    }

    for (int i = 0; i < diff->services_to_stop_count; i++) {
        svc_stop(diff->services_to_stop[i]);
        int rc = svc_set_start_type(diff->services_to_stop[i], SERVICE_DISABLED);
        if (rc == 0) result->services_stopped++;
        else result->services_failed++;
    }

    return (result->processes_failed == 0 && result->services_failed == 0) ? 0 : -1;
}
