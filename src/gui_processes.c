#include "gui.h"
#include "proc_utils.h"
#include <commctrl.h>

#define ID_PROC_LIST      500
#define ID_PROC_KILL      501
#define ID_PROC_KILL_ALL  502
#define ID_PROC_REFRESH   503
#define ID_PROC_TIMER     1

#define BUTTON_HEIGHT 30
#define GAP 8
#define AUTO_REFRESH_MS 3000
#define CPU_SAMPLE_MS 300

static HWND g_list, g_kill_btn, g_kill_all_btn, g_refresh_btn;
static ProcInfo g_procs[PROC_MAX];
static int g_proc_count;
static int g_visible;

static void proc_create(HWND parent) {
    g_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                             WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             0, 0, 0, 0, parent, (HMENU)ID_PROC_LIST, NULL, NULL);
    gui_apply_font(g_list);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);

    LVCOLUMNA col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = (char *)"PID";     col.cx = 70;  col.iSubItem = 0;
    SendMessageA(g_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    col.pszText = (char *)"Process"; col.cx = 240; col.iSubItem = 1;
    SendMessageA(g_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
    col.pszText = (char *)"CPU %";   col.cx = 80;  col.iSubItem = 2;
    SendMessageA(g_list, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);
    col.pszText = (char *)"Memory";  col.cx = 100; col.iSubItem = 3;
    SendMessageA(g_list, LVM_INSERTCOLUMNA, 3, (LPARAM)&col);
    col.pszText = (char *)"Notes";   col.cx = 260; col.iSubItem = 4;
    SendMessageA(g_list, LVM_INSERTCOLUMNA, 4, (LPARAM)&col);

    g_kill_btn = CreateWindowExA(0, "BUTTON", "End Selected Process",
                                 WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                 0, 0, 0, 0, parent, (HMENU)ID_PROC_KILL, NULL, NULL);
    g_kill_all_btn = CreateWindowExA(0, "BUTTON", "Close All Flagged Background Apps",
                                     WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                     0, 0, 0, 0, parent, (HMENU)ID_PROC_KILL_ALL, NULL, NULL);
    g_refresh_btn = CreateWindowExA(0, "BUTTON", "Refresh",
                                    WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                    0, 0, 0, 0, parent, (HMENU)ID_PROC_REFRESH, NULL, NULL);
    gui_apply_font(g_kill_btn);
    gui_apply_font(g_kill_all_btn);
    gui_apply_font(g_refresh_btn);
}

static void proc_layout(int x, int y, int w, int h) {
    int list_height = h - BUTTON_HEIGHT - GAP;
    if (list_height < 0) list_height = 0;
    MoveWindow(g_list, x, y, w, list_height, TRUE);

    int by = y + list_height + GAP;
    MoveWindow(g_kill_btn, x, by, 170, BUTTON_HEIGHT, TRUE);
    MoveWindow(g_kill_all_btn, x + 178, by, 250, BUTTON_HEIGHT, TRUE);
    MoveWindow(g_refresh_btn, x + 436, by, 110, BUTTON_HEIGHT, TRUE);
}

static void proc_populate(void) {
    int selected_pid = -1;
    int selected_row = (int)SendMessage(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (selected_row >= 0 && selected_row < g_proc_count) {
        selected_pid = (int)g_procs[selected_row].pid;
    }

    g_proc_count = proc_list(g_procs, CPU_SAMPLE_MS);

    SendMessage(g_list, WM_SETREDRAW, FALSE, 0);
    SendMessage(g_list, LVM_DELETEALLITEMS, 0, 0);

    int shown = 0;
    int flagged = 0;
    for (int i = 0; i < g_proc_count && shown < 60; i++) {
        char pid_text[16], cpu_text[16], mem_text[24];
        _snprintf(pid_text, sizeof(pid_text), "%lu", (unsigned long)g_procs[i].pid);
        _snprintf(cpu_text, sizeof(cpu_text), "%.1f", g_procs[i].cpu_percent);
        _snprintf(mem_text, sizeof(mem_text), "%.1f MB", g_procs[i].working_set_kb / 1024.0);

        LVITEMA item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = shown;
        item.pszText = pid_text;
        item.lParam = i;
        SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&item);

        ListView_SetItemText(g_list, shown, 1, g_procs[i].name);
        ListView_SetItemText(g_list, shown, 2, cpu_text);
        ListView_SetItemText(g_list, shown, 3, mem_text);
        ListView_SetItemText(g_list, shown, 4,
            (char *)(g_procs[i].flagged ? "low-value background app" : ""));

        if (g_procs[i].flagged) flagged++;
        if (selected_pid >= 0 && (int)g_procs[i].pid == selected_pid) {
            ListView_SetItemState(g_list, shown, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
        shown++;
    }

    SendMessage(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, NULL, TRUE);

    char msg[160];
    _snprintf(msg, sizeof(msg), "%d processes running, %d flagged as low-value background apps",
              g_proc_count, flagged);
    msg[sizeof(msg) - 1] = '\0';
    gui_status(msg);
}

static void proc_show(void) {
    g_visible = 1;
    proc_populate();
    ShowWindow(g_list, SW_SHOW);
    ShowWindow(g_kill_btn, SW_SHOW);
    ShowWindow(g_kill_all_btn, SW_SHOW);
    ShowWindow(g_refresh_btn, SW_SHOW);
    SetTimer(gui_main_window(), ID_PROC_TIMER, AUTO_REFRESH_MS, NULL);
}

static void proc_hide(void) {
    g_visible = 0;
    KillTimer(gui_main_window(), ID_PROC_TIMER);
    ShowWindow(g_list, SW_HIDE);
    ShowWindow(g_kill_btn, SW_HIDE);
    ShowWindow(g_kill_all_btn, SW_HIDE);
    ShowWindow(g_refresh_btn, SW_HIDE);
}

void gui_processes_tick(void) {
    if (g_visible) proc_populate();
}

static void kill_selected(void) {
    int row = (int)SendMessage(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (row < 0) {
        MessageBoxA(gui_main_window(), "Select a process first.", APP_NAME, MB_OK | MB_ICONINFORMATION);
        return;
    }

    LVITEMA item;
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!SendMessageA(g_list, LVM_GETITEMA, 0, (LPARAM)&item)) return;

    int index = (int)item.lParam;
    if (index < 0 || index >= g_proc_count) return;

    char prompt[400];
    _snprintf(prompt, sizeof(prompt),
              "End \"%s\" (PID %lu)?\n\nUnsaved work in that process will be lost.",
              g_procs[index].name, (unsigned long)g_procs[index].pid);
    prompt[sizeof(prompt) - 1] = '\0';
    if (MessageBoxA(gui_main_window(), prompt, APP_NAME, MB_YESNO | MB_ICONWARNING) != IDYES) return;

    if (proc_kill(g_procs[index].pid) != 0) {
        MessageBoxA(gui_main_window(),
                    "Could not end that process (access denied or it already exited).",
                    APP_NAME, MB_OK | MB_ICONWARNING);
    }
    proc_populate();
}

static void kill_all_flagged(void) {
    int flagged = 0;
    for (int i = 0; i < g_proc_count; i++) {
        if (g_procs[i].flagged) flagged++;
    }
    if (flagged == 0) {
        MessageBoxA(gui_main_window(), "No flagged background apps are running.",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
        return;
    }

    char prompt[256];
    _snprintf(prompt, sizeof(prompt),
              "Close %d flagged background app(s)?\n\nUnsaved work in them will be lost.", flagged);
    prompt[sizeof(prompt) - 1] = '\0';
    if (MessageBoxA(gui_main_window(), prompt, APP_NAME, MB_YESNO | MB_ICONWARNING) != IDYES) return;

    int killed = 0;
    for (int i = 0; i < g_proc_count; i++) {
        if (g_procs[i].flagged && proc_kill(g_procs[i].pid) == 0) killed++;
    }

    char msg[128];
    _snprintf(msg, sizeof(msg), "Closed %d of %d flagged process(es).", killed, flagged);
    msg[sizeof(msg) - 1] = '\0';
    MessageBoxA(gui_main_window(), msg, APP_NAME, MB_OK | MB_ICONINFORMATION);
    proc_populate();
}

static int proc_command(WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    switch (LOWORD(wparam)) {
        case ID_PROC_KILL:     kill_selected(); return 1;
        case ID_PROC_KILL_ALL: kill_all_flagged(); return 1;
        case ID_PROC_REFRESH:  proc_populate(); return 1;
    }
    return 0;
}

GuiPage g_page_processes = {
    proc_create, proc_layout, proc_show, proc_hide, proc_command, NULL
};
