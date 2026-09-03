#include "gui.h"
#include <commctrl.h>

#define ID_PERF_LIST     200
#define ID_PERF_APPLY    201
#define ID_PERF_REVERT   202
#define ID_PERF_REFRESH  203
#define ID_TELE_LIST     210
#define ID_TELE_APPLY    211
#define ID_TELE_REVERT   212
#define ID_TELE_REFRESH  213

#define BUTTON_HEIGHT 30
#define BUTTON_GAP    8

typedef struct {
    TweakCategory category;
    int id_list, id_apply, id_revert, id_refresh;
    const char *apply_label;
    const char *revert_label;
    HWND list, apply_btn, revert_btn, refresh_btn;
    int updating;   /* suppresses re-entrant handling while we set check states ourselves */
} TweakPageState;

static TweakPageState g_perf = {
    CAT_PERFORMANCE, ID_PERF_LIST, ID_PERF_APPLY, ID_PERF_REVERT, ID_PERF_REFRESH,
    "Apply All Performance Tweaks", "Revert All", NULL, NULL, NULL, NULL, 0
};
static TweakPageState g_tele = {
    CAT_TELEMETRY, ID_TELE_LIST, ID_TELE_APPLY, ID_TELE_REVERT, ID_TELE_REFRESH,
    "Disable All Tracking", "Restore All", NULL, NULL, NULL, NULL, 0
};

static void page_create(TweakPageState *st, HWND parent) {
    st->list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                               WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                               0, 0, 0, 0, parent, (HMENU)(INT_PTR)st->id_list, NULL, NULL);
    gui_apply_font(st->list);
    ListView_SetExtendedListViewStyle(st->list,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);

    LVCOLUMNA col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = (char *)"Tweak";       col.cx = 260; col.iSubItem = 0;
    SendMessageA(st->list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    col.pszText = (char *)"Status";      col.cx = 80;  col.iSubItem = 1;
    SendMessageA(st->list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
    col.pszText = (char *)"Admin";       col.cx = 55;  col.iSubItem = 2;
    SendMessageA(st->list, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);
    col.pszText = (char *)"What it does"; col.cx = 480; col.iSubItem = 3;
    SendMessageA(st->list, LVM_INSERTCOLUMNA, 3, (LPARAM)&col);

    st->apply_btn = CreateWindowExA(0, "BUTTON", st->apply_label,
                                    WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                    0, 0, 0, 0, parent, (HMENU)(INT_PTR)st->id_apply, NULL, NULL);
    st->revert_btn = CreateWindowExA(0, "BUTTON", st->revert_label,
                                     WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                     0, 0, 0, 0, parent, (HMENU)(INT_PTR)st->id_revert, NULL, NULL);
    st->refresh_btn = CreateWindowExA(0, "BUTTON", "Re-scan System",
                                      WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 0, 0, parent, (HMENU)(INT_PTR)st->id_refresh, NULL, NULL);
    gui_apply_font(st->apply_btn);
    gui_apply_font(st->revert_btn);
    gui_apply_font(st->refresh_btn);
}

static void page_layout(TweakPageState *st, int x, int y, int w, int h) {
    int list_height = h - BUTTON_HEIGHT - BUTTON_GAP;
    if (list_height < 0) list_height = 0;
    MoveWindow(st->list, x, y, w, list_height, TRUE);

    int by = y + list_height + BUTTON_GAP;
    int apply_w = 230, revert_w = 120, refresh_w = 130;
    MoveWindow(st->apply_btn, x, by, apply_w, BUTTON_HEIGHT, TRUE);
    MoveWindow(st->revert_btn, x + apply_w + BUTTON_GAP, by, revert_w, BUTTON_HEIGHT, TRUE);
    MoveWindow(st->refresh_btn, x + apply_w + revert_w + BUTTON_GAP * 2, by, refresh_w, BUTTON_HEIGHT, TRUE);
}

/* Repopulates the list from g_tweaks. lParam on each row is the index into g_tweaks. */
static void page_populate(TweakPageState *st) {
    st->updating = 1;
    SendMessage(st->list, LVM_DELETEALLITEMS, 0, 0);

    int row = 0;
    for (int i = 0; i < g_tweak_count; i++) {
        if (g_tweaks[i].category != st->category) continue;

        LVITEMA item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.pszText = (char *)g_tweaks[i].name;
        item.lParam = i;
        SendMessageA(st->list, LVM_INSERTITEMA, 0, (LPARAM)&item);

        ListView_SetItemText(st->list, row, 1, (char *)(g_tweaks[i].enabled ? "ON" : "OFF"));
        ListView_SetItemText(st->list, row, 2,
            (char *)(g_tweaks[i].requires_admin ? (is_running_elevated() ? "yes" : "NEEDED") : "-"));
        ListView_SetItemText(st->list, row, 3, (char *)g_tweaks[i].description);

        ListView_SetCheckState(st->list, row, g_tweaks[i].enabled ? TRUE : FALSE);
        row++;
    }
    st->updating = 0;
}

static void page_show(TweakPageState *st) {
    page_populate(st);
    ShowWindow(st->list, SW_SHOW);
    ShowWindow(st->apply_btn, SW_SHOW);
    ShowWindow(st->revert_btn, SW_SHOW);
    ShowWindow(st->refresh_btn, SW_SHOW);

    int on = 0, total = 0;
    for (int i = 0; i < g_tweak_count; i++) {
        if (g_tweaks[i].category != st->category) continue;
        total++;
        if (g_tweaks[i].enabled) on++;
    }
    char msg[128];
    _snprintf(msg, sizeof(msg), "%d of %d %s tweaks currently active on this system",
              on, total, st->category == CAT_PERFORMANCE ? "performance" : "privacy");
    msg[sizeof(msg) - 1] = '\0';
    gui_status(msg);
}

static void page_hide(TweakPageState *st) {
    ShowWindow(st->list, SW_HIDE);
    ShowWindow(st->apply_btn, SW_HIDE);
    ShowWindow(st->revert_btn, SW_HIDE);
    ShowWindow(st->refresh_btn, SW_HIDE);
}

static void toggle_row(TweakPageState *st, int row) {
    LVITEMA item;
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!SendMessageA(st->list, LVM_GETITEMA, 0, (LPARAM)&item)) return;

    int index = (int)item.lParam;
    if (index < 0 || index >= g_tweak_count) return;

    int was_enabled = g_tweaks[index].enabled;

    char msg[256];
    _snprintf(msg, sizeof(msg), "%s %s...", was_enabled ? "Reverting" : "Applying", g_tweaks[index].name);
    msg[sizeof(msg) - 1] = '\0';
    gui_status(msg);

    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int rc = tweaks_toggle(index);
    SetCursor(old);

    if (rc != 0) {
        char err[512];
        _snprintf(err, sizeof(err), "Could not %s \"%s\".%s",
                  was_enabled ? "revert" : "apply", g_tweaks[index].name,
                  (g_tweaks[index].requires_admin && !is_running_elevated())
                      ? "\n\nThis tweak needs GameOptimizer to run as Administrator."
                      : "");
        err[sizeof(err) - 1] = '\0';
        MessageBoxA(gui_main_window(), err, APP_NAME, MB_OK | MB_ICONWARNING);
    }

    /* tweaks_toggle() re-reads live state, so repopulating always shows the truth --
     * including the case where an apply silently failed and the row snaps back to OFF. */
    page_show(st);
}

static int page_command(TweakPageState *st, WPARAM wparam) {
    int id = LOWORD(wparam);

    if (id == st->id_apply || id == st->id_revert) {
        int want = (id == st->id_apply);
        int failed = gui_bulk_toggle(st->category, want);
        gui_refresh_tweak_state();

        if (failed > 0) {
            char msg[256];
            _snprintf(msg, sizeof(msg),
                      "%d tweak(s) could not be %s.%s", failed, want ? "applied" : "reverted",
                      is_running_elevated() ? "" : "\n\nMost of these need Administrator rights.");
            msg[sizeof(msg) - 1] = '\0';
            MessageBoxA(gui_main_window(), msg, APP_NAME, MB_OK | MB_ICONWARNING);
        }
        return 1;
    }

    if (id == st->id_refresh) {
        gui_status("Re-scanning system state...");
        gui_refresh_tweak_state();
        return 1;
    }
    return 0;
}

static int page_notify(TweakPageState *st, LPNMHDR hdr) {
    if (hdr->idFrom != (UINT_PTR)st->id_list) return 0;
    if (st->updating) return 0;

    if (hdr->code == LVN_ITEMCHANGED) {
        NMLISTVIEW *nlv = (NMLISTVIEW *)hdr;
        /* Bits 12-15 of the state hold the check image index; a change there means the user
         * clicked the checkbox. Image 1 = unchecked, 2 = checked. */
        if (!(nlv->uChanged & LVIF_STATE)) return 0;
        UINT old_check = (nlv->uOldState & LVIS_STATEIMAGEMASK) >> 12;
        UINT new_check = (nlv->uNewState & LVIS_STATEIMAGEMASK) >> 12;
        if (old_check == 0 || old_check == new_check) return 0;

        toggle_row(st, nlv->iItem);
        return 1;
    }
    return 0;
}

/* ---- Page interface bindings ---- */

static void perf_create(HWND parent) { page_create(&g_perf, parent); }
static void perf_layout(int x, int y, int w, int h) { page_layout(&g_perf, x, y, w, h); }
static void perf_show(void) { page_show(&g_perf); }
static void perf_hide(void) { page_hide(&g_perf); }
static int  perf_command(WPARAM w, LPARAM l) { (void)l; return page_command(&g_perf, w); }
static int  perf_notify(LPNMHDR h) { return page_notify(&g_perf, h); }

static void tele_create(HWND parent) { page_create(&g_tele, parent); }
static void tele_layout(int x, int y, int w, int h) { page_layout(&g_tele, x, y, w, h); }
static void tele_show(void) { page_show(&g_tele); }
static void tele_hide(void) { page_hide(&g_tele); }
static int  tele_command(WPARAM w, LPARAM l) { (void)l; return page_command(&g_tele, w); }
static int  tele_notify(LPNMHDR h) { return page_notify(&g_tele, h); }

GuiPage g_page_performance = {
    perf_create, perf_layout, perf_show, perf_hide, perf_command, perf_notify
};
GuiPage g_page_telemetry = {
    tele_create, tele_layout, tele_show, tele_hide, tele_command, tele_notify
};
