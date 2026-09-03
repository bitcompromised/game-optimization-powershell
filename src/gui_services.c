#include "gui.h"
#include "svc_monitor.h"
#include "svc_utils.h"
#include <commctrl.h>

#define ID_SVC_LIST    400
#define ID_SVC_TOGGLE  401
#define ID_SVC_REFRESH 402

#define BUTTON_HEIGHT 30
#define GAP 8

static HWND g_list, g_toggle_btn, g_refresh_btn;

static void svc_create(HWND parent) {
    g_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                             WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             0, 0, 0, 0, parent, (HMENU)ID_SVC_LIST, NULL, NULL);
    gui_apply_font(g_list);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);

    LVCOLUMNA col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = (char *)"Service";    col.cx = 170; col.iSubItem = 0;
    SendMessageA(g_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    col.pszText = (char *)"State";      col.cx = 90;  col.iSubItem = 1;
    SendMessageA(g_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
    col.pszText = (char *)"Start type"; col.cx = 100; col.iSubItem = 2;
    SendMessageA(g_list, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);
    col.pszText = (char *)"Why it's listed"; col.cx = 500; col.iSubItem = 3;
    SendMessageA(g_list, LVM_INSERTCOLUMNA, 3, (LPARAM)&col);

    g_toggle_btn = CreateWindowExA(0, "BUTTON", "Disable / Re-enable Selected",
                                   WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 0, 0, parent, (HMENU)ID_SVC_TOGGLE, NULL, NULL);
    g_refresh_btn = CreateWindowExA(0, "BUTTON", "Refresh",
                                    WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                    0, 0, 0, 0, parent, (HMENU)ID_SVC_REFRESH, NULL, NULL);
    gui_apply_font(g_toggle_btn);
    gui_apply_font(g_refresh_btn);
}

static void svc_layout(int x, int y, int w, int h) {
    int list_height = h - BUTTON_HEIGHT - GAP;
    if (list_height < 0) list_height = 0;
    MoveWindow(g_list, x, y, w, list_height, TRUE);

    int by = y + list_height + GAP;
    MoveWindow(g_toggle_btn, x, by, 220, BUTTON_HEIGHT, TRUE);
    MoveWindow(g_refresh_btn, x + 220 + GAP, by, 110, BUTTON_HEIGHT, TRUE);
}

static void svc_populate(void) {
    int selected = (int)SendMessage(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    SendMessage(g_list, LVM_DELETEALLITEMS, 0, 0);

    for (int i = 0; i < KNOWN_SERVICE_COUNT; i++) {
        const KnownService *ks = &KNOWN_SERVICES[i];
        DWORD start_type = 0, state = 0;
        int have_start = (svc_query_start_type(ks->service_name, &start_type) == 0);
        int have_state = (svc_query_state(ks->service_name, &state) == 0);

        LVITEMA item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.pszText = (char *)ks->service_name;
        item.lParam = i;
        SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&item);

        if (!have_start && !have_state) {
            ListView_SetItemText(g_list, i, 1, (char *)"not present");
            ListView_SetItemText(g_list, i, 2, (char *)"-");
        } else {
            ListView_SetItemText(g_list, i, 1,
                (char *)(have_state ? svc_state_str(state) : "?"));
            ListView_SetItemText(g_list, i, 2,
                (char *)(have_start ? svc_start_type_str(start_type) : "?"));
        }
        ListView_SetItemText(g_list, i, 3, (char *)ks->note);
    }

    if (selected >= 0) {
        ListView_SetItemState(g_list, selected, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
}

static void svc_show(void) {
    svc_populate();
    ShowWindow(g_list, SW_SHOW);
    ShowWindow(g_toggle_btn, SW_SHOW);
    ShowWindow(g_refresh_btn, SW_SHOW);
    gui_status("Service monitor -- select a service and use the button below to toggle it");
}

static void svc_hide(void) {
    ShowWindow(g_list, SW_HIDE);
    ShowWindow(g_toggle_btn, SW_HIDE);
    ShowWindow(g_refresh_btn, SW_HIDE);
}

static void toggle_selected(void) {
    int row = (int)SendMessage(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (row < 0) {
        MessageBoxA(gui_main_window(), "Select a service first.", APP_NAME, MB_OK | MB_ICONINFORMATION);
        return;
    }

    const KnownService *ks = &KNOWN_SERVICES[row];
    DWORD start_type = 0;
    int is_disabled = (svc_query_start_type(ks->service_name, &start_type) == 0 &&
                       start_type == SERVICE_DISABLED);

    char prompt[512];
    _snprintf(prompt, sizeof(prompt), "%s the \"%s\" service?\n\n%s",
              is_disabled ? "Re-enable and start" : "Stop and disable",
              ks->service_name, ks->note);
    prompt[sizeof(prompt) - 1] = '\0';
    if (MessageBoxA(gui_main_window(), prompt, APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int rc;
    if (is_disabled) {
        rc = svc_set_start_type(ks->service_name, SERVICE_DEMAND_START);
        if (rc == 0) svc_start(ks->service_name);
    } else {
        svc_stop(ks->service_name);
        rc = svc_set_start_type(ks->service_name, SERVICE_DISABLED);
    }
    SetCursor(old);

    if (rc != 0) {
        MessageBoxA(gui_main_window(),
                    "Could not change that service.\n\nChanging service start types requires "
                    "running GameOptimizer as Administrator.",
                    APP_NAME, MB_OK | MB_ICONWARNING);
    }
    svc_populate();
}

static int svc_command(WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    switch (LOWORD(wparam)) {
        case ID_SVC_TOGGLE:  toggle_selected(); return 1;
        case ID_SVC_REFRESH: svc_populate(); return 1;
    }
    return 0;
}

static int svc_notify(LPNMHDR hdr) {
    if (hdr->idFrom == ID_SVC_LIST && hdr->code == NM_DBLCLK) {
        toggle_selected();
        return 1;
    }
    return 0;
}

GuiPage g_page_services = {
    svc_create, svc_layout, svc_show, svc_hide, svc_command, svc_notify
};
