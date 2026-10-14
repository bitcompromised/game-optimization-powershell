#include "gui.h"
#include "svc_monitor.h"
#include "svc_utils.h"

#define ID_LIST      400
#define ID_SEARCH    401
#define ID_DISABLE   402
#define ID_ENABLE    403
#define ID_REFRESH   404
#define ID_CHIP_BASE 410

#define ROW_HEIGHT 58
#define MAX_SVC    64

enum { FILTER_ALL, FILTER_RUNNING, FILTER_DISABLED, FILTER_COUNT };
static const char *FILTER_LABELS[FILTER_COUNT] = { "All", "Running", "Disabled" };

typedef struct {
    char display[128];
    int present;
    DWORD state;
    DWORD start;
} SvcRow;

static HWND g_panel, g_list, g_search, g_disable, g_enable, g_refresh, g_chips[FILTER_COUNT];
static SvcRow g_svc[MAX_SVC];
static int g_rows[MAX_SVC], g_row_count;
static int g_filter;
static char g_query[128];
static int g_width;
static RECT g_list_card;
static int g_names_loaded;

/* Start type each service had before this session disabled it, so re-enabling restores it
 * (Automatic stays Automatic) instead of guessing Manual. */
static DWORD g_prev_start[MAX_SVC];

/* ---- data ---- */

static void query_all(void) {
    for (int i = 0; i < KNOWN_SERVICE_COUNT && i < MAX_SVC; i++) {
        SvcRow *s = &g_svc[i];
        if (!g_names_loaded) {
            svc_query_display_name(KNOWN_SERVICES[i].service_name, s->display, sizeof(s->display));
            if (!s->display[0]) strncpy(s->display, KNOWN_SERVICES[i].service_name, sizeof(s->display) - 1);
        }
        int have_start = svc_query_start_type(KNOWN_SERVICES[i].service_name, &s->start) == 0;
        int have_state = svc_query_state(KNOWN_SERVICES[i].service_name, &s->state) == 0;
        s->present = have_start || have_state;
    }
    g_names_loaded = 1;
}

static int matches(int i) {
    const SvcRow *s = &g_svc[i];
    if (g_filter == FILTER_RUNNING && !(s->present && s->state == SERVICE_RUNNING)) return 0;
    if (g_filter == FILTER_DISABLED && !(s->present && s->start == SERVICE_DISABLED)) return 0;
    if (g_query[0] && !ui_stristr(s->display, g_query) && !ui_stristr(KNOWN_SERVICES[i].service_name, g_query) &&
        !ui_stristr(KNOWN_SERVICES[i].note, g_query)) {
        return 0;
    }
    return 1;
}

static void update_buttons(void) {
    int can_disable = 0, can_enable = 0;
    for (int row = ListView_GetNextItem(g_list, -1, LVNI_SELECTED); row >= 0;
         row = ListView_GetNextItem(g_list, row, LVNI_SELECTED)) {
        const SvcRow *s = &g_svc[g_rows[row]];
        if (!s->present) continue;
        if (s->start == SERVICE_DISABLED) can_enable++;
        else can_disable++;
    }
    char label[48];
    _snprintf(label, sizeof(label), can_disable > 1 ? "Disable %d" : "Disable", can_disable);
    label[sizeof(label) - 1] = '\0';
    ui_button_set_text(g_disable, label);
    _snprintf(label, sizeof(label), can_enable > 1 ? "Enable %d" : "Enable", can_enable);
    label[sizeof(label) - 1] = '\0';
    ui_button_set_text(g_enable, label);
    EnableWindow(g_disable, can_disable > 0);
    EnableWindow(g_enable, can_enable > 0);
}

static void build_rows(void) {
    int keep[MAX_SVC], keep_count = 0;
    for (int row = ListView_GetNextItem(g_list, -1, LVNI_SELECTED); row >= 0 && keep_count < MAX_SVC;
         row = ListView_GetNextItem(g_list, row, LVNI_SELECTED)) {
        if (row < g_row_count) keep[keep_count++] = g_rows[row];
    }

    g_row_count = 0;
    for (int i = 0; i < KNOWN_SERVICE_COUNT && i < MAX_SVC; i++) {
        if (matches(i)) g_rows[g_row_count++] = i;
    }
    ListView_SetItemCountEx(g_list, g_row_count, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
    for (int r = 0; r < g_row_count; r++) {
        for (int k = 0; k < keep_count; k++) {
            if (g_rows[r] == keep[k]) ListView_SetItemState(g_list, r, LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    ui_list_set_empty(g_list, g_query[0] ? "No service matches that search." : "No services match this filter.");
    InvalidateRect(g_list, NULL, FALSE);
    update_buttons();
    InvalidateRect(g_panel, NULL, FALSE);
}

static void summary(int *running, int *disabled, int *present) {
    *running = *disabled = *present = 0;
    for (int i = 0; i < KNOWN_SERVICE_COUNT && i < MAX_SVC; i++) {
        if (!g_svc[i].present) continue;
        (*present)++;
        if (g_svc[i].state == SERVICE_RUNNING) (*running)++;
        if (g_svc[i].start == SERVICE_DISABLED) (*disabled)++;
    }
}

static void reload(void) {
    query_all();
    build_rows();
    int running, disabled, present;
    summary(&running, &disabled, &present);
    char msg[160];
    _snprintf(msg, sizeof(msg), "%d of %d listed services installed \xC2\xB7 %d running \xC2\xB7 %d disabled",
              present, KNOWN_SERVICE_COUNT, running, disabled);
    msg[sizeof(msg) - 1] = '\0';
    gui_status(msg);
}

/* ---- jobs ---- */

typedef struct {
    int idx[MAX_SVC];
    DWORD target_start[MAX_SVC];
    int start_after[MAX_SVC];      /* start the service once enabled */
    int count;
    int disabling;
    int is_undo;
    DWORD before_start[MAX_SVC];
    DWORD before_state[MAX_SVC];
    int ok, failed;
} SvcJob;

static SvcJob g_undo;
static int g_undo_valid;

static void svc_work(GuiJob *job) {
    SvcJob *j = (SvcJob *)job->ctx;
    for (int k = 0; k < j->count; k++) {
        const char *name = KNOWN_SERVICES[j->idx[k]].service_name;
        char step[160];
        _snprintf(step, sizeof(step), "%s %s\xE2\x80\xA6",
                  j->target_start[k] == SERVICE_DISABLED ? "Stopping and disabling" : "Enabling", name);
        step[sizeof(step) - 1] = '\0';
        gui_job_step(job, k, j->count, step);

        j->before_start[k] = SERVICE_DEMAND_START;
        j->before_state[k] = SERVICE_STOPPED;
        svc_query_start_type(name, &j->before_start[k]);
        svc_query_state(name, &j->before_state[k]);

        int rc;
        if (j->target_start[k] == SERVICE_DISABLED) {
            svc_stop(name);
            rc = svc_set_start_type(name, SERVICE_DISABLED);
        } else {
            rc = svc_set_start_type(name, j->target_start[k]);
            if (rc == 0 && j->start_after[k]) svc_start(name);
        }
        if (rc == 0) j->ok++;
        else j->failed++;
    }
}

static void undo_services(void);

static void svc_done(GuiJob *job) {
    SvcJob *j = (SvcJob *)job->ctx;
    char msg[240];
    if (j->count == 1) {
        const char *name = g_svc[j->idx[0]].display;
        if (j->failed) _snprintf(msg, sizeof(msg), "Couldn't change \xE2\x80\x9C%s\xE2\x80\x9D.", name);
        else _snprintf(msg, sizeof(msg), "%s \xE2\x80\x9C%s\xE2\x80\x9D", j->is_undo ? "Restored" : j->disabling ? "Disabled" : "Enabled", name);
    } else {
        int n = _snprintf(msg, sizeof(msg), "%s %d service%s", j->is_undo ? "Restored" : j->disabling ? "Disabled" : "Enabled",
                          j->ok, j->ok == 1 ? "" : "s");
        if (j->failed && n > 0) _snprintf(msg + n, sizeof(msg) - n, " \xC2\xB7 %d couldn't be changed", j->failed);
    }
    msg[sizeof(msg) - 1] = '\0';

    for (int k = 0; k < j->count; k++) {
        if (j->target_start[k] == SERVICE_DISABLED && j->before_start[k] != SERVICE_DISABLED) {
            g_prev_start[j->idx[k]] = j->before_start[k];
        }
    }

    int offer = !j->is_undo && j->ok > 0;
    if (offer) {
        memset(&g_undo, 0, sizeof(g_undo));
        g_undo.is_undo = 1;
        for (int k = 0; k < j->count; k++) {
            g_undo.idx[g_undo.count] = j->idx[k];
            g_undo.target_start[g_undo.count] = j->before_start[k];
            g_undo.start_after[g_undo.count] = j->before_state[k] == SERVICE_RUNNING;
            g_undo.count++;
        }
        g_undo_valid = 1;
    } else if (j->is_undo) {
        g_undo_valid = 0;
    }

    gui_toast(j->failed ? TONE_WARN : TONE_GOOD, msg, offer ? "Undo" : NULL, offer ? undo_services : NULL);
    if (gui_current_page() == PAGE_SERVICES) reload();
    gui_state_changed();
}

static void start_job(SvcJob *j, const char *label) {
    gui_job_start(label, svc_work, svc_done, j);
}

static void undo_services(void) {
    if (!g_undo_valid) return;
    SvcJob *j = (SvcJob *)malloc(sizeof(SvcJob));
    if (!j) return;
    *j = g_undo;
    j->ok = j->failed = 0;
    start_job(j, "Restoring services");
}

static void change_selected(int disable, int only_row) {
    if (!is_running_elevated()) {
        gui_offer_elevation(disable ? "Disabling services" : "Enabling services");
        return;
    }
    SvcJob *j = (SvcJob *)calloc(1, sizeof(SvcJob));
    if (!j) return;
    j->disabling = disable;

    char details[2048] = "";
    size_t dlen = 0;
    for (int row = only_row >= 0 ? only_row : ListView_GetNextItem(g_list, -1, LVNI_SELECTED); row >= 0;
         row = only_row >= 0 ? -1 : ListView_GetNextItem(g_list, row, LVNI_SELECTED)) {
        int i = g_rows[row];
        const SvcRow *s = &g_svc[i];
        if (!s->present) continue;
        if (disable == (s->start == SERVICE_DISABLED)) continue;
        j->idx[j->count] = i;
        j->target_start[j->count] = disable ? SERVICE_DISABLED
                                            : (g_prev_start[i] ? g_prev_start[i] : SERVICE_DEMAND_START);
        j->start_after[j->count] = !disable;
        j->count++;
        if (dlen < sizeof(details) - 1) {
            int w = _snprintf(details + dlen, sizeof(details) - dlen, "%s%s \xE2\x80\x94 %s",
                              dlen ? "\r\n" : "", s->display, KNOWN_SERVICES[i].note);
            if (w > 0) dlen += (size_t)w;
            else dlen = sizeof(details) - 1;
        }
    }
    details[sizeof(details) - 1] = '\0';
    if (j->count == 0) {
        free(j);
        return;
    }

    if (disable) {
        char heading[96], ok[48];
        if (j->count == 1) _snprintf(heading, sizeof(heading), "Stop and disable %s?", g_svc[j->idx[0]].display);
        else _snprintf(heading, sizeof(heading), "Stop and disable %d services?", j->count);
        _snprintf(ok, sizeof(ok), j->count == 1 ? "Disable" : "Disable %d", j->count);
        heading[sizeof(heading) - 1] = ok[sizeof(ok) - 1] = '\0';
        UiDialog d;
        memset(&d, 0, sizeof(d));
        d.heading = heading;
        d.body = "Each service is stopped now and won't start again until you re-enable it here. "
                 "You can undo this from the status bar afterwards.";
        d.details = details;
        d.ok = ok;
        d.cancel = "Cancel";
        d.danger = 1;
        d.tone = TONE_WARN;
        d.glyph = GLYPH_WARNING;
        if (!ui_dialog(gui_main_window(), &d)) {
            free(j);
            return;
        }
    }
    start_job(j, disable ? "Disabling services" : "Enabling services");
}

/* ---- painting ---- */

static void draw_row(HDC dc, int row) {
    RECT rc;
    ui_list_row_rect(g_list, row, &rc);
    ui_list_row_bg(dc, g_list, &rc, row);
    int i = g_rows[row];
    const SvcRow *s = &g_svc[i];
    RECT client;
    GetClientRect(g_list, &client);
    int cy = (rc.top + rc.bottom) / 2;

    COLORREF dot = !s->present ? g_ui.border_strong
                 : s->start == SERVICE_DISABLED ? g_ui.bad
                 : s->state == SERVICE_RUNNING ? g_ui.good : g_ui.text_faint;
    int dx = client.left + ui_px(24);
    RECT d = { dx, cy - ui_px(5), dx + ui_px(10), cy + ui_px(5) };
    ui_ellipse(dc, &d, dot);

    int right = client.right - ui_px(20);
    const char *pill;
    UiTone tone;
    if (!s->present)                       { pill = "Not installed"; tone = TONE_NEUTRAL; }
    else if (s->start == SERVICE_DISABLED) { pill = "Disabled"; tone = TONE_BAD; }
    else if (s->state == SERVICE_RUNNING)  { pill = "Running"; tone = TONE_GOOD; }
    else                                   { pill = svc_state_str(s->state); tone = TONE_NEUTRAL; }
    int pw = ui_pill(dc, right, cy, pill, tone, 1);
    right -= pw + ui_px(12);
    if (s->present) {
        char start[48];
        _snprintf(start, sizeof(start), "Startup: %s", svc_start_type_str(s->start));
        start[sizeof(start) - 1] = '\0';
        RECT sr = { right - ui_px(140), rc.top, right, rc.bottom };
        ui_text(dc, start, &sr, FONT_SMALL, g_ui.text_dim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        right = sr.left - ui_px(8);
    }

    int tx = d.right + ui_px(18);
    HDC mdc = dc;
    int nw = ui_text_width(mdc, s->display, FONT_BODY_BOLD);
    RECT nr = { tx, rc.top + ui_px(6), right, cy + ui_px(1) };
    ui_text(dc, s->display, &nr, FONT_BODY_BOLD, s->present ? g_ui.text : g_ui.text_dim,
            DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (_stricmp(s->display, KNOWN_SERVICES[i].service_name) != 0 && tx + nw + ui_px(10) < right) {
        RECT kr = { tx + nw + ui_px(8), rc.top + ui_px(6), right, cy + ui_px(1) };
        ui_text(dc, KNOWN_SERVICES[i].service_name, &kr, FONT_SMALL, g_ui.text_faint,
                DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    RECT note = { tx, cy + ui_px(3), right, rc.bottom - ui_px(4) };
    ui_text(dc, KNOWN_SERVICES[i].note, &note, FONT_SMALL, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void svc_paint(HDC dc, const RECT *client) {
    (void)client;
    int pad = ui_px(28);
    RECT title = { pad, pad - ui_px(4), g_width / 2, pad + ui_px(34) };
    ui_text(dc, "Services", &title, FONT_TITLE, g_ui.text, DT_LEFT | DT_TOP | DT_SINGLELINE);
    int running, disabled, present;
    summary(&running, &disabled, &present);
    char sub[200];
    _snprintf(sub, sizeof(sub), "Background services most gaming PCs don't need \xC2\xB7 %d running \xC2\xB7 %d disabled",
              running, disabled);
    sub[sizeof(sub) - 1] = '\0';
    RECT sr = { pad, title.bottom + ui_px(4), g_width - pad, title.bottom + ui_px(26) };
    ui_text(dc, sub, &sr, FONT_BODY, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    ui_search_paint(dc, g_search);
    ui_card(dc, &g_list_card);
}

/* ---- page callbacks ---- */

static void svc_layout(int w, int h) {
    g_width = w;
    int pad = ui_px(28), bh = ui_px(36);
    int x = w - pad;
    int rw = ui_px(36);
    MoveWindow(g_refresh, x - rw, pad, rw, bh, TRUE);
    x -= rw + ui_px(8);
    int ew = ui_button_width(g_enable);
    if (ew < ui_px(96)) ew = ui_px(96);
    MoveWindow(g_enable, x - ew, pad, ew, bh, TRUE);
    x -= ew + ui_px(8);
    int dw = ui_button_width(g_disable);
    if (dw < ui_px(104)) dw = ui_px(104);
    MoveWindow(g_disable, x - dw, pad, dw, bh, TRUE);

    int ty = pad + ui_px(76);
    int chips_w = 0;
    for (int f = 0; f < FILTER_COUNT; f++) chips_w += ui_button_width(g_chips[f]) + ui_px(8);
    int sw = (w - pad * 2) - chips_w - ui_px(16);
    if (sw > ui_px(300)) sw = ui_px(300);
    if (sw < ui_px(160)) sw = ui_px(160);
    ui_search_place(g_search, pad, ty, sw, bh);
    int cx = pad + sw + ui_px(16), ch = ui_px(30);
    for (int f = 0; f < FILTER_COUNT; f++) {
        int cw = ui_button_width(g_chips[f]);
        MoveWindow(g_chips[f], cx, ty + (bh - ch) / 2, cw, ch, TRUE);
        cx += cw + ui_px(8);
    }
    SetRect(&g_list_card, pad, ty + bh + ui_px(16), w - pad, h - ui_px(20));
    int inset = ui_px(6);
    MoveWindow(g_list, g_list_card.left + inset, g_list_card.top + inset,
               g_list_card.right - g_list_card.left - inset * 2, g_list_card.bottom - g_list_card.top - inset * 2, TRUE);
}

static void svc_create(HWND panel) {
    g_panel = panel;
    g_list = ui_list(panel, ID_LIST, 0, ROW_HEIGHT, 1);
    g_search = ui_search(panel, ID_SEARCH, "Search services");
    for (int f = 0; f < FILTER_COUNT; f++) g_chips[f] = ui_button(panel, ID_CHIP_BASE + f, BTN_CHIP, 0, FILTER_LABELS[f]);
    ui_button_set_checked(g_chips[FILTER_ALL], 1);
    g_disable = ui_button(panel, ID_DISABLE, BTN_DANGER, 0, "Disable");
    g_enable = ui_button(panel, ID_ENABLE, BTN_SECONDARY, 0, "Enable");
    g_refresh = ui_button(panel, ID_REFRESH, BTN_ICON, GLYPH_REFRESH, "");
    EnableWindow(g_disable, FALSE);
    EnableWindow(g_enable, FALSE);
}

static void svc_show(void) { reload(); }

static int svc_command(WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    int id = LOWORD(wparam);
    if (id == ID_SEARCH && HIWORD(wparam) == EN_CHANGE) {
        ui_search_text(g_search, g_query, sizeof(g_query));
        build_rows();
        return 1;
    }
    if (id >= ID_CHIP_BASE && id < ID_CHIP_BASE + FILTER_COUNT) {
        g_filter = id - ID_CHIP_BASE;
        for (int f = 0; f < FILTER_COUNT; f++) ui_button_set_checked(g_chips[f], f == g_filter);
        build_rows();
        return 1;
    }
    switch (id) {
        case ID_DISABLE: change_selected(1, -1); return 1;
        case ID_ENABLE:  change_selected(0, -1); return 1;
        case ID_REFRESH: reload(); return 1;
    }
    return 0;
}

static void toggle_row(int row) {
    if (row < 0 || row >= g_row_count) return;
    const SvcRow *s = &g_svc[g_rows[row]];
    if (!s->present) return;
    change_selected(s->start != SERVICE_DISABLED, row);
}

static int svc_notify(LPNMHDR hdr, LRESULT *res) {
    if (hdr->hwndFrom != g_list) return 0;
    switch (hdr->code) {
        case NM_CUSTOMDRAW: {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)hdr;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) { *res = CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT; return 1; }
            if (cd->nmcd.dwDrawStage == CDDS_POSTPAINT) {
                ui_list_paint_tail(cd->nmcd.hdr.hwndFrom, cd->nmcd.hdc);
                *res = CDRF_DODEFAULT;
                return 1;
            }
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int row = (int)cd->nmcd.dwItemSpec;
                if (row >= 0 && row < g_row_count) draw_row(cd->nmcd.hdc, row);
                *res = CDRF_SKIPDEFAULT;
                return 1;
            }
            return 0;
        }
        case LVN_GETDISPINFOW: {
            NMLVDISPINFOW *di = (NMLVDISPINFOW *)hdr;
            int row = di->item.iItem;
            if ((di->item.mask & LVIF_TEXT) && row >= 0 && row < g_row_count && di->item.cchTextMax > 0) {
                ui_widen(g_svc[g_rows[row]].display, di->item.pszText, di->item.cchTextMax);
            }
            return 1;
        }
        case LVN_ODFINDITEMW:
            *res = -1;
            return 1;
        case NM_DBLCLK:
            toggle_row(((NMITEMACTIVATE *)hdr)->iItem);
            return 1;
        case NM_RETURN:
            toggle_row(ui_list_selected(g_list));
            return 1;
        case LVN_ITEMCHANGED:
        case LVN_ODSTATECHANGED:
            update_buttons();
            return 1;
        case NM_SETFOCUS:
        case NM_KILLFOCUS:
            InvalidateRect(g_list, NULL, FALSE);
            return 0;
    }
    return 0;
}

static void svc_context(HWND from, int x, int y) {
    if (from != g_list) return;
    int row = ui_list_selected(g_list);
    if (row < 0) return;
    if (x == -1 && y == -1) {
        RECT rc;
        ui_list_row_rect(g_list, row, &rc);
        POINT p = { rc.left + ui_px(60), rc.bottom };
        ClientToScreen(g_list, &p);
        x = p.x;
        y = p.y;
    }
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (IsWindowEnabled(g_disable) ? 0 : MF_GRAYED), 1, L"Stop and disable");
    AppendMenuW(m, MF_STRING | (IsWindowEnabled(g_enable) ? 0 : MF_GRAYED), 2, L"Enable");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"Copy service name");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, g_panel, NULL);
    DestroyMenu(m);
    if (cmd == 1) change_selected(1, -1);
    else if (cmd == 2) change_selected(0, -1);
    else if (cmd == 3 && ui_copy_text(gui_main_window(), KNOWN_SERVICES[g_rows[row]].service_name) == 0) {
        gui_toast(TONE_GOOD, "Copied to the clipboard.", NULL, NULL);
    }
}

static void svc_focus_search(void) {
    SetFocus(g_search);
    SendMessageW(g_search, EM_SETSEL, 0, -1);
}

static int svc_badge(char *buf, size_t size) {
    if (!g_names_loaded) query_all();
    int running, disabled, present;
    summary(&running, &disabled, &present);
    if (running == 0) return 0;
    _snprintf(buf, size, "%d on", running);
    buf[size - 1] = '\0';
    return 1;
}

GuiPage g_page_services = {
    "Services", GLYPH_SERVICES, svc_create, svc_layout, svc_paint, svc_show, NULL,
    svc_command, svc_notify, NULL, svc_context, reload, reload, svc_focus_search, svc_badge
};
