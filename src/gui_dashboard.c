#include "gui.h"
#include "sysinfo.h"
#include "ps_utils.h"
#include "task_utils.h"
#include <commctrl.h>
#include <stdarg.h>

#define ID_DASH_INFO        300
#define ID_DASH_SUMMARY     301
#define ID_DASH_DISABLE_ALL 302
#define ID_DASH_PERF_ALL    303
#define ID_DASH_REVERT_ALL  304
#define ID_DASH_RESCAN      305
#define ID_DASH_TRIM        306
#define ID_DASH_FLUSHDNS    307
#define ID_DASH_CLEANTEMP   308

#define BUTTON_HEIGHT 32
#define GAP 8

static HWND g_info;
static HWND g_summary;
static HWND g_disable_all, g_perf_all, g_revert_all, g_rescan;
static HWND g_trim, g_flushdns, g_cleantemp;
static SysInfo g_sys;
static int g_sys_loaded;

static HWND make_button(HWND parent, const char *text, int id) {
    HWND h = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    gui_apply_font(h);
    return h;
}

static void dash_create(HWND parent) {
    g_info = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                             WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                             0, 0, 0, 0, parent, (HMENU)ID_DASH_INFO, NULL, NULL);
    gui_apply_font(g_info);

    g_summary = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                0, 0, 0, 0, parent, (HMENU)ID_DASH_SUMMARY, NULL, NULL);
    gui_apply_font(g_summary);

    g_disable_all = make_button(parent, "Disable All Windows Tracking", ID_DASH_DISABLE_ALL);
    g_perf_all    = make_button(parent, "Apply All Performance Tweaks", ID_DASH_PERF_ALL);
    g_revert_all  = make_button(parent, "Revert Everything", ID_DASH_REVERT_ALL);
    g_rescan      = make_button(parent, "Re-scan System", ID_DASH_RESCAN);
    g_trim        = make_button(parent, "Optimize SSD (TRIM)", ID_DASH_TRIM);
    g_flushdns    = make_button(parent, "Flush DNS Cache", ID_DASH_FLUSHDNS);
    g_cleantemp   = make_button(parent, "Clean Temp Files", ID_DASH_CLEANTEMP);
}

static void dash_layout(int x, int y, int w, int h) {
    int button_rows = 2;
    int buttons_height = button_rows * BUTTON_HEIGHT + GAP;
    int panels_height = h - buttons_height - GAP * 2;
    if (panels_height < 100) panels_height = 100;

    int left_w = (w - GAP) / 2;
    int right_w = w - left_w - GAP;

    MoveWindow(g_info, x, y, left_w, panels_height, TRUE);
    MoveWindow(g_summary, x + left_w + GAP, y, right_w, panels_height, TRUE);

    int by = y + panels_height + GAP;
    int bw = (w - GAP * 3) / 4;

    MoveWindow(g_disable_all, x, by, bw, BUTTON_HEIGHT, TRUE);
    MoveWindow(g_perf_all, x + (bw + GAP), by, bw, BUTTON_HEIGHT, TRUE);
    MoveWindow(g_revert_all, x + (bw + GAP) * 2, by, bw, BUTTON_HEIGHT, TRUE);
    MoveWindow(g_rescan, x + (bw + GAP) * 3, by, bw, BUTTON_HEIGHT, TRUE);

    int by2 = by + BUTTON_HEIGHT + GAP;
    MoveWindow(g_trim, x, by2, bw, BUTTON_HEIGHT, TRUE);
    MoveWindow(g_flushdns, x + (bw + GAP), by2, bw, BUTTON_HEIGHT, TRUE);
    MoveWindow(g_cleantemp, x + (bw + GAP) * 2, by2, bw, BUTTON_HEIGHT, TRUE);
}

static void append(char *buf, size_t size, const char *fmt, ...) {
    size_t len = strlen(buf);
    if (len >= size - 1) return;
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf + len, size - len - 1, fmt, args);
    va_end(args);
    buf[size - 1] = '\0';
}

static void refresh_info_panel(void) {
    if (!g_sys_loaded) {
        gui_status("Collecting system information...");
        HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
        sysinfo_collect(&g_sys);
        SetCursor(old);
        g_sys_loaded = 1;
    } else {
        /* Cheap fields worth keeping live without re-running the PowerShell GPU query. */
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            g_sys.ram_available_mb = ms.ullAvailPhys / (1024 * 1024);
            g_sys.ram_load_percent = ms.dwMemoryLoad;
        }
        ULONGLONG total_minutes = GetTickCount64() / 60000ULL;
        g_sys.uptime_hours = total_minutes / 60;
        g_sys.uptime_minutes = total_minutes % 60;
    }

    char buf[4096];
    buf[0] = '\0';

    append(buf, sizeof(buf), "SYSTEM INFORMATION\r\n\r\n");
    append(buf, sizeof(buf), "OS:          %s\r\n", g_sys.os_name);
    append(buf, sizeof(buf), "Version:     %s (build %s)\r\n",
           g_sys.os_display_version[0] ? g_sys.os_display_version : "?", g_sys.os_build);
    append(buf, sizeof(buf), "\r\n");
    append(buf, sizeof(buf), "CPU:         %s\r\n", g_sys.cpu_name);
    append(buf, sizeof(buf), "Cores:       %lu physical / %lu logical\r\n",
           (unsigned long)g_sys.cpu_cores, (unsigned long)g_sys.cpu_threads);
    append(buf, sizeof(buf), "GPU:         %s\r\n", g_sys.gpu_name);
    append(buf, sizeof(buf), "\r\n");
    append(buf, sizeof(buf), "Memory:      %llu MB total / %llu MB free (%lu%% in use)\r\n",
           (unsigned long long)g_sys.ram_total_mb, (unsigned long long)g_sys.ram_available_mb,
           (unsigned long)g_sys.ram_load_percent);
    append(buf, sizeof(buf), "System disk: %llu GB total / %llu GB free\r\n",
           (unsigned long long)g_sys.disk_total_gb, (unsigned long long)g_sys.disk_free_gb);
    append(buf, sizeof(buf), "\r\n");
    append(buf, sizeof(buf), "Power plan:  %s\r\n", g_sys.power_plan);
    append(buf, sizeof(buf), "Uptime:      %lluh %llum\r\n",
           (unsigned long long)g_sys.uptime_hours, (unsigned long long)g_sys.uptime_minutes);
    append(buf, sizeof(buf), "Privileges:  %s\r\n",
           g_sys.elevated ? "Administrator" : "Standard user (admin tweaks unavailable)");

    SetWindowTextA(g_info, buf);
}

static void refresh_summary_panel(void) {
    int perf_on = 0, perf_total = 0, tele_on = 0, tele_total = 0;
    for (int i = 0; i < g_tweak_count; i++) {
        if (g_tweaks[i].category == CAT_PERFORMANCE) {
            perf_total++;
            if (g_tweaks[i].enabled) perf_on++;
        } else {
            tele_total++;
            if (g_tweaks[i].enabled) tele_on++;
        }
    }

    char buf[8192];
    buf[0] = '\0';

    append(buf, sizeof(buf), "LIVE OPTIMIZATION STATE\r\n");
    append(buf, sizeof(buf), "(scanned from the system, not from saved settings)\r\n\r\n");
    append(buf, sizeof(buf), "Performance tweaks active:  %d / %d\r\n", perf_on, perf_total);
    append(buf, sizeof(buf), "Privacy tweaks active:      %d / %d\r\n\r\n", tele_on, tele_total);

    append(buf, sizeof(buf), "--- PERFORMANCE ---\r\n");
    for (int i = 0; i < g_tweak_count; i++) {
        if (g_tweaks[i].category != CAT_PERFORMANCE) continue;
        append(buf, sizeof(buf), "[%s] %s\r\n", g_tweaks[i].enabled ? "ON " : "off", g_tweaks[i].name);
    }

    append(buf, sizeof(buf), "\r\n--- PRIVACY / TRACKING ---\r\n");
    for (int i = 0; i < g_tweak_count; i++) {
        if (g_tweaks[i].category != CAT_TELEMETRY) continue;
        append(buf, sizeof(buf), "[%s] %s\r\n", g_tweaks[i].enabled ? "ON " : "off", g_tweaks[i].name);
    }

    SetWindowTextA(g_summary, buf);
}

static void dash_show(void) {
    refresh_info_panel();
    refresh_summary_panel();

    ShowWindow(g_info, SW_SHOW);
    ShowWindow(g_summary, SW_SHOW);
    ShowWindow(g_disable_all, SW_SHOW);
    ShowWindow(g_perf_all, SW_SHOW);
    ShowWindow(g_revert_all, SW_SHOW);
    ShowWindow(g_rescan, SW_SHOW);
    ShowWindow(g_trim, SW_SHOW);
    ShowWindow(g_flushdns, SW_SHOW);
    ShowWindow(g_cleantemp, SW_SHOW);

    gui_status("Dashboard -- live system state");
}

static void dash_hide(void) {
    ShowWindow(g_info, SW_HIDE);
    ShowWindow(g_summary, SW_HIDE);
    ShowWindow(g_disable_all, SW_HIDE);
    ShowWindow(g_perf_all, SW_HIDE);
    ShowWindow(g_revert_all, SW_HIDE);
    ShowWindow(g_rescan, SW_HIDE);
    ShowWindow(g_trim, SW_HIDE);
    ShowWindow(g_flushdns, SW_HIDE);
    ShowWindow(g_cleantemp, SW_HIDE);
}

static void run_maintenance(const char *label, const char *script, const char *done_msg) {
    char prompt[512];
    _snprintf(prompt, sizeof(prompt), "Run \"%s\" now?", label);
    prompt[sizeof(prompt) - 1] = '\0';
    if (MessageBoxA(gui_main_window(), prompt, APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    gui_status(label);
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int rc = ps_run(script);
    SetCursor(old);

    MessageBoxA(gui_main_window(), rc == 0 ? done_msg : "The command did not complete successfully.",
                APP_NAME, MB_OK | (rc == 0 ? MB_ICONINFORMATION : MB_ICONWARNING));
    gui_status("Ready");
}

static int dash_command(WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    switch (LOWORD(wparam)) {
        case ID_DASH_DISABLE_ALL: {
            if (MessageBoxA(gui_main_window(),
                    "This turns off every Windows 11 tracking, telemetry and diagnostics option "
                    "this tool knows about.\n\nEverything is reversible from the Privacy tab or "
                    "the \"Revert Everything\" button.\n\nContinue?",
                    APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return 1;
            }
            int failed = gui_bulk_toggle(CAT_TELEMETRY, 1);
            gui_refresh_tweak_state();
            char msg[256];
            _snprintf(msg, sizeof(msg), "Tracking disabled.%s",
                      failed ? "\n\nSome settings could not be changed -- they most likely need "
                               "Administrator rights." : "");
            msg[sizeof(msg) - 1] = '\0';
            MessageBoxA(gui_main_window(), msg, APP_NAME, MB_OK | MB_ICONINFORMATION);
            return 1;
        }

        case ID_DASH_PERF_ALL: {
            if (MessageBoxA(gui_main_window(),
                    "Apply every performance tweak?\n\nA few of them (HAGS, dynamic tick, NIC "
                    "interrupt moderation) only take effect after a reboot.",
                    APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return 1;
            }
            int failed = gui_bulk_toggle(CAT_PERFORMANCE, 1);
            gui_refresh_tweak_state();
            char msg[256];
            _snprintf(msg, sizeof(msg), "Performance tweaks applied.%s",
                      failed ? "\n\nSome could not be applied -- they most likely need "
                               "Administrator rights." : "");
            msg[sizeof(msg) - 1] = '\0';
            MessageBoxA(gui_main_window(), msg, APP_NAME, MB_OK | MB_ICONINFORMATION);
            return 1;
        }

        case ID_DASH_REVERT_ALL: {
            if (MessageBoxA(gui_main_window(),
                    "Revert every performance and privacy tweak back to Windows defaults?",
                    APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return 1;
            }
            gui_bulk_toggle(CAT_PERFORMANCE, 0);
            gui_bulk_toggle(CAT_TELEMETRY, 0);
            gui_refresh_tweak_state();
            MessageBoxA(gui_main_window(), "All tweaks reverted.", APP_NAME, MB_OK | MB_ICONINFORMATION);
            return 1;
        }

        case ID_DASH_RESCAN:
            gui_status("Re-scanning system state...");
            g_sys_loaded = 0;
            gui_refresh_tweak_state();
            return 1;

        case ID_DASH_TRIM:
            run_maintenance("Optimizing drives (TRIM)...",
                            "Get-Volume | Where-Object { $_.DriveLetter -and $_.FileSystemType -eq 'NTFS' } | "
                            "ForEach-Object { Optimize-Volume -DriveLetter $_.DriveLetter -ReTrim -ErrorAction SilentlyContinue }\r\n",
                            "Drive optimization finished.");
            return 1;

        case ID_DASH_FLUSHDNS:
            run_maintenance("Flushing DNS cache...",
                            "ipconfig /flushdns | Out-Null\r\n",
                            "DNS resolver cache cleared.");
            return 1;

        case ID_DASH_CLEANTEMP:
            run_maintenance("Cleaning temp files...",
                            "Remove-Item -Path \"$env:TEMP\\*\" -Recurse -Force -ErrorAction SilentlyContinue\r\n"
                            "Remove-Item -Path \"$env:SystemRoot\\Temp\\*\" -Recurse -Force -ErrorAction SilentlyContinue\r\n"
                            "exit 0\r\n",
                            "Temp files cleaned (files in use were skipped).");
            return 1;
    }
    return 0;
}

GuiPage g_page_dashboard = {
    dash_create, dash_layout, dash_show, dash_hide, dash_command, NULL
};
