#include "task_utils.h"
#include "config.h"

int run_hidden(char *cmdline) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                         CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return -1;
    }

    WaitForSingleObject(pi.hProcess, 15000);
    DWORD exit_code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
}

int task_set_enabled(const char *task_path, int enabled) {
    char cmdline[1024];
    _snprintf(cmdline, sizeof(cmdline), "schtasks.exe /Change /TN \"%s\" /%s",
              task_path, enabled ? "Enable" : "Disable");
    cmdline[sizeof(cmdline) - 1] = '\0';

    int rc = run_hidden(cmdline);
    return (rc == 0) ? 0 : -1;
}

int task_is_enabled(const char *task_path) {
    char dir[MAX_PATH];
    config_get_dir(dir, sizeof(dir));

    char out_path[MAX_PATH];
    _snprintf(out_path, sizeof(out_path), "%s\\tmp_taskq_%lu.txt", dir, (unsigned long)GetCurrentProcessId());
    out_path[sizeof(out_path) - 1] = '\0';

    char cmdline[1024];
    _snprintf(cmdline, sizeof(cmdline),
              "cmd.exe /C schtasks.exe /Query /TN \"%s\" /V /FO LIST > \"%s\" 2>&1",
              task_path, out_path);
    cmdline[sizeof(cmdline) - 1] = '\0';

    run_hidden(cmdline);

    int result = -1;
    FILE *f = fopen(out_path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "Scheduled Task State")) {
                if (strstr(line, "Enabled")) result = 1;
                else if (strstr(line, "Disabled")) result = 0;
                break;
            }
        }
        fclose(f);
    }
    DeleteFileA(out_path);
    return result;
}
