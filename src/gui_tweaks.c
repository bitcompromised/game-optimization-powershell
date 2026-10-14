#include "gui.h"
#include <windowsx.h>
#include <ctype.h>

/* Performance and Privacy share this implementation: a grouped, searchable list of switches
 * with a detail card for the selected tweak. */

#define ID_LIST      200
#define ID_SEARCH    201
#define ID_APPLY     202
#define ID_REVERT    203
#define ID_DETAIL    204
#define ID_CHIP_BASE 210

#define ROW_HEIGHT 58
#define MAX_ROWS   192

typedef struct {
    const char *name;
    const char *ids;    /* space-separated tweak ids */
} TweakGroup;

enum { ROW_GROUP, ROW_TWEAK };
typedef struct { int kind; int value; } Row;

enum { FILTER_ALL, FILTER_ON, FILTER_OFF, FILTER_RESTART, FILTER_ADMIN, FILTER_COUNT };
static const char *FILTER_LABELS[FILTER_COUNT] = { "All", "On", "Off", "Needs restart", "Needs admin" };

typedef struct {
    TweakCategory cat;
    const char *title;
    const char *noun;             /* "performance tweaks" */
    const char *apply_label, *revert_label;
    const char *apply_heading, *revert_heading;
    WCHAR apply_glyph;
    const TweakGroup *groups;
    int group_count;              /* the implicit "Other" group is index group_count */

    HWND panel, list, search, apply_btn, revert_btn, detail_btn;
    HWND chips[FILTER_COUNT];
    int has_restart_tweaks;
    int group_of[128];
    Row rows[MAX_ROWS];
    int row_count;
    int filter;
    char query[128];
    unsigned collapsed;
    int width, height, toolbar_y;
    int compact;                  /* no detail card: the window is too short for it */
    RECT list_card, detail_card;
} TweakPage;

static const TweakGroup PERF_GROUPS[] = {
    { "Power & CPU", "power_high_perf power_ultimate boost_mode core_parking_off power_throttling_off "
                     "usb_suspend_off hibernate_off" },
    { "Gaming", "game_mode disable_gamedvr game_priority valorant_priority fso_off toasts_off" },
    { "Graphics & display", "hags mpo_off visual_effects" },
    { "Network", "network_throttle nagle_off nic_power_off nic_interrupt_mod_off" },
    { "Input", "mouse_accel_off accessibility_hotkeys_off" },
    { "Timers & boot", "dynamic_tick_off platform_clock_off startup_delay" },
    { "Background activity", "disable_sysmain disable_search_index background_apps driver_search_off "
                             "widgets_off auto_maintenance_off pca_off edge_background_off" },
};

static const TweakGroup PRIVACY_GROUPS[] = {
    { "Telemetry & diagnostics", "disable_diagtrack autologger_off telemetry_level dmwappush_off "
                                 "telemetry_tasks ceip_off error_reporting app_diagnostics_off "
                                 "feedback_notifications handwriting_reports_off" },
    { "AI & voice", "recall_off copilot_off cortana voice_activation_off speech_off" },
    { "Search & Start menu", "bing_search_off cloud_search_off start_ads_off app_launch_tracking_off spotlight_off" },
    { "Ads & personalization", "advertising_id tailored_experiences inking_off language_list_off" },
    { "Sync & location", "activity_history cloud_clipboard_off settings_sync_off find_my_device_off location_tracking" },
    { "Network & sharing", "delivery_opt_off remote_assist_off wifi_sense_off" },
    { "Browser", "edge_telemetry_off" },
};

static TweakPage g_perf = {
    CAT_PERFORMANCE, "Performance", "performance tweaks",
    "Apply all", "Revert all",
    "Apply every performance tweak?", "Revert every performance tweak?",
    GLYPH_BOLT, PERF_GROUPS, (int)(sizeof(PERF_GROUPS) / sizeof(PERF_GROUPS[0])),
};

static TweakPage g_tele = {
    CAT_TELEMETRY, "Privacy", "privacy protections",
    "Block all tracking", "Restore all",
    "Turn on every privacy protection?", "Restore Windows' default tracking settings?",
    GLYPH_SHIELD, PRIVACY_GROUPS, (int)(sizeof(PRIVACY_GROUPS) / sizeof(PRIVACY_GROUPS[0])),
};

/* ---- data ---- */

static int ids_contain(const char *ids, const char *id) {
    size_t n = strlen(id);
    for (const char *p = strstr(ids, id); p; p = strstr(p + n, id)) {
        if ((p == ids || p[-1] == ' ') && (p[n] == '\0' || p[n] == ' ')) return 1;
    }
    return 0;
}

static const char *group_name(TweakPage *st, int g) {
    return g < st->group_count ? st->groups[g].name : "Other";
}

static int tweak_matches(TweakPage *st, int i) {
    const Tweak *t = &g_tweaks[i];
    if (t->category != st->cat) return 0;
    switch (st->filter) {
        case FILTER_ON:      if (!t->enabled) return 0; break;
        case FILTER_OFF:     if (t->enabled) return 0; break;
        case FILTER_RESTART: if (!t->requires_reboot) return 0; break;
        case FILTER_ADMIN:   if (!t->requires_admin) return 0; break;
    }
    if (st->query[0] && !ui_stristr(t->name, st->query) && !ui_stristr(t->description, st->query) &&
        !ui_stristr(group_name(st, st->group_of[i]), st->query)) {
        return 0;
    }
    return 1;
}

static int selected_tweak(TweakPage *st) {
    int row = ui_list_selected(st->list);
    if (row < 0 || row >= st->row_count || st->rows[row].kind != ROW_TWEAK) return -1;
    return st->rows[row].value;
}

static void update_detail_button(TweakPage *st);

static void build_rows(TweakPage *st) {
    int keep = -1, keep_group = -1;
    int sel = ui_list_selected(st->list);
    if (sel >= 0 && sel < st->row_count) {
        if (st->rows[sel].kind == ROW_TWEAK) keep = st->rows[sel].value;
        else keep_group = st->rows[sel].value;
    }

    st->row_count = 0;
    for (int g = 0; g <= st->group_count; g++) {
        int header = -1;
        for (int i = 0; i < g_tweak_count && st->row_count < MAX_ROWS; i++) {
            if (st->group_of[i] != g || !tweak_matches(st, i)) continue;
            if (header < 0) {
                header = st->row_count;
                st->rows[st->row_count++] = (Row){ ROW_GROUP, g };
                if ((st->collapsed & (1u << g)) && !st->query[0]) break;
            }
            st->rows[st->row_count++] = (Row){ ROW_TWEAK, i };
        }
    }

    ListView_SetItemCountEx(st->list, st->row_count, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);

    int restore = -1;
    for (int r = 0; r < st->row_count; r++) {
        if ((st->rows[r].kind == ROW_TWEAK && st->rows[r].value == keep) ||
            (st->rows[r].kind == ROW_GROUP && st->rows[r].value == keep_group)) {
            restore = r;
            break;
        }
    }
    ListView_SetItemState(st->list, -1, 0, LVIS_SELECTED);
    if (restore >= 0) ListView_SetItemState(st->list, restore, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

    if (!gui_ready()) {
        ui_list_set_empty(st->list, "Scanning your system \xE2\x80\x94 every switch shows the live Windows setting once it's read.");
    } else if (st->query[0]) {
        char msg[200];
        _snprintf(msg, sizeof(msg), "Nothing matches \xE2\x80\x9C%s\xE2\x80\x9D. Press Esc to clear the search.", st->query);
        msg[sizeof(msg) - 1] = '\0';
        ui_list_set_empty(st->list, msg);
    } else {
        ui_list_set_empty(st->list, "Nothing matches this filter.");
    }
    InvalidateRect(st->list, NULL, FALSE);
    update_detail_button(st);
    InvalidateRect(st->panel, NULL, FALSE);
}

static void counts(TweakPage *st, int *on, int *total, int *admin_locked, int *pending) {
    *on = *total = *admin_locked = *pending = 0;
    int elevated = is_running_elevated();
    for (int i = 0; i < g_tweak_count; i++) {
        if (g_tweaks[i].category != st->cat) continue;
        (*total)++;
        if (g_tweaks[i].enabled) (*on)++;
        if (g_tweaks[i].requires_admin && !elevated) (*admin_locked)++;
        if (gui_restart_pending_for(i)) (*pending)++;
    }
}

/* ---- actions ---- */

static void toggle_tweak(int i) {
    const Tweak *t = &g_tweaks[i];
    char label[160];
    _snprintf(label, sizeof(label), "%s %s\xE2\x80\xA6", t->enabled ? "Turning off" : "Turning on", t->name);
    label[sizeof(label) - 1] = '\0';
    gui_tweaks_set(&i, 1, -1, label);
}

static void toggle_group(TweakPage *st, int g) {
    st->collapsed ^= (1u << g);
    build_rows(st);
}

static void activate_row(TweakPage *st, int row) {
    if (row < 0 || row >= st->row_count) return;
    if (st->rows[row].kind == ROW_GROUP) toggle_group(st, st->rows[row].value);
    else toggle_tweak(st->rows[row].value);
}

static void apply_all(TweakPage *st, int want) {
    if (!gui_ready()) {
        gui_toast(TONE_NEUTRAL, "Still scanning your system \xE2\x80\x94 one moment.", NULL, NULL);
        return;
    }
    int idx[128], n = 0, runnable = 0, admin = 0, restart = 0, total = 0;
    int elevated = is_running_elevated();
    for (int i = 0; i < g_tweak_count && n < 128; i++) {
        if (g_tweaks[i].category != st->cat) continue;
        total++;
        if (g_tweaks[i].enabled == want) continue;
        idx[n++] = i;
        if (g_tweaks[i].requires_admin && !elevated) { admin++; continue; }
        runnable++;
        if (g_tweaks[i].requires_reboot) restart++;
    }
    if (n == 0) {
        char msg[160];
        _snprintf(msg, sizeof(msg), "All %d %s are already %s.", total, st->noun, want ? "on" : "off");
        msg[sizeof(msg) - 1] = '\0';
        gui_toast(TONE_GOOD, msg, NULL, NULL);
        return;
    }
    if (runnable == 0) {
        gui_offer_elevation(want ? st->apply_label : st->revert_label);
        return;
    }

    char body[600];
    int len = _snprintf(body, sizeof(body), "%d of %d %s %s currently %s and will be turned %s.",
                        runnable, total, st->noun, runnable == 1 ? "is" : "are", want ? "off" : "on", want ? "on" : "off");
    if (admin && len > 0) {
        len += _snprintf(body + len, sizeof(body) - len,
                         "\n\n%d more need administrator rights and will be skipped.", admin);
    }
    if (restart && len > 0) {
        len += _snprintf(body + len, sizeof(body) - len,
                         "\n\n%d of these only take effect after you restart Windows.", restart);
    }
    if (len > 0) {
        _snprintf(body + len, sizeof(body) - len, "\n\nYou can undo this from the status bar afterwards.");
    }
    body[sizeof(body) - 1] = '\0';

    char ok[64];
    _snprintf(ok, sizeof(ok), "Turn %s %d", want ? "on" : "off", runnable);
    ok[sizeof(ok) - 1] = '\0';

    UiDialog d;
    memset(&d, 0, sizeof(d));
    d.heading = want ? st->apply_heading : st->revert_heading;
    d.body = body;
    d.ok = ok;
    d.cancel = "Cancel";
    d.danger = !want;
    d.tone = want ? TONE_ACCENT : TONE_WARN;
    d.glyph = want ? st->apply_glyph : GLYPH_UNDO;
    if (!ui_dialog(gui_main_window(), &d)) return;

    gui_tweaks_set(idx, n, want, want ? "Applying" : "Reverting");
}

static void copy_details(int i) {
    const Tweak *t = &g_tweaks[i];
    char text[1024];
    _snprintf(text, sizeof(text), "%s\r\n%s\r\nState: %s\r\nNeeds administrator: %s\r\nNeeds restart: %s\r\nID: %s",
              t->name, t->description, t->enabled ? "On" : "Off", t->requires_admin ? "yes" : "no",
              t->requires_reboot ? "yes" : "no", t->id);
    text[sizeof(text) - 1] = '\0';
    if (ui_copy_text(gui_main_window(), text) == 0) gui_toast(TONE_GOOD, "Copied to the clipboard.", NULL, NULL);
}

/* ---- painting ---- */

static int switch_hit(HWND list, int x) {
    (void)list;
    return x >= ui_px(12) && x <= ui_px(24) + ui_px(UI_SWITCH_W) + ui_px(10);
}

static void draw_group_row(TweakPage *st, HDC dc, const RECT *rc, int row, int g) {
    ui_fill_rect(dc, rc, g_ui.surface);
    RECT band = { rc->left + ui_px(6), rc->top + ui_px(18), rc->right - ui_px(6), rc->bottom - ui_px(2) };
    RECT client;
    GetClientRect(st->list, &client);
    band.right = client.right - ui_px(6);
    int selected = (ListView_GetItemState(st->list, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    if (selected) ui_round_fill(dc, &band, ui_px(6), GetFocus() == st->list ? g_ui.accent_soft : g_ui.surface3);
    else if (ui_list_hot(st->list) == row) ui_round_fill(dc, &band, ui_px(6), g_ui.surface2);

    int collapsed = (st->collapsed & (1u << g)) && !st->query[0];
    RECT cr = { band.left + ui_px(12), band.top, band.left + ui_px(30), band.bottom };
    ui_glyph(dc, collapsed ? GLYPH_CHEVRON_RIGHT : GLYPH_CHEVRON_DOWN, &cr, FONT_ICON_SMALL, g_ui.text_dim);

    char upper[64];
    const char *name = group_name(st, g);
    size_t k = 0;
    for (; name[k] && k < sizeof(upper) - 1; k++) upper[k] = (char)toupper((unsigned char)name[k]);
    upper[k] = '\0';
    RECT tr = { cr.right + ui_px(6), band.top, band.right - ui_px(120), band.bottom };
    ui_text(dc, upper, &tr, FONT_SMALL_BOLD, g_ui.text_dim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    int on = 0, total = 0;
    for (int i = 0; i < g_tweak_count; i++) {
        if (g_tweaks[i].category != st->cat || st->group_of[i] != g) continue;
        total++;
        on += g_tweaks[i].enabled;
    }
    char count[48];
    if (gui_ready()) _snprintf(count, sizeof(count), "%d of %d on", on, total);
    else _snprintf(count, sizeof(count), "%d settings", total);
    count[sizeof(count) - 1] = '\0';
    RECT nr = { band.right - ui_px(160), band.top, band.right - ui_px(14), band.bottom };
    ui_text(dc, count, &nr, FONT_SMALL, on == total && gui_ready() ? g_ui.good : g_ui.text_faint,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void draw_tweak_row(TweakPage *st, HDC dc, const RECT *rc, int row, int i) {
    const Tweak *t = &g_tweaks[i];
    ui_list_row_bg(dc, st->list, rc, row);

    int ready = gui_ready();
    int busy = gui_tweak_in_progress() == i;
    int locked = t->requires_admin && !is_running_elevated();
    int hot = ui_list_hot(st->list) == row;
    int cy = (rc->top + rc->bottom) / 2;
    RECT client;
    GetClientRect(st->list, &client);

    int x = client.left + ui_px(24);
    ui_switch(dc, x, cy - ui_px(UI_SWITCH_H) / 2, t->enabled, ready && !locked && !busy, hot && !locked);
    if (locked) {
        RECT lr = { x + ui_px(UI_SWITCH_W) + ui_px(2), cy - ui_px(12), x + ui_px(UI_SWITCH_W) + ui_px(18), cy + ui_px(12) };
        ui_glyph(dc, GLYPH_LOCK, &lr, FONT_ICON_SMALL, g_ui.warn);
    }
    int text_x = x + ui_px(UI_SWITCH_W) + ui_px(22);

    int right = client.right - ui_px(20);
    const char *state = !ready ? "\xE2\x80\xA6" : busy ? "Working\xE2\x80\xA6" : t->enabled ? "On" : "Off";
    COLORREF state_color = busy ? g_ui.accent : t->enabled ? g_ui.good : g_ui.text_faint;
    RECT sr = { right - ui_px(64), rc->top, right, rc->bottom };
    ui_text(dc, state, &sr, FONT_SMALL_BOLD, state_color, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    right = sr.left - ui_px(4);

    if (gui_restart_pending_for(i)) {
        right -= ui_pill(dc, right, cy, "Restart pending", TONE_WARN, 1) + ui_px(6);
    } else if (t->requires_reboot) {
        right -= ui_pill(dc, right, cy, "Restart", TONE_NEUTRAL, 1) + ui_px(6);
    }
    if (t->requires_admin) {
        right -= ui_pill(dc, right, cy, "Admin", locked ? TONE_WARN : TONE_NEUTRAL, 1) + ui_px(6);
    }

    RECT nr = { text_x, rc->top + ui_px(6), right - ui_px(8), cy + ui_px(1) };
    ui_text(dc, t->name, &nr, FONT_BODY_BOLD, locked ? g_ui.text_dim : g_ui.text,
            DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT dr = { text_x, cy + ui_px(3), right - ui_px(8), rc->bottom - ui_px(4) };
    ui_text(dc, t->description, &dr, FONT_SMALL, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void page_paint(TweakPage *st, HDC dc, const RECT *client) {
    (void)client;
    int pad = ui_px(28);
    RECT title = { pad, pad - ui_px(4), st->width / 2, pad + ui_px(34) };
    ui_text(dc, st->title, &title, FONT_TITLE, g_ui.text, DT_LEFT | DT_TOP | DT_SINGLELINE);

    char sub[200];
    if (!gui_ready()) {
        _snprintf(sub, sizeof(sub), "Scanning your system\xE2\x80\xA6");
    } else {
        int on, total, locked, pending;
        counts(st, &on, &total, &locked, &pending);
        int n = _snprintf(sub, sizeof(sub), "%d of %d %s active", on, total, st->noun);
        if (locked && n > 0) n += _snprintf(sub + n, sizeof(sub) - n, " \xC2\xB7 %d need administrator", locked);
        if (pending && n > 0) _snprintf(sub + n, sizeof(sub) - n, " \xC2\xB7 %d waiting for a restart", pending);
    }
    sub[sizeof(sub) - 1] = '\0';
    RECT sr = { pad, title.bottom + ui_px(4), st->width - pad, title.bottom + ui_px(26) };
    ui_text(dc, sub, &sr, FONT_BODY, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    ui_search_paint(dc, st->search);
    ui_card(dc, &st->list_card);
    if (st->compact) return;
    ui_card(dc, &st->detail_card);

    RECT d = st->detail_card;
    int i = selected_tweak(st);
    RECT btn;
    GetWindowRect(st->detail_btn, &btn);
    MapWindowPoints(NULL, st->panel, (POINT *)&btn, 2);
    int text_right = IsWindowVisible(st->detail_btn) ? btn.left - ui_px(20) : d.right - ui_px(20);
    int x = d.left + ui_px(20);

    if (i < 0) {
        RECT r = { x, d.top, d.right - ui_px(20), d.bottom };
        ui_text(dc, "Select a tweak to see what it does. Click its switch \xE2\x80\x94 or press Space \xE2\x80\x94 to change it.",
                &r, FONT_BODY, g_ui.text_dim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    const Tweak *t = &g_tweaks[i];
    int y = d.top + ui_px(14);
    RECT nr = { x, y, text_right, y + ui_px(24) };
    ui_text(dc, t->name, &nr, FONT_HEADING, g_ui.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    int line = ui_font_height(FONT_BODY);
    RECT dr = { x, nr.bottom + ui_px(2), text_right, nr.bottom + ui_px(2) + line * 2 };
    ui_text(dc, t->description, &dr, FONT_BODY, g_ui.text_dim, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);

    char meta[256];
    int n = _snprintf(meta, sizeof(meta), "%s", !gui_ready() ? "Reading state\xE2\x80\xA6" : t->enabled ? "Currently on" : "Currently off");
    if (t->requires_admin && n > 0) {
        n += _snprintf(meta + n, sizeof(meta) - n, " \xC2\xB7 %s",
                       is_running_elevated() ? "Uses administrator rights" : "Needs administrator \xE2\x80\x94 locked");
    }
    if (t->requires_reboot && n > 0) {
        n += _snprintf(meta + n, sizeof(meta) - n, " \xC2\xB7 %s",
                       gui_restart_pending_for(i) ? "Restart pending" : "Takes effect after a restart");
    }
    if (n > 0) _snprintf(meta + n, sizeof(meta) - n, " \xC2\xB7 id: %s", t->id);
    meta[sizeof(meta) - 1] = '\0';
    RECT mr = { x, d.bottom - ui_px(14) - ui_font_height(FONT_SMALL), text_right, d.bottom - ui_px(12) };
    ui_text(dc, meta, &mr, FONT_SMALL, g_ui.text_faint, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* ---- layout ---- */

static void place_detail_button(TweakPage *st) {
    int bh = ui_px(36);
    int bw = ui_button_width(st->detail_btn);
    if (bw < ui_px(112)) bw = ui_px(112);
    RECT d = st->detail_card;
    MoveWindow(st->detail_btn, d.right - ui_px(20) - bw, d.top + (d.bottom - d.top - bh) / 2, bw, bh, TRUE);
}

static void update_detail_button(TweakPage *st) {
    int i = selected_tweak(st);
    if (i < 0 || !gui_ready() || st->compact) {
        ShowWindow(st->detail_btn, SW_HIDE);
        return;
    }
    const Tweak *t = &g_tweaks[i];
    int locked = t->requires_admin && !is_running_elevated();
    ui_button_set_text(st->detail_btn, locked ? "Unlock\xE2\x80\xA6" : t->enabled ? "Turn off" : "Turn on");
    ui_button_set_glyph(st->detail_btn, locked ? GLYPH_ADMIN : 0);
    ui_button_set_kind(st->detail_btn, t->enabled || locked ? BTN_SECONDARY : BTN_PRIMARY);
    place_detail_button(st);
    ShowWindow(st->detail_btn, SW_SHOW);
}

static void page_layout(TweakPage *st, int w, int h) {
    st->width = w;
    st->height = h;
    int pad = ui_px(28), bottom = ui_px(20), bh = ui_px(36);

    int rw = ui_button_width(st->revert_btn), aw = ui_button_width(st->apply_btn);
    int x = w - pad - rw;
    MoveWindow(st->revert_btn, x, pad, rw, bh, TRUE);
    x -= ui_px(8) + aw;
    MoveWindow(st->apply_btn, x, pad, aw, bh, TRUE);

    st->toolbar_y = pad + ui_px(76);
    int chips_w = 0;
    for (int f = 0; f < FILTER_COUNT; f++) {
        if (f == FILTER_RESTART && !st->has_restart_tweaks) continue;
        chips_w += ui_button_width(st->chips[f]) + ui_px(8);
    }
    int sw = (w - pad * 2) - chips_w - ui_px(16);
    if (sw > ui_px(300)) sw = ui_px(300);
    if (sw < ui_px(160)) sw = ui_px(160);
    ui_search_place(st->search, pad, st->toolbar_y, sw, bh);
    int cx = pad + sw + ui_px(16), ch = ui_px(30);
    for (int f = 0; f < FILTER_COUNT; f++) {
        if (f == FILTER_RESTART && !st->has_restart_tweaks) continue;
        int cw = ui_button_width(st->chips[f]);
        MoveWindow(st->chips[f], cx, st->toolbar_y + (bh - ch) / 2, cw, ch, TRUE);
        cx += cw + ui_px(8);
    }

    /* On short windows (small or high-DPI screens) the list matters more than the detail card;
     * rows already carry each tweak's name and description. */
    int list_top = st->toolbar_y + bh + ui_px(16);
    int dh = ui_px(104);
    st->compact = (h - bottom - dh - ui_px(12)) - list_top < ui_px(236);
    if (st->compact) {
        SetRectEmpty(&st->detail_card);
        SetRect(&st->list_card, pad, list_top, w - pad, h - bottom);
    } else {
        SetRect(&st->detail_card, pad, h - bottom - dh, w - pad, h - bottom);
        SetRect(&st->list_card, pad, list_top, w - pad, st->detail_card.top - ui_px(12));
    }
    int inset = ui_px(6);
    MoveWindow(st->list, st->list_card.left + inset, st->list_card.top + inset,
               st->list_card.right - st->list_card.left - inset * 2,
               st->list_card.bottom - st->list_card.top - inset * 2, TRUE);
    place_detail_button(st);
    update_detail_button(st);
}

/* ---- page callbacks ---- */

static void page_create(TweakPage *st, HWND panel) {
    st->panel = panel;
    for (int i = 0; i < g_tweak_count && i < 128; i++) {
        st->group_of[i] = st->group_count;
        for (int g = 0; g < st->group_count; g++) {
            if (ids_contain(st->groups[g].ids, g_tweaks[i].id)) { st->group_of[i] = g; break; }
        }
        if (g_tweaks[i].category == st->cat && g_tweaks[i].requires_reboot) st->has_restart_tweaks = 1;
    }

    st->list = ui_list(panel, ID_LIST, LVS_SINGLESEL, ROW_HEIGHT, 1);
    st->search = ui_search(panel, ID_SEARCH, "Search by name or what it does");
    for (int f = 0; f < FILTER_COUNT; f++) {
        st->chips[f] = ui_button(panel, ID_CHIP_BASE + f, BTN_CHIP, 0, FILTER_LABELS[f]);
        if (f == FILTER_RESTART && !st->has_restart_tweaks) ShowWindow(st->chips[f], SW_HIDE);
    }
    ui_button_set_checked(st->chips[FILTER_ALL], 1);
    st->apply_btn = ui_button(panel, ID_APPLY, BTN_PRIMARY, st->apply_glyph, st->apply_label);
    st->revert_btn = ui_button(panel, ID_REVERT, BTN_SECONDARY, GLYPH_UNDO, st->revert_label);
    st->detail_btn = ui_button(panel, ID_DETAIL, BTN_PRIMARY, 0, "Turn on");
    ui_button_set_backdrop(st->detail_btn, BACK_SURFACE);
    ShowWindow(st->detail_btn, SW_HIDE);
    build_rows(st);
}

static void update_status(TweakPage *st) {
    if (!gui_ready()) return;
    int on, total, locked, pending;
    counts(st, &on, &total, &locked, &pending);
    char msg[160];
    _snprintf(msg, sizeof(msg), "%d of %d %s active on this PC", on, total, st->noun);
    msg[sizeof(msg) - 1] = '\0';
    gui_status(msg);
}

static void page_show(TweakPage *st) {
    build_rows(st);
    update_status(st);
}

/* Live state changed while the page is up: rows, header counts and the status line. */
static void page_refresh(TweakPage *st) {
    build_rows(st);
    update_status(st);
}

static int page_command(TweakPage *st, WPARAM wparam) {
    int id = LOWORD(wparam), code = HIWORD(wparam);
    if (id == ID_SEARCH && code == EN_CHANGE) {
        ui_search_text(st->search, st->query, sizeof(st->query));
        build_rows(st);
        return 1;
    }
    if (id >= ID_CHIP_BASE && id < ID_CHIP_BASE + FILTER_COUNT) {
        st->filter = id - ID_CHIP_BASE;
        for (int f = 0; f < FILTER_COUNT; f++) ui_button_set_checked(st->chips[f], f == st->filter);
        build_rows(st);
        return 1;
    }
    if (id == ID_APPLY)  { apply_all(st, 1); return 1; }
    if (id == ID_REVERT) { apply_all(st, 0); return 1; }
    if (id == ID_DETAIL) {
        int i = selected_tweak(st);
        if (i >= 0) toggle_tweak(i);
        return 1;
    }
    return 0;
}

static int find_row_by_prefix(TweakPage *st, const WCHAR *prefix, int start) {
    char p[64];
    ui_narrow(prefix, p, sizeof(p));
    size_t n = strlen(p);
    if (n == 0) return -1;
    for (int k = 0; k < st->row_count; k++) {
        int r = (start + k) % st->row_count;
        const char *name = st->rows[r].kind == ROW_TWEAK ? g_tweaks[st->rows[r].value].name
                                                         : group_name(st, st->rows[r].value);
        if (_strnicmp(name, p, n) == 0) return r;
    }
    return -1;
}

static int page_notify(TweakPage *st, LPNMHDR hdr, LRESULT *res) {
    if (hdr->hwndFrom != st->list) return 0;
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
                if (row >= 0 && row < st->row_count) {
                    RECT rc;
                    ui_list_row_rect(st->list, row, &rc);
                    if (st->rows[row].kind == ROW_GROUP) draw_group_row(st, cd->nmcd.hdc, &rc, row, st->rows[row].value);
                    else draw_tweak_row(st, cd->nmcd.hdc, &rc, row, st->rows[row].value);
                }
                *res = CDRF_SKIPDEFAULT;
                return 1;
            }
            return 0;
        }
        case LVN_GETDISPINFOW: {
            NMLVDISPINFOW *di = (NMLVDISPINFOW *)hdr;
            int row = di->item.iItem;
            if ((di->item.mask & LVIF_TEXT) && row >= 0 && row < st->row_count && di->item.cchTextMax > 0) {
                const char *text = st->rows[row].kind == ROW_TWEAK ? g_tweaks[st->rows[row].value].name
                                                                   : group_name(st, st->rows[row].value);
                ui_widen(text, di->item.pszText, di->item.cchTextMax);
            }
            return 1;
        }
        case LVN_ODFINDITEMW: {
            NMLVFINDITEMW *fi = (NMLVFINDITEMW *)hdr;
            *res = (fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) && fi->lvfi.psz
                 ? find_row_by_prefix(st, fi->lvfi.psz, fi->iStart) : -1;
            return 1;
        }
        case NM_CLICK:
        case NM_DBLCLK: {
            NMITEMACTIVATE *a = (NMITEMACTIVATE *)hdr;
            int row = a->iItem;
            if (row < 0 || row >= st->row_count) return 0;
            if (st->rows[row].kind == ROW_GROUP) {
                if (hdr->code == NM_CLICK) toggle_group(st, st->rows[row].value);
            } else {
                int on_switch = switch_hit(st->list, a->ptAction.x);
                /* A double-click on the switch is two clicks: the first already toggled it. */
                if ((hdr->code == NM_CLICK) == on_switch) toggle_tweak(st->rows[row].value);
            }
            return 1;
        }
        case NM_RETURN:
            activate_row(st, ui_list_selected(st->list));
            return 1;
        case LVN_KEYDOWN: {
            NMLVKEYDOWN *k = (NMLVKEYDOWN *)hdr;
            if (k->wVKey == VK_SPACE) {
                activate_row(st, ui_list_selected(st->list));
                return 1;
            }
            return 0;
        }
        case LVN_ITEMCHANGED:
            update_detail_button(st);
            InvalidateRect(st->panel, &st->detail_card, FALSE);
            return 1;
        case NM_SETFOCUS:
        case NM_KILLFOCUS:
            InvalidateRect(st->list, NULL, FALSE);
            return 0;
    }
    return 0;
}

static void page_context(TweakPage *st, HWND from, int x, int y) {
    if (from != st->list) return;
    int row = ui_list_selected(st->list);
    if (x == -1 && y == -1) {
        RECT rc;
        if (row < 0) return;
        ui_list_row_rect(st->list, row, &rc);
        POINT p = { rc.left + ui_px(80), rc.bottom };
        ClientToScreen(st->list, &p);
        x = p.x;
        y = p.y;
    }
    if (row < 0 || row >= st->row_count) return;

    HMENU m = CreatePopupMenu();
    int tweak = st->rows[row].kind == ROW_TWEAK ? st->rows[row].value : -1;
    if (tweak >= 0) {
        AppendMenuW(m, MF_STRING, 1, g_tweaks[tweak].enabled ? L"Turn off" : L"Turn on");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, 2, L"Copy name");
        AppendMenuW(m, MF_STRING, 3, L"Copy details");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, 4, L"Collapse all groups");
    AppendMenuW(m, MF_STRING, 5, L"Expand all groups");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, st->panel, NULL);
    DestroyMenu(m);

    switch (cmd) {
        case 1: toggle_tweak(tweak); break;
        case 2:
            if (ui_copy_text(gui_main_window(), g_tweaks[tweak].name) == 0) gui_toast(TONE_GOOD, "Copied to the clipboard.", NULL, NULL);
            break;
        case 3: copy_details(tweak); break;
        case 4: st->collapsed = 0xFFFFFFFFu; build_rows(st); break;
        case 5: st->collapsed = 0; build_rows(st); break;
    }
}

static void page_focus_search(TweakPage *st) {
    SetFocus(st->search);
    SendMessageW(st->search, EM_SETSEL, 0, -1);
}

static int page_badge(TweakPage *st, char *buf, size_t size) {
    if (!gui_ready()) return 0;
    int on, total, locked, pending;
    counts(st, &on, &total, &locked, &pending);
    _snprintf(buf, size, "%d/%d", on, total);
    buf[size - 1] = '\0';
    return 1;
}

/* ---- bindings ---- */

static void perf_create(HWND p) { page_create(&g_perf, p); }
static void perf_layout(int w, int h) { page_layout(&g_perf, w, h); }
static void perf_paint(HDC dc, const RECT *rc) { page_paint(&g_perf, dc, rc); }
static void perf_show(void) { page_show(&g_perf); }
static int  perf_command(WPARAM w, LPARAM l) { (void)l; return page_command(&g_perf, w); }
static int  perf_notify(LPNMHDR h, LRESULT *r) { return page_notify(&g_perf, h, r); }
static void perf_context(HWND f, int x, int y) { page_context(&g_perf, f, x, y); }
static void perf_refresh(void) { page_refresh(&g_perf); }
static void perf_reload(void) { gui_rescan(0); }
static void perf_search(void) { page_focus_search(&g_perf); }
static int  perf_badge(char *b, size_t n) { return page_badge(&g_perf, b, n); }

static void tele_create(HWND p) { page_create(&g_tele, p); }
static void tele_layout(int w, int h) { page_layout(&g_tele, w, h); }
static void tele_paint(HDC dc, const RECT *rc) { page_paint(&g_tele, dc, rc); }
static void tele_show(void) { page_show(&g_tele); }
static int  tele_command(WPARAM w, LPARAM l) { (void)l; return page_command(&g_tele, w); }
static int  tele_notify(LPNMHDR h, LRESULT *r) { return page_notify(&g_tele, h, r); }
static void tele_context(HWND f, int x, int y) { page_context(&g_tele, f, x, y); }
static void tele_refresh(void) { page_refresh(&g_tele); }
static void tele_search(void) { page_focus_search(&g_tele); }
static int  tele_badge(char *b, size_t n) { return page_badge(&g_tele, b, n); }

GuiPage g_page_performance = {
    "Performance", GLYPH_SPEED, perf_create, perf_layout, perf_paint, perf_show, NULL,
    perf_command, perf_notify, NULL, perf_context, perf_refresh, perf_reload, perf_search, perf_badge
};

GuiPage g_page_telemetry = {
    "Privacy", GLYPH_SHIELD, tele_create, tele_layout, tele_paint, tele_show, NULL,
    tele_command, tele_notify, NULL, tele_context, tele_refresh, perf_reload, tele_search, tele_badge
};
