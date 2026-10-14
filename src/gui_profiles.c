#include "gui.h"
#include "profile.h"
#include <shellapi.h>
#include <ctype.h>

#define ID_LIST     600
#define ID_CONTENTS 601
#define ID_SAVE     602
#define ID_APPLY    603
#define ID_DELETE   604
#define ID_FOLDER   605

typedef struct {
    char name[PROFILE_NAME_MAX];
    int processes;
    int services;
    FILETIME saved;
} ProfileItem;

static HWND g_panel, g_list, g_contents, g_save, g_apply, g_delete, g_folder;
static ProfileItem g_items[PROFILE_MAX_COUNT];
static int g_item_count;
static ProfileContents g_detail;
static int g_detail_index = -1;
static RECT g_list_card, g_detail_card;
static int g_width, g_contents_top;

/* ---- data ---- */

static void format_saved(const FILETIME *ft, char *out, size_t size) {
    if (!ft->dwHighDateTime && !ft->dwLowDateTime) {
        _snprintf(out, size, "unknown date");
        out[size - 1] = '\0';
        return;
    }
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(ft, &local);
    FileTimeToSystemTime(&local, &st);
    char date[64], time[32];
    GetDateFormatA(LOCALE_USER_DEFAULT, 0, &st, "MMM d, yyyy", date, sizeof(date));
    GetTimeFormatA(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, time, sizeof(time));
    _snprintf(out, size, "%s at %s", date, time);
    out[size - 1] = '\0';
}

static int selected(void) {
    int row = ui_list_selected(g_list);
    return (row >= 0 && row < g_item_count) ? row : -1;
}

static void load_detail(void) {
    int sel = selected();
    g_detail_index = -1;
    memset(&g_detail, 0, sizeof(g_detail));
    if (sel >= 0 && profile_read(g_items[sel].name, &g_detail) == 0) g_detail_index = sel;

    int n = g_detail_index >= 0 ? g_detail.process_count + g_detail.service_count : 0;
    ListView_SetItemCountEx(g_contents, n, 0);
    InvalidateRect(g_contents, NULL, FALSE);

    int show = g_detail_index >= 0;
    ShowWindow(g_apply, show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_delete, show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_contents, show ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_panel, NULL, FALSE);
}

static void reload_list(const char *select_name) {
    static char names[PROFILE_MAX_COUNT][PROFILE_NAME_MAX];
    static ProfileContents scratch;
    char keep[PROFILE_NAME_MAX] = "";
    if (select_name) strncpy(keep, select_name, sizeof(keep) - 1);
    else if (selected() >= 0) strncpy(keep, g_items[selected()].name, sizeof(keep) - 1);

    g_item_count = profile_list(names, PROFILE_MAX_COUNT);
    for (int i = 0; i < g_item_count; i++) {
        ProfileItem *it = &g_items[i];
        memset(it, 0, sizeof(*it));
        memcpy(it->name, names[i], sizeof(it->name));   /* both PROFILE_NAME_MAX, NUL-terminated */
        it->name[sizeof(it->name) - 1] = '\0';
        if (profile_read(it->name, &scratch) == 0) {
            it->processes = scratch.process_count;
            it->services = scratch.service_count;
            it->saved = scratch.saved;
        }
    }
    ListView_SetItemCountEx(g_list, g_item_count, LVSICF_NOSCROLL);
    int sel = g_item_count > 0 ? 0 : -1;
    for (int i = 0; i < g_item_count; i++) {
        if (_stricmp(g_items[i].name, keep) == 0) { sel = i; break; }
    }
    ui_list_select(g_list, sel);
    ui_list_set_empty(g_list, "No profiles yet.");
    InvalidateRect(g_list, NULL, FALSE);
    load_detail();

    char msg[96];
    _snprintf(msg, sizeof(msg), "%d saved profile%s", g_item_count, g_item_count == 1 ? "" : "s");
    msg[sizeof(msg) - 1] = '\0';
    gui_status(msg);
}

/* ---- actions ---- */

static void do_save(void) {
    char name[PROFILE_NAME_MAX] = "Gaming";
    if (g_item_count > 0) name[0] = '\0';
    for (;;) {
        UiDialog d;
        memset(&d, 0, sizeof(d));
        d.heading = "Save running apps as a profile";
        d.body = "Everything running right now is recorded. Applying the profile later closes anything that "
                 "wasn't running at this moment \xE2\x80\x94 core Windows and security processes are never touched.";
        d.input = name;
        d.input_size = sizeof(name);
        d.input_cue = "Profile name, e.g. Gaming";
        d.ok = "Save profile";
        d.cancel = "Cancel";
        d.tone = TONE_ACCENT;
        d.glyph = GLYPH_SAVE;
        if (!ui_dialog(gui_main_window(), &d)) return;
        trim_newline(name);
        if (!profile_name_is_valid(name)) {
            ui_message(gui_main_window(), TONE_WARN, "That name won't work",
                       "Use letters, numbers, spaces, hyphens and underscores only (up to 63 characters).");
            continue;
        }
        if (profile_exists(name)) {
            char body[200];
            _snprintf(body, sizeof(body), "A profile named \xE2\x80\x9C%s\xE2\x80\x9D already exists. Replace it with what's running now?", name);
            body[sizeof(body) - 1] = '\0';
            UiDialog c;
            memset(&c, 0, sizeof(c));
            c.heading = "Replace profile?";
            c.body = body;
            c.ok = "Replace";
            c.cancel = "Choose another name";
            c.danger = 1;
            c.tone = TONE_WARN;
            c.glyph = GLYPH_WARNING;
            if (!ui_dialog(gui_main_window(), &c)) continue;
        }
        break;
    }

    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int rc = profile_save(name);
    SetCursor(old);
    char msg[160];
    if (rc == 0) {
        _snprintf(msg, sizeof(msg), "Saved \xE2\x80\x9C%s\xE2\x80\x9D from everything running right now.", name);
        msg[sizeof(msg) - 1] = '\0';
        gui_toast(TONE_GOOD, msg, NULL, NULL);
    } else {
        gui_toast(TONE_BAD, "Couldn't save that profile.", NULL, NULL);
    }
    reload_list(name);
}

typedef struct {
    ProfileDiff diff;
    ProfileApplyResult result;
    char name[PROFILE_NAME_MAX];
} ApplyJob;

static void apply_work(GuiJob *job) {
    ApplyJob *a = (ApplyJob *)job->ctx;
    gui_job_step(job, 0, 1, "Closing apps that aren't in the profile\xE2\x80\xA6");
    profile_execute_diff(&a->diff, &a->result);
}

static void apply_done(GuiJob *job) {
    ApplyJob *a = (ApplyJob *)job->ctx;
    ProfileApplyResult *r = &a->result;
    char msg[240];
    int n = _snprintf(msg, sizeof(msg), "Applied \xE2\x80\x9C%s\xE2\x80\x9D: closed %d app%s", a->name,
                      r->processes_closed, r->processes_closed == 1 ? "" : "s");
    if (a->diff.services_to_stop_count && n > 0) {
        n += _snprintf(msg + n, sizeof(msg) - n, ", stopped %d service%s", r->services_stopped, r->services_stopped == 1 ? "" : "s");
    }
    if ((r->processes_failed || r->services_failed) && n > 0) {
        _snprintf(msg + n, sizeof(msg) - n, " \xC2\xB7 %d couldn't be closed%s", r->processes_failed + r->services_failed,
                  is_running_elevated() ? "" : " (need administrator)");
    }
    msg[sizeof(msg) - 1] = '\0';
    gui_toast(r->processes_failed || r->services_failed ? TONE_WARN : TONE_GOOD, msg, NULL, NULL);
}

static void do_apply(void) {
    int sel = selected();
    if (sel < 0) return;
    ApplyJob *a = (ApplyJob *)calloc(1, sizeof(ApplyJob));
    if (!a) return;
    strncpy(a->name, g_items[sel].name, sizeof(a->name) - 1);

    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int rc = profile_compute_diff(a->name, &a->diff);
    SetCursor(old);
    if (rc != 0) {
        gui_toast(TONE_BAD, "Couldn't read that profile.", NULL, NULL);
        free(a);
        return;
    }
    ProfileDiff *d = &a->diff;
    if (d->procs_to_close_count == 0 && d->services_to_stop_count == 0) {
        gui_toast(TONE_GOOD, "Nothing to close \xE2\x80\x94 what's running already matches this profile.", NULL, NULL);
        free(a);
        return;
    }

    size_t cap = 8192, len = 0;
    char *details = (char *)malloc(cap);
    if (!details) { free(a); return; }
    details[0] = '\0';
    for (int i = 0; i < d->procs_to_close_count && len < cap - 1; i++) {
        int w = _snprintf(details + len, cap - len, "%s%s (PID %lu)", len ? "\r\n" : "",
                          d->procs_to_close[i].name, (unsigned long)d->procs_to_close[i].pid);
        if (w > 0) len += (size_t)w; else len = cap - 1;
    }
    for (int i = 0; i < d->services_to_stop_count && len < cap - 1; i++) {
        int w = _snprintf(details + len, cap - len, "%sService: %s (stop and disable)", len ? "\r\n" : "",
                          d->services_to_stop[i]);
        if (w > 0) len += (size_t)w; else len = cap - 1;
    }
    details[cap - 1] = '\0';

    char heading[128], body[400], ok[48];
    int total = d->procs_to_close_count + d->services_to_stop_count;
    _snprintf(heading, sizeof(heading), "Apply \xE2\x80\x9C%s\xE2\x80\x9D?", a->name);
    int n = _snprintf(body, sizeof(body), "This closes %d app%s", d->procs_to_close_count, d->procs_to_close_count == 1 ? "" : "s");
    if (d->services_to_stop_count && n > 0) {
        n += _snprintf(body + n, sizeof(body) - n, " and stops %d service%s", d->services_to_stop_count,
                       d->services_to_stop_count == 1 ? "" : "s");
    }
    if (n > 0) n += _snprintf(body + n, sizeof(body) - n, " that weren't running when you saved it. Anything unsaved in them will be lost.");
    if (d->procs_protected_count && n > 0) {
        _snprintf(body + n, sizeof(body) - n, "\n\n%d core Windows and security process%s will be left alone.",
                  d->procs_protected_count, d->procs_protected_count == 1 ? "" : "es");
    }
    _snprintf(ok, sizeof(ok), "Close %d", total);
    heading[sizeof(heading) - 1] = body[sizeof(body) - 1] = ok[sizeof(ok) - 1] = '\0';

    UiDialog dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.heading = heading;
    dlg.body = body;
    dlg.details = details;
    dlg.ok = ok;
    dlg.cancel = "Cancel";
    dlg.danger = 1;
    dlg.tone = TONE_WARN;
    dlg.glyph = GLYPH_PROFILES;
    int go = ui_dialog(gui_main_window(), &dlg);
    free(details);
    if (!go) {
        free(a);
        return;
    }
    char label[128];
    _snprintf(label, sizeof(label), "Applying %s", a->name);
    label[sizeof(label) - 1] = '\0';
    gui_job_start(label, apply_work, apply_done, a);
}

static void do_delete(void) {
    int sel = selected();
    if (sel < 0) return;
    char heading[128];
    _snprintf(heading, sizeof(heading), "Delete \xE2\x80\x9C%s\xE2\x80\x9D?", g_items[sel].name);
    heading[sizeof(heading) - 1] = '\0';
    UiDialog d;
    memset(&d, 0, sizeof(d));
    d.heading = heading;
    d.body = "The saved snapshot is removed. Nothing running on your PC changes.";
    d.ok = "Delete";
    d.cancel = "Cancel";
    d.danger = 1;
    d.tone = TONE_WARN;
    d.glyph = GLYPH_DELETE;
    if (!ui_dialog(gui_main_window(), &d)) return;
    if (profile_delete(g_items[sel].name) == 0) gui_toast(TONE_GOOD, "Profile deleted.", NULL, NULL);
    else gui_toast(TONE_BAD, "Couldn't delete that profile.", NULL, NULL);
    reload_list(NULL);
}

static void open_folder(void) {
    char dir[MAX_PATH];
    profile_dir(dir, sizeof(dir));
    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, dir, -1, w, MAX_PATH);
    ShellExecuteW(gui_main_window(), L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- painting ---- */

static void draw_profile_row(HDC dc, int row) {
    RECT rc;
    ui_list_row_rect(g_list, row, &rc);
    ui_list_row_bg(dc, g_list, &rc, row);
    const ProfileItem *it = &g_items[row];
    RECT client;
    GetClientRect(g_list, &client);
    int cy = (rc.top + rc.bottom) / 2;
    RECT ic = { client.left + ui_px(18), cy - ui_px(16), client.left + ui_px(50), cy + ui_px(16) };
    ui_round_fill(dc, &ic, ui_px(8), g_ui.accent_soft);
    char initial[2] = { (char)toupper((unsigned char)it->name[0]), 0 };
    ui_text(dc, initial, &ic, FONT_BODY_BOLD, g_ui.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int tx = ic.right + ui_px(14);
    RECT nr = { tx, rc.top + ui_px(6), client.right - ui_px(16), cy + ui_px(1) };
    ui_text(dc, it->name, &nr, FONT_BODY_BOLD, g_ui.text, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
    char date[96], meta[200];
    format_saved(&it->saved, date, sizeof(date));
    _snprintf(meta, sizeof(meta), "%d apps \xC2\xB7 %s", it->processes, date);
    meta[sizeof(meta) - 1] = '\0';
    RECT mr = { tx, cy + ui_px(3), client.right - ui_px(16), rc.bottom - ui_px(4) };
    ui_text(dc, meta, &mr, FONT_SMALL, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void draw_content_row(HDC dc, int row) {
    RECT rc;
    ui_list_row_rect(g_contents, row, &rc);
    ui_list_row_bg(dc, g_contents, &rc, row);
    RECT client;
    GetClientRect(g_contents, &client);
    int is_service = row >= g_detail.process_count;
    const char *name = is_service ? g_detail.services[row - g_detail.process_count] : g_detail.processes[row];
    RECT t = { client.left + ui_px(16), rc.top, client.right - ui_px(100), rc.bottom };
    ui_text(dc, name, &t, FONT_BODY, g_ui.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (is_service) ui_pill(dc, client.right - ui_px(16), (rc.top + rc.bottom) / 2, "Service", TONE_ACCENT, 1);
    else if (proc_is_protected(name)) ui_pill(dc, client.right - ui_px(16), (rc.top + rc.bottom) / 2, "System", TONE_NEUTRAL, 1);
}

static void prof_paint(HDC dc, const RECT *client) {
    (void)client;
    int pad = ui_px(28);
    RECT title = { pad, pad - ui_px(4), g_width / 2, pad + ui_px(34) };
    ui_text(dc, "Profiles", &title, FONT_TITLE, g_ui.text, DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT sr = { pad, title.bottom + ui_px(4), g_width - pad, title.bottom + ui_px(26) };
    ui_text(dc, "Snapshot the apps you want running, then close everything else in one step.",
            &sr, FONT_BODY, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    ui_card(dc, &g_list_card);
    ui_card(dc, &g_detail_card);
    RECT d = g_detail_card;
    int x = d.left + ui_px(24), right = d.right - ui_px(24);

    if (g_detail_index < 0) {
        int y = d.top + ui_px(28);
        RECT h = { x, y, right, y + ui_px(26) };
        ui_text(dc, g_item_count ? "Select a profile" : "How profiles work", &h, FONT_HEADING, g_ui.text,
                DT_LEFT | DT_TOP | DT_SINGLELINE);
        y = h.bottom + ui_px(14);
        static const char *steps[3] = {
            "Open your game and every app you want to keep \xE2\x80\x94 Discord, your browser, whatever you use.",
            "Click \xE2\x80\x9CSave running apps\xE2\x80\x9D and name it, e.g. Gaming.",
            "Next time, apply the profile to close everything else in one step. Core Windows and security "
            "processes are never closed.",
        };
        for (int i = 0; i < 3; i++) {
            RECT num = { x, y, x + ui_px(26), y + ui_px(26) };
            ui_ellipse(dc, &num, g_ui.accent_soft);
            char n[4];
            _snprintf(n, sizeof(n), "%d", i + 1);
            n[sizeof(n) - 1] = '\0';
            ui_text(dc, n, &num, FONT_SMALL_BOLD, g_ui.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            int tw = right - (num.right + ui_px(12));
            int th = ui_text_height(dc, steps[i], FONT_BODY, tw);
            RECT t = { num.right + ui_px(12), y + ui_px(3), right, y + ui_px(3) + th };
            ui_text(dc, steps[i], &t, FONT_BODY, g_ui.text_dim, DT_LEFT | DT_TOP | DT_WORDBREAK);
            y += (th > ui_px(26) ? th : ui_px(26)) + ui_px(16);
        }
        return;
    }

    const ProfileItem *it = &g_items[g_detail_index];
    RECT h = { x, d.top + ui_px(22), right, d.top + ui_px(52) };
    ui_text(dc, it->name, &h, FONT_TITLE, g_ui.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    char date[96], meta[240];
    format_saved(&g_detail.saved, date, sizeof(date));
    _snprintf(meta, sizeof(meta), "Saved %s \xC2\xB7 %d apps \xC2\xB7 %d service%s", date, g_detail.process_count,
              g_detail.service_count, g_detail.service_count == 1 ? "" : "s");
    meta[sizeof(meta) - 1] = '\0';
    RECT m = { x, h.bottom + ui_px(4), right, h.bottom + ui_px(24) };
    ui_text(dc, meta, &m, FONT_SMALL, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT l = { x, g_contents_top - ui_px(24), right, g_contents_top - ui_px(6) };
    ui_text(dc, "KEEPS RUNNING", &l, FONT_SMALL_BOLD, g_ui.text_dim, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
}

/* ---- layout & lifecycle ---- */

static void prof_layout(int w, int h) {
    g_width = w;
    int pad = ui_px(28), bh = ui_px(36), gap = ui_px(16);
    MoveWindow(g_folder, w - pad - bh, pad, bh, bh, TRUE);
    int sw = ui_button_width(g_save);
    MoveWindow(g_save, w - pad - bh - ui_px(8) - sw, pad, sw, bh, TRUE);

    int top = pad + ui_px(76), bottom = h - ui_px(20);
    int inner = w - pad * 2;
    int lw = inner * 38 / 100;
    if (lw < ui_px(300)) lw = ui_px(300);
    SetRect(&g_list_card, pad, top, pad + lw, bottom);
    SetRect(&g_detail_card, g_list_card.right + gap, top, w - pad, bottom);
    int inset = ui_px(6);
    MoveWindow(g_list, g_list_card.left + inset, g_list_card.top + inset,
               g_list_card.right - g_list_card.left - inset * 2, g_list_card.bottom - g_list_card.top - inset * 2, TRUE);

    /* Actions sit under the name so long profile names get the full width. */
    int dw = ui_button_width(g_delete);
    int aw = ui_button_width(g_apply);
    int by = g_detail_card.top + ui_px(88);
    MoveWindow(g_apply, g_detail_card.left + ui_px(24), by, aw, bh, TRUE);
    MoveWindow(g_delete, g_detail_card.left + ui_px(24) + aw + ui_px(8), by, dw, bh, TRUE);

    g_contents_top = by + bh + ui_px(44);
    MoveWindow(g_contents, g_detail_card.left + ui_px(12), g_contents_top,
               g_detail_card.right - g_detail_card.left - ui_px(24), g_detail_card.bottom - g_contents_top - ui_px(12), TRUE);
}

static void prof_create(HWND panel) {
    g_panel = panel;
    g_list = ui_list(panel, ID_LIST, LVS_SINGLESEL, 62, 1);
    g_contents = ui_list(panel, ID_CONTENTS, LVS_SINGLESEL, 32, 1);
    g_save = ui_button(panel, ID_SAVE, BTN_PRIMARY, GLYPH_SAVE, "Save running apps");
    g_folder = ui_button(panel, ID_FOLDER, BTN_ICON, GLYPH_FOLDER, "");
    g_apply = ui_button(panel, ID_APPLY, BTN_PRIMARY, GLYPH_PLAY, "Apply profile");
    g_delete = ui_button(panel, ID_DELETE, BTN_DANGER, GLYPH_DELETE, "Delete");
    ui_button_set_backdrop(g_apply, BACK_SURFACE);
    ui_button_set_backdrop(g_delete, BACK_SURFACE);
    ShowWindow(g_apply, SW_HIDE);
    ShowWindow(g_delete, SW_HIDE);
    ShowWindow(g_contents, SW_HIDE);
    static char names[PROFILE_MAX_COUNT][PROFILE_NAME_MAX];
    g_item_count = profile_list(names, PROFILE_MAX_COUNT);   /* for the sidebar badge */
}

static void prof_show(void) { reload_list(NULL); }

static int prof_command(WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    switch (LOWORD(wparam)) {
        case ID_SAVE:   do_save(); return 1;
        case ID_APPLY:  do_apply(); return 1;
        case ID_DELETE: do_delete(); return 1;
        case ID_FOLDER: open_folder(); return 1;
    }
    return 0;
}

static int custom_draw(NMLVCUSTOMDRAW *cd, LRESULT *res, int contents) {
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) { *res = CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT; return 1; }
    if (cd->nmcd.dwDrawStage == CDDS_POSTPAINT) {
        ui_list_paint_tail(cd->nmcd.hdr.hwndFrom, cd->nmcd.hdc);
        *res = CDRF_DODEFAULT;
        return 1;
    }
    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        int row = (int)cd->nmcd.dwItemSpec;
        if (contents) {
            if (row < g_detail.process_count + g_detail.service_count) draw_content_row(cd->nmcd.hdc, row);
        } else if (row < g_item_count) {
            draw_profile_row(cd->nmcd.hdc, row);
        }
        *res = CDRF_SKIPDEFAULT;
        return 1;
    }
    return 0;
}

static int prof_notify(LPNMHDR hdr, LRESULT *res) {
    if (hdr->hwndFrom == g_contents) {
        if (hdr->code == NM_CUSTOMDRAW) return custom_draw((NMLVCUSTOMDRAW *)hdr, res, 1);
        if (hdr->code == LVN_ODFINDITEMW) { *res = -1; return 1; }
        return 0;
    }
    if (hdr->hwndFrom != g_list) return 0;
    switch (hdr->code) {
        case NM_CUSTOMDRAW:
            return custom_draw((NMLVCUSTOMDRAW *)hdr, res, 0);
        case LVN_GETDISPINFOW: {
            NMLVDISPINFOW *di = (NMLVDISPINFOW *)hdr;
            int row = di->item.iItem;
            if ((di->item.mask & LVIF_TEXT) && row >= 0 && row < g_item_count && di->item.cchTextMax > 0) {
                ui_widen(g_items[row].name, di->item.pszText, di->item.cchTextMax);
            }
            return 1;
        }
        case LVN_ODFINDITEMW:
            *res = -1;
            return 1;
        case LVN_ITEMCHANGED:
            if (selected() != g_detail_index) load_detail();
            return 1;
        case NM_DBLCLK:
        case NM_RETURN:
            do_apply();
            return 1;
        case LVN_KEYDOWN:
            if (((NMLVKEYDOWN *)hdr)->wVKey == VK_DELETE) { do_delete(); return 1; }
            return 0;
        case NM_SETFOCUS:
        case NM_KILLFOCUS:
            InvalidateRect(g_list, NULL, FALSE);
            return 0;
    }
    return 0;
}

static void prof_context(HWND from, int x, int y) {
    if (from != g_list || selected() < 0) return;
    if (x == -1 && y == -1) {
        RECT rc;
        ui_list_row_rect(g_list, selected(), &rc);
        POINT p = { rc.left + ui_px(60), rc.bottom };
        ClientToScreen(g_list, &p);
        x = p.x;
        y = p.y;
    }
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Apply\tEnter");
    AppendMenuW(m, MF_STRING, 2, L"Delete\tDel");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"Open profiles folder");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, g_panel, NULL);
    DestroyMenu(m);
    if (cmd == 1) do_apply();
    else if (cmd == 2) do_delete();
    else if (cmd == 3) open_folder();
}

static void prof_reload(void) { reload_list(NULL); }

static int prof_badge(char *buf, size_t size) {
    if (g_item_count == 0) return 0;
    _snprintf(buf, size, "%d", g_item_count);
    buf[size - 1] = '\0';
    return 1;
}

GuiPage g_page_profiles = {
    "Profiles", GLYPH_PROFILES, prof_create, prof_layout, prof_paint, prof_show, NULL,
    prof_command, prof_notify, NULL, prof_context, NULL, prof_reload, NULL, prof_badge
};
