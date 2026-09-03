#include "gui.h"
#include "profile.h"
#include <commctrl.h>

#define ID_PROF_LIST    600
#define ID_PROF_SAVE    601
#define ID_PROF_APPLY   602
#define ID_PROF_DELETE  603
#define ID_PROF_NAME    604
#define ID_PROF_OK      605
#define ID_PROF_CANCEL  606

#define BUTTON_HEIGHT 30
#define GAP 8

static HWND g_list, g_save_btn, g_apply_btn, g_delete_btn, g_help;
static char g_names[PROFILE_MAX_COUNT][PROFILE_NAME_MAX];
static int g_name_count;

/* --- Small modal prompt for the profile name (Win32 has no built-in input box) --- */

static char g_name_buffer[PROFILE_NAME_MAX];

static LRESULT CALLBACK name_dlg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE: {
            HWND label = CreateWindowExA(0, "STATIC",
                "Profile name (letters, numbers, spaces, - and _ only):",
                WS_CHILD | WS_VISIBLE, 12, 12, 360, 18, hwnd, NULL, NULL, NULL);
            HWND edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                12, 34, 360, 24, hwnd, (HMENU)ID_PROF_NAME, NULL, NULL);
            HWND ok = CreateWindowExA(0, "BUTTON", "Save",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                200, 70, 82, 26, hwnd, (HMENU)ID_PROF_OK, NULL, NULL);
            HWND cancel = CreateWindowExA(0, "BUTTON", "Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                290, 70, 82, 26, hwnd, (HMENU)ID_PROF_CANCEL, NULL, NULL);

            gui_apply_font(label);
            gui_apply_font(edit);
            gui_apply_font(ok);
            gui_apply_font(cancel);
            SendMessage(edit, EM_LIMITTEXT, PROFILE_NAME_MAX - 1, 0);
            SetFocus(edit);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wparam) == ID_PROF_OK) {
                GetDlgItemTextA(hwnd, ID_PROF_NAME, g_name_buffer, sizeof(g_name_buffer));
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wparam) == ID_PROF_CANCEL) {
                g_name_buffer[0] = '\0';
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_CLOSE:
            g_name_buffer[0] = '\0';
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* Returns 1 if the user entered a name, 0 if they cancelled. */
static int prompt_for_name(char *out, size_t out_size) {
    static const char *cls = "GameOptimizerNamePrompt";
    static int registered = 0;
    HINSTANCE inst = (HINSTANCE)GetModuleHandle(NULL);

    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = name_dlg_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = cls;
        RegisterClassA(&wc);
        registered = 1;
    }

    g_name_buffer[0] = '\0';
    HWND owner = gui_main_window();

    RECT orc;
    GetWindowRect(owner, &orc);
    int width = 400, height = 145;
    int x = orc.left + ((orc.right - orc.left) - width) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - height) / 2;

    HWND dlg = CreateWindowExA(WS_EX_DLGMODALFRAME, cls, "Save Profile",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               x, y, width, height, owner, NULL, inst, NULL);
    if (!dlg) return 0;

    EnableWindow(owner, FALSE);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessage(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);

    if (g_name_buffer[0] == '\0') return 0;
    strncpy(out, g_name_buffer, out_size - 1);
    out[out_size - 1] = '\0';
    return 1;
}

/* --- Page --- */

static void prof_create(HWND parent) {
    g_help = CreateWindowExA(0, "STATIC",
        "A profile is a snapshot of what was running when you saved it. Applying one closes "
        "anything running now that wasn't in the snapshot (core OS and security processes are "
        "never touched). Typical use: launch your game and the apps you want, save that as "
        "\"Gaming\", then apply it later to clear everything else in one step.",
        WS_CHILD, 0, 0, 0, 0, parent, NULL, NULL, NULL);
    gui_apply_font(g_help);

    g_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                             WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             0, 0, 0, 0, parent, (HMENU)ID_PROF_LIST, NULL, NULL);
    gui_apply_font(g_list);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNA col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = (char *)"Saved profiles"; col.cx = 400; col.iSubItem = 0;
    SendMessageA(g_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);

    g_save_btn = CreateWindowExA(0, "BUTTON", "Save Current State as Profile",
                                 WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                 0, 0, 0, 0, parent, (HMENU)ID_PROF_SAVE, NULL, NULL);
    g_apply_btn = CreateWindowExA(0, "BUTTON", "Apply Selected",
                                  WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                  0, 0, 0, 0, parent, (HMENU)ID_PROF_APPLY, NULL, NULL);
    g_delete_btn = CreateWindowExA(0, "BUTTON", "Delete Selected",
                                   WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 0, 0, parent, (HMENU)ID_PROF_DELETE, NULL, NULL);
    gui_apply_font(g_save_btn);
    gui_apply_font(g_apply_btn);
    gui_apply_font(g_delete_btn);
}

static void prof_layout(int x, int y, int w, int h) {
    int help_height = 66;
    MoveWindow(g_help, x, y, w, help_height, TRUE);

    int list_y = y + help_height + GAP;
    int list_height = h - help_height - GAP - BUTTON_HEIGHT - GAP;
    if (list_height < 0) list_height = 0;
    MoveWindow(g_list, x, list_y, w, list_height, TRUE);

    int by = list_y + list_height + GAP;
    MoveWindow(g_save_btn, x, by, 220, BUTTON_HEIGHT, TRUE);
    MoveWindow(g_apply_btn, x + 228, by, 140, BUTTON_HEIGHT, TRUE);
    MoveWindow(g_delete_btn, x + 376, by, 140, BUTTON_HEIGHT, TRUE);
}

static void prof_populate(void) {
    SendMessage(g_list, LVM_DELETEALLITEMS, 0, 0);
    g_name_count = profile_list(g_names, PROFILE_MAX_COUNT);

    for (int i = 0; i < g_name_count; i++) {
        LVITEMA item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = g_names[i];
        SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&item);
    }
}

static void prof_show(void) {
    prof_populate();
    ShowWindow(g_help, SW_SHOW);
    ShowWindow(g_list, SW_SHOW);
    ShowWindow(g_save_btn, SW_SHOW);
    ShowWindow(g_apply_btn, SW_SHOW);
    ShowWindow(g_delete_btn, SW_SHOW);

    char msg[96];
    _snprintf(msg, sizeof(msg), "%d saved profile(s)", g_name_count);
    msg[sizeof(msg) - 1] = '\0';
    gui_status(msg);
}

static void prof_hide(void) {
    ShowWindow(g_help, SW_HIDE);
    ShowWindow(g_list, SW_HIDE);
    ShowWindow(g_save_btn, SW_HIDE);
    ShowWindow(g_apply_btn, SW_HIDE);
    ShowWindow(g_delete_btn, SW_HIDE);
}

static int selected_index(void) {
    return (int)SendMessage(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

static void do_save(void) {
    char name[PROFILE_NAME_MAX];
    if (!prompt_for_name(name, sizeof(name))) return;

    if (!profile_name_is_valid(name)) {
        MessageBoxA(gui_main_window(),
                    "That name isn't valid. Use only letters, numbers, spaces, hyphens and underscores.",
                    APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }

    gui_status("Saving profile...");
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int rc = profile_save(name);
    SetCursor(old);

    if (rc == 0) {
        char msg[256];
        _snprintf(msg, sizeof(msg),
                  "Profile \"%s\" saved from everything running right now.", name);
        msg[sizeof(msg) - 1] = '\0';
        MessageBoxA(gui_main_window(), msg, APP_NAME, MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxA(gui_main_window(), "Could not save that profile.", APP_NAME, MB_OK | MB_ICONWARNING);
    }
    prof_show();
}

static void build_diff_text(const char *name, const ProfileDiff *diff, char *out, size_t out_size) {
    _snprintf(out, out_size, "Applying profile \"%s\" will:\r\n\r\nClose %d process(es):\r\n",
              name, diff->procs_to_close_count);
    out[out_size - 1] = '\0';

    for (int i = 0; i < diff->procs_to_close_count && i < 40; i++) {
        char line[300];
        _snprintf(line, sizeof(line), "   %s (PID %lu)\r\n",
                  diff->procs_to_close[i].name, (unsigned long)diff->procs_to_close[i].pid);
        line[sizeof(line) - 1] = '\0';
        strncat(out, line, out_size - strlen(out) - 1);
    }
    if (diff->procs_to_close_count > 40) {
        strncat(out, "   ...\r\n", out_size - strlen(out) - 1);
    }
    if (diff->procs_to_close_count == 0) {
        strncat(out, "   (none)\r\n", out_size - strlen(out) - 1);
    }

    char svc_header[128];
    _snprintf(svc_header, sizeof(svc_header), "\r\nStop and disable %d service(s):\r\n",
              diff->services_to_stop_count);
    svc_header[sizeof(svc_header) - 1] = '\0';
    strncat(out, svc_header, out_size - strlen(out) - 1);

    for (int i = 0; i < diff->services_to_stop_count; i++) {
        char line[128];
        _snprintf(line, sizeof(line), "   %s\r\n", diff->services_to_stop[i]);
        line[sizeof(line) - 1] = '\0';
        strncat(out, line, out_size - strlen(out) - 1);
    }
    if (diff->services_to_stop_count == 0) {
        strncat(out, "   (none)\r\n", out_size - strlen(out) - 1);
    }

    if (diff->procs_protected_count > 0) {
        char note[200];
        _snprintf(note, sizeof(note),
                  "\r\n%d core OS/security process(es) were skipped and will be left alone.\r\n",
                  diff->procs_protected_count);
        note[sizeof(note) - 1] = '\0';
        strncat(out, note, out_size - strlen(out) - 1);
    }

    strncat(out, "\r\nProceed?", out_size - strlen(out) - 1);
}

static void do_apply(void) {
    int row = selected_index();
    if (row < 0 || row >= g_name_count) {
        MessageBoxA(gui_main_window(), "Select a profile first.", APP_NAME, MB_OK | MB_ICONINFORMATION);
        return;
    }

    gui_status("Comparing current state to the profile...");
    static ProfileDiff diff;
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int rc = profile_compute_diff(g_names[row], &diff);
    SetCursor(old);

    if (rc != 0) {
        MessageBoxA(gui_main_window(), "Could not read that profile.", APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }

    if (diff.procs_to_close_count == 0 && diff.services_to_stop_count == 0) {
        MessageBoxA(gui_main_window(),
                    "Nothing to do -- what's running now already matches this profile.",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
        return;
    }

    static char text[16384];
    build_diff_text(g_names[row], &diff, text, sizeof(text));

    if (MessageBoxA(gui_main_window(), text, APP_NAME, MB_YESNO | MB_ICONWARNING) != IDYES) {
        gui_status("Cancelled");
        return;
    }

    ProfileApplyResult result;
    old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    profile_execute_diff(&diff, &result);
    SetCursor(old);

    char msg[400];
    _snprintf(msg, sizeof(msg),
              "Closed %d process(es) and stopped %d service(s).%s",
              result.processes_closed, result.services_stopped,
              (result.processes_failed || result.services_failed)
                  ? "\n\nSome items could not be closed -- those usually need Administrator rights."
                  : "");
    msg[sizeof(msg) - 1] = '\0';
    MessageBoxA(gui_main_window(), msg, APP_NAME, MB_OK | MB_ICONINFORMATION);
    gui_status("Profile applied");
}

static void do_delete(void) {
    int row = selected_index();
    if (row < 0 || row >= g_name_count) {
        MessageBoxA(gui_main_window(), "Select a profile first.", APP_NAME, MB_OK | MB_ICONINFORMATION);
        return;
    }

    char prompt[256];
    _snprintf(prompt, sizeof(prompt), "Delete the profile \"%s\"?", g_names[row]);
    prompt[sizeof(prompt) - 1] = '\0';
    if (MessageBoxA(gui_main_window(), prompt, APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    if (profile_delete(g_names[row]) != 0) {
        MessageBoxA(gui_main_window(), "Could not delete that profile.", APP_NAME, MB_OK | MB_ICONWARNING);
    }
    prof_show();
}

static int prof_command(WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    switch (LOWORD(wparam)) {
        case ID_PROF_SAVE:   do_save(); return 1;
        case ID_PROF_APPLY:  do_apply(); return 1;
        case ID_PROF_DELETE: do_delete(); return 1;
    }
    return 0;
}

static int prof_notify(LPNMHDR hdr) {
    if (hdr->idFrom == ID_PROF_LIST && hdr->code == NM_DBLCLK) {
        do_apply();
        return 1;
    }
    return 0;
}

GuiPage g_page_profiles = {
    prof_create, prof_layout, prof_show, prof_hide, prof_command, prof_notify
};
