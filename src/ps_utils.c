#include "ps_utils.h"
#include "task_utils.h"
#include "config.h"

static void make_temp_path(char *out, size_t out_size, const char *suffix) {
    char dir[MAX_PATH];
    config_get_dir(dir, sizeof(dir));
    static LONG counter = 0;
    LONG n = InterlockedIncrement(&counter);
    _snprintf(out, out_size, "%s\\tmp_%lu_%ld%s", dir,
              (unsigned long)GetCurrentProcessId(), (long)n, suffix);
    out[out_size - 1] = '\0';
}

static int write_script_file(const char *path, const char *script) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(script, f);
    fclose(f);
    return 0;
}

static int run_script_file(const char *script_path) {
    char cmdline[MAX_PATH + 128];
    _snprintf(cmdline, sizeof(cmdline),
              "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%s\"",
              script_path);
    cmdline[sizeof(cmdline) - 1] = '\0';
    return run_hidden(cmdline);
}

int ps_run(const char *script) {
    char script_path[MAX_PATH];
    make_temp_path(script_path, sizeof(script_path), ".ps1");

    if (write_script_file(script_path, script) != 0) return -1;

    int rc = run_script_file(script_path);
    DeleteFileA(script_path);
    return (rc == 0) ? 0 : -1;
}

static int ps_run_capturing(const char *script, char *out_path, size_t out_path_size) {
    char script_path[MAX_PATH];
    make_temp_path(script_path, sizeof(script_path), ".ps1");
    make_temp_path(out_path, out_path_size, ".out");

    char full_script[8192];
    _snprintf(full_script, sizeof(full_script),
              "%s\r\n$__result | Out-File -Encoding ascii -FilePath \"%s\"\r\n",
              script, out_path);
    full_script[sizeof(full_script) - 1] = '\0';

    int wrote = write_script_file(script_path, full_script);
    int rc = -1;
    if (wrote == 0) rc = run_script_file(script_path);
    DeleteFileA(script_path);
    return (wrote == 0 && rc == 0) ? 0 : -1;
}

int ps_capture_int(const char *script, long *out) {
    char out_path[MAX_PATH];
    int ran = ps_run_capturing(script, out_path, sizeof(out_path));

    int found = 0;
    FILE *f = ran == 0 ? fopen(out_path, "r") : NULL;
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            trim_newline(line);
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0') continue;
            char *end;
            long val = strtol(p, &end, 10);
            if (end != p) { *out = val; found = 1; break; }
        }
        fclose(f);
    }
    DeleteFileA(out_path);
    return found ? 0 : -1;
}

int ps_capture_text(const char *script, char *out, size_t out_size) {
    out[0] = '\0';
    char out_path[MAX_PATH];
    int ran = ps_run_capturing(script, out_path, sizeof(out_path));

    int found = 0;
    FILE *f = ran == 0 ? fopen(out_path, "r") : NULL;
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            trim_newline(line);
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0') continue;
            strncpy(out, p, out_size - 1);
            out[out_size - 1] = '\0';
            found = 1;
            break;
        }
        fclose(f);
    }
    DeleteFileA(out_path);
    return found ? 0 : -1;
}
