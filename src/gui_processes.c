#include "gui.h"
#include "proc_utils.h"
#include "affinity.h"
#include <shellapi.h>

#define ID_LIST         500
#define ID_SEARCH       501
#define ID_KILL         502
#define ID_KILL_FLAGGED 503
#define ID_FOCUS        504
#define ID_LIVE         505
#define ID_REFRESH      506
#define ID_CHIP_BASE    510

#define TIMER_REFRESH 1
#define TIMER_FIRST   2
#define REFRESH_MS    2000
#define ROW_HEIGHT    36

enum { COL_NAME, COL_PID, COL_CPU, COL_MEM, COL_CORES, COL_NOTES, COL_COUNT };
static const char *COL_LABELS[COL_COUNT] = { "Process", "PID", "CPU", "Memory", "Cores", "Notes" };

enum { F_ALL, F_FLAGGED, F_PINNED, F_COUNT };

static HWND g_panel, g_list, g_search, g_kill, g_kill_flagged, g_focus, g_live_btn, g_refresh;
static HWND g_chips[F_COUNT];
static ProcInfo g_procs[PROC_MAX];
static int g_count;
static int g_rows[PROC_MAX], g_row_count;
static int g_sort_col = COL_CPU, g_sort_dir = -1;
static int g_filter;
static char g_query[128];
static int g_live = 1, g_visible;
static DWORD g_sel_pid;
static DWORD_PTR g_focus_mask;
static int g_cores, g_flagged, g_pinned_now;
static DWORD g_game_pid;
static char g_game_name[64];
static RECT g_game_card, g_list_card;
static int g_width;
static int g_live_compact;   /* toolbar too narrow for the Live label */
static int g_rebuilding;     /* selection changes made by build_rows, not the user */

/* ---- classification ---- */

static int is_pinned(const ProcInfo *p) {
    return g_focus_mask && p->affinity_mask == g_focus_mask && !proc_is_protected(p->name);
}

static int is_game(const ProcInfo *p) {
    return affinity_is_game(p->name, AFFINITY_VALORANT_EXES, AFFINITY_VALORANT_EXE_COUNT);
}

static int note_rank(const ProcInfo *p) {
    return is_game(p) ? 3 : is_pinned(p) ? 2 : p->flagged ? 1 : 0;
}

static int cmp_rows(const void *a, const void *b) {
    const ProcInfo *pa = &g_procs[*(const int *)a], *pb = &g_procs[*(const int *)b];
    int r = 0;
    switch (g_sort_col) {
        case COL_NAME:  r = _stricmp(pa->name, pb->name); break;
        case COL_PID:   r = (pa->pid > pb->pid) - (pa->pid < pb->pid); break;
        case COL_CPU:   r = (pa->cpu_percent > pb->cpu_percent) - (pa->cpu_percent < pb->cpu_percent); break;
        case COL_MEM:   r = (pa->working_set_kb > pb->working_set_kb) - (pa->working_set_kb < pb->working_set_kb); break;
        case COL_CORES: r = pa->affinity_cores - pb->affinity_cores; break;
        case COL_NOTES: r = note_rank(pa) - note_rank(pb); break;
    }
    if (g_sort_dir < 0) r = -r;
    if (r == 0) r = _stricmp(pa->name, pb->name);
    if (r == 0) r = (pa->pid > pb->pid) - (pa->pid < pb->pid);
    return r;
}

/* ---- data ---- */

static void update_buttons(void) {
    int row = ui_list_selected(g_list);
    EnableWindow(g_kill, row >= 0 && row < g_row_count);
    char label[64];
    if (g_flagged > 0) _snprintf(label, sizeof(label), "Close %d background app%s", g_flagged, g_flagged == 1 ? "" : "s");
    else _snprintf(label, sizeof(label), "Close background apps");
    label[sizeof(label) - 1] = '\0';
    ui_button_set_text(g_kill_flagged, label);
    EnableWindow(g_kill_flagged, g_flagged > 0);

    if (g_pinned_now > 0) {
        ui_button_set_text(g_focus, "Release CPU cores");
        ui_button_set_kind(g_focus, BTN_SECONDARY);
        ui_button_set_glyph(g_focus, GLYPH_UNDO);
    } else {
        ui_button_set_text(g_focus, "Focus CPU on Valorant");
        ui_button_set_kind(g_focus, BTN_PRIMARY);
        ui_button_set_glyph(g_focus, GLYPH_GAME);
    }
    EnableWindow(g_focus, g_cores >= 4 || g_pinned_now > 0);

    char chip[48];
    _snprintf(chip, sizeof(chip), "Background apps (%d)", g_flagged);
    chip[sizeof(chip) - 1] = '\0';
    ui_button_set_text(g_chips[F_FLAGGED], chip);
    _snprintf(chip, sizeof(chip), "Pinned (%d)", g_pinned_now);
    chip[sizeof(chip) - 1] = '\0';
    ui_button_set_text(g_chips[F_PINNED], chip);
}

static void build_rows(void) {
    g_row_count = 0;
    for (int i = 0; i < g_count; i++) {
        const ProcInfo *p = &g_procs[i];
        if (g_filter == F_FLAGGED && !p->flagged) continue;
        if (g_filter == F_PINNED && !is_pinned(p)) continue;
        if (g_query[0] && !ui_stristr(p->name, g_query)) {
            char pid[16];
            _snprintf(pid, sizeof(pid), "%lu", (unsigned long)p->pid);
            pid[sizeof(pid) - 1] = '\0';
            if (strcmp(pid, g_query) != 0) continue;
        }
        g_rows[g_row_count++] = i;
    }
    qsort(g_rows, (size_t)g_row_count, sizeof(int), cmp_rows);

    g_rebuilding = 1;
    ListView_SetItemCountEx(g_list, g_row_count, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    int sel = -1;
    for (int r = 0; r < g_row_count && g_sel_pid; r++) {
        if (g_procs[g_rows[r]].pid == g_sel_pid) { sel = r; break; }
    }
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
    if (sel >= 0) ListView_SetItemState(g_list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    g_rebuilding = 0;

    ui_list_set_empty(g_list, g_query[0] ? "No running process matches that search."
                               : g_filter == F_FLAGGED ? "No known background apps are running. Nice."
                               : g_filter == F_PINNED ? "Nothing is pinned. Use Focus CPU on Valorant to reserve cores for the game."
                               : "No processes.");
    InvalidateRect(g_list, NULL, FALSE);
    update_buttons();
}

static void refresh_data(void) {
    g_count = proc_list_delta(g_procs);
    g_cores = affinity_cpu_count();
    g_focus_mask = affinity_focus_mask();
    g_flagged = g_pinned_now = 0;
    g_game_pid = 0;
    g_game_name[0] = '\0';
    for (int i = 0; i < g_count; i++) {
        g_flagged += g_procs[i].flagged;
        g_pinned_now += is_pinned(&g_procs[i]);
        if (!g_game_pid && is_game(&g_procs[i])) {
            g_game_pid = g_procs[i].pid;
            strncpy(g_game_name, g_procs[i].name, sizeof(g_game_name) - 1);
        }
    }
    build_rows();

    char msg[200];
    _snprintf(msg, sizeof(msg), "%d processes \xC2\xB7 %d known background app%s \xC2\xB7 %d pinned%s",
              g_count, g_flagged, g_flagged == 1 ? "" : "s", g_pinned_now,
              g_live ? " \xC2\xB7 updating every 2 s" : " \xC2\xB7 paused");
    msg[sizeof(msg) - 1] = '\0';
    gui_status(msg);
    InvalidateRect(g_panel, &g_game_card, FALSE);
}

int gui_processes_pinned(void) {
    return affinity_count_pinned();
}

/* ---- actions ---- */

static const ProcInfo *selected_proc(void) {
    int row = ui_list_selected(g_list);
    if (row < 0 || row >= g_row_count) return NULL;
    return &g_procs[g_rows[row]];
}

static void kill_selected(void) {
    const ProcInfo *p = selected_proc();
    if (!p) return;
    ProcInfo copy = *p;
    char heading[320], body[400];
    _snprintf(heading, sizeof(heading), "End \xE2\x80\x9C%s\xE2\x80\x9D?", copy.name);
    if (proc_is_protected(copy.name)) {
        _snprintf(body, sizeof(body), "PID %lu is a core Windows process. Ending it can crash Windows, "
                  "sign you out, or leave the desktop unusable until you restart.", (unsigned long)copy.pid);
    } else {
        _snprintf(body, sizeof(body), "PID %lu. Anything unsaved in this app will be lost.", (unsigned long)copy.pid);
    }
    heading[sizeof(heading) - 1] = body[sizeof(body) - 1] = '\0';
    UiDialog d;
    memset(&d, 0, sizeof(d));
    d.heading = heading;
    d.body = body;
    d.ok = "End process";
    d.cancel = "Cancel";
    d.danger = 1;
    d.tone = proc_is_protected(copy.name) ? TONE_BAD : TONE_WARN;
    d.glyph = GLYPH_WARNING;
    if (!ui_dialog(gui_main_window(), &d)) return;

    char msg[320];
    if (proc_kill(copy.pid) == 0) {
        _snprintf(msg, sizeof(msg), "Ended %s.", copy.name);
        gui_toast(TONE_GOOD, msg, NULL, NULL);
    } else {
        _snprintf(msg, sizeof(msg), "Couldn't end %s \xE2\x80\x94 access denied or it already exited.", copy.name);
        gui_toast(TONE_WARN, msg, NULL, NULL);
    }
    refresh_data();
}

static void kill_flagged(void) {
    int count = 0;
    char details[4096] = "";
    size_t len = 0;
    for (int i = 0; i < g_count; i++) {
        if (!g_procs[i].flagged) continue;
        count++;
        if (len < sizeof(details) - 1) {
            int w = _snprintf(details + len, sizeof(details) - len, "%s%s (PID %lu)", len ? "\r\n" : "",
                              g_procs[i].name, (unsigned long)g_procs[i].pid);
            if (w > 0) len += (size_t)w;
            else len = sizeof(details) - 1;
        }
    }
    details[sizeof(details) - 1] = '\0';
    if (count == 0) return;

    char heading[96], ok[48];
    _snprintf(heading, sizeof(heading), "Close %d background app%s?", count, count == 1 ? "" : "s");
    _snprintf(ok, sizeof(ok), "Close %d", count);
    heading[sizeof(heading) - 1] = ok[sizeof(ok) - 1] = '\0';
    UiDialog d;
    memset(&d, 0, sizeof(d));
    d.heading = heading;
    d.body = "These are known low-value apps (sync clients, widgets, chat helpers) that are usually safe "
             "to close for a gaming session. Anything unsaved in them will be lost.";
    d.details = details;
    d.ok = ok;
    d.cancel = "Cancel";
    d.danger = 1;
    d.tone = TONE_WARN;
    d.glyph = GLYPH_WARNING;
    if (!ui_dialog(gui_main_window(), &d)) return;

    int closed = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_procs[i].flagged && proc_kill(g_procs[i].pid) == 0) closed++;
    }
    char msg[128];
    _snprintf(msg, sizeof(msg), "Closed %d of %d background app%s.", closed, count, count == 1 ? "" : "s");
    msg[sizeof(msg) - 1] = '\0';
    gui_toast(closed == count ? TONE_GOOD : TONE_WARN, msg, NULL, NULL);
    refresh_data();
}

static void explain_game_priority(const AffinityResult *r) {
    if (!r->game_found || r->game_priority == 1) return;
    char body[700];
    _snprintf(body, sizeof(body),
              "%s (PID %lu) is running, but Windows denied the priority change \xE2\x80\x94 Vanguard protects the "
              "game process from outside modification, so this is expected.\n\nThe background cores were still "
              "reserved. For High priority as well, turn on \xE2\x80\x9CLaunch Valorant at High CPU priority\xE2\x80\x9D on "
              "the Performance page: Windows applies it at launch, before the anti-cheat locks the process.",
              r->game_name, (unsigned long)r->game_pid);
    body[sizeof(body) - 1] = '\0';
    ui_message(gui_main_window(), TONE_ACCENT, "Cores reserved \xE2\x80\x94 priority left to Windows", body);
}

static void focus_cores(void) {
    int total = affinity_cpu_count();
    int background = affinity_background_core_count();
    if (total < 4) {
        char body[200];
        _snprintf(body, sizeof(body), "This PC has %d logical processors. There aren't enough to split "
                  "between the game and everything else.", total);
        body[sizeof(body) - 1] = '\0';
        ui_message(gui_main_window(), TONE_WARN, "Not enough cores", body);
        return;
    }
    char body[900], ok[64];
    _snprintf(body, sizeof(body),
              "Every ordinary background process moves onto cores 0\xE2\x80\x93%d, leaving %d of %d cores with nothing "
              "else scheduled on them.\n\nValorant itself is never touched \xE2\x80\x94 it keeps every core and the "
              "scheduler puts it where nothing else may run. Vanguard, the Riot client, GPU and audio drivers and "
              "core Windows processes are left alone.\n\nNothing is written to disk: pins reset when a process "
              "restarts, and Release puts everything back immediately.",
              background - 1, total - background, total);
    _snprintf(ok, sizeof(ok), "Reserve %d cores", total - background);
    body[sizeof(body) - 1] = ok[sizeof(ok) - 1] = '\0';
    UiDialog d;
    memset(&d, 0, sizeof(d));
    d.heading = "Reserve CPU cores for Valorant?";
    d.body = body;
    d.ok = ok;
    d.cancel = "Cancel";
    d.tone = TONE_ACCENT;
    d.glyph = GLYPH_GAME;
    if (!ui_dialog(gui_main_window(), &d)) return;

    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    AffinityResult r;
    int rc = affinity_focus_game(AFFINITY_VALORANT_EXES, AFFINITY_VALORANT_EXE_COUNT, &r);
    SetCursor(old);
    if (rc != 0) return;

    char msg[320];
    int n = _snprintf(msg, sizeof(msg), "Pinned %d background process%s to cores 0\xE2\x80\x93%d",
                      r.adjusted, r.adjusted == 1 ? "" : "es", r.background_cores - 1);
    if (!r.game_found && n > 0) n += _snprintf(msg + n, sizeof(msg) - n, " \xC2\xB7 Valorant isn't running yet");
    else if (r.game_priority == 1 && n > 0) n += _snprintf(msg + n, sizeof(msg) - n, " \xC2\xB7 %s set to High priority", r.game_name);
    if (r.denied > 0 && n > 0) {
        _snprintf(msg + n, sizeof(msg) - n, " \xC2\xB7 %d skipped (access denied%s)", r.denied,
                  is_running_elevated() ? "" : " \xE2\x80\x94 run as administrator to reach them");
    }
    msg[sizeof(msg) - 1] = '\0';
    gui_toast(TONE_GOOD, msg, NULL, NULL);
    explain_game_priority(&r);
}

static void release_cores(void) {
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    AffinityResult r;
    affinity_reset(AFFINITY_VALORANT_EXES, AFFINITY_VALORANT_EXE_COUNT, &r);
    SetCursor(old);
    char msg[160];
    _snprintf(msg, sizeof(msg), "Released %d process%s back to all %d cores.", r.adjusted, r.adjusted == 1 ? "" : "es",
              r.total_cores);
    msg[sizeof(msg) - 1] = '\0';
    gui_toast(TONE_GOOD, msg, NULL, NULL);
}

void gui_processes_focus_toggle(void) {
    if (affinity_count_pinned() > 0) release_cores();
    else focus_cores();
    if (g_visible) refresh_data();
}

static void open_location(const ProcInfo *p) {
    char path[MAX_PATH];
    if (proc_image_path(p->pid, path, sizeof(path)) != 0) {
        gui_toast(TONE_WARN, "Windows didn't allow reading that process's location \xE2\x80\x94 try as administrator.", NULL, NULL);
        return;
    }
    WCHAR wpath[MAX_PATH], args[MAX_PATH + 16];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    _snwprintf(args, MAX_PATH + 16, L"/select,\"%ls\"", wpath);
    args[MAX_PATH + 15] = 0;
    ShellExecuteW(gui_main_window(), L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

/* ---- painting ---- */

static void col_rect(int row, int col, const RECT *row_rc, RECT *out) {
    if (col == 0) {
        *out = *row_rc;
        RECT b;
        ListView_GetItemRect(g_list, row, &b, LVIR_BOUNDS);
        out->left = b.left;
        out->right = b.left + ListView_GetColumnWidth(g_list, 0);
    } else {
        ListView_GetSubItemRect(g_list, row, col, LVIR_BOUNDS, out);
    }
}

static void draw_row(HDC dc, int row) {
    RECT rc;
    ui_list_row_rect(g_list, row, &rc);
    ui_list_row_bg(dc, g_list, &rc, row);
    const ProcInfo *p = &g_procs[g_rows[row]];
    int cy = (rc.top + rc.bottom) / 2;
    int limited = p->affinity_mask == 0 && p->working_set_kb == 0;
    int pad = ui_px(12);
    char buf[64];
    RECT c;

    col_rect(row, COL_NAME, &rc, &c);
    RECT t = { c.left + pad + ui_px(4), c.top, c.right - ui_px(4), c.bottom };
    ui_text(dc, p->name, &t, is_game(p) ? FONT_BODY_BOLD : FONT_BODY, limited ? g_ui.text_dim : g_ui.text,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    col_rect(row, COL_PID, &rc, &c);
    _snprintf(buf, sizeof(buf), "%lu", (unsigned long)p->pid);
    buf[sizeof(buf) - 1] = '\0';
    t = c;
    t.right -= pad;
    ui_text(dc, buf, &t, FONT_SMALL, g_ui.text_dim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    col_rect(row, COL_CPU, &rc, &c);
    int num_w = ui_px(46);
    RECT bar = { c.left + pad, cy - ui_px(3), c.right - pad - num_w - ui_px(6), cy + ui_px(3) };
    if (bar.right - bar.left > ui_px(20)) {
        double frac = p->cpu_percent / 100.0;
        COLORREF color = p->cpu_percent >= 50 ? g_ui.warn : p->cpu_percent >= 15 ? g_ui.accent : ui_mix(g_ui.accent, g_ui.surface, 60);
        ui_progress(dc, &bar, frac > 0.004 ? frac : 0, g_ui.surface2, color);
    }
    if (p->cpu_percent >= 10) _snprintf(buf, sizeof(buf), "%.0f%%", p->cpu_percent);
    else _snprintf(buf, sizeof(buf), "%.1f%%", p->cpu_percent);
    buf[sizeof(buf) - 1] = '\0';
    t = c;
    t.right -= pad;
    t.left = t.right - num_w;
    ui_text(dc, buf, &t, FONT_SMALL, p->cpu_percent >= 1 ? g_ui.text : g_ui.text_faint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    col_rect(row, COL_MEM, &rc, &c);
    double mb = p->working_set_kb / 1024.0;
    if (p->working_set_kb == 0) _snprintf(buf, sizeof(buf), "\xE2\x80\x94");
    else if (mb >= 1024) _snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
    else _snprintf(buf, sizeof(buf), "%.0f MB", mb);
    buf[sizeof(buf) - 1] = '\0';
    t = c;
    t.right -= pad;
    ui_text(dc, buf, &t, FONT_SMALL, p->working_set_kb ? g_ui.text : g_ui.text_faint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    col_rect(row, COL_CORES, &rc, &c);
    if (p->affinity_cores > 0) _snprintf(buf, sizeof(buf), "%d/%d", p->affinity_cores, g_cores);
    else _snprintf(buf, sizeof(buf), "\xE2\x80\x94");
    buf[sizeof(buf) - 1] = '\0';
    t = c;
    t.right -= pad;
    ui_text(dc, buf, &t, is_pinned(p) ? FONT_SMALL_BOLD : FONT_SMALL,
            is_pinned(p) ? g_ui.accent : p->affinity_cores ? g_ui.text_dim : g_ui.text_faint,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    col_rect(row, COL_NOTES, &rc, &c);
    int x = c.left + pad;
    if (is_game(p)) x += ui_pill(dc, x, cy, "Game", TONE_GOOD, 0) + ui_px(6);
    if (is_pinned(p)) x += ui_pill(dc, x, cy, "Pinned", TONE_ACCENT, 0) + ui_px(6);
    if (p->flagged) x += ui_pill(dc, x, cy, "Background app", TONE_WARN, 0) + ui_px(6);
    if (limited && x == c.left + pad) {
        t = c;
        t.left = x;
        ui_text(dc, "Limited access", &t, FONT_SMALL, g_ui.text_faint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void proc_paint(HDC dc, const RECT *client) {
    (void)client;
    int pad = ui_px(28);
    RECT title = { pad, pad - ui_px(4), g_width / 2, pad + ui_px(34) };
    ui_text(dc, "Processes", &title, FONT_TITLE, g_ui.text, DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT sr = { pad, title.bottom + ui_px(4), g_width - pad, title.bottom + ui_px(26) };
    ui_text(dc, "Live CPU and memory for everything running, with one-click cleanup and core reservation.",
            &sr, FONT_BODY, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT c = g_game_card;
    ui_card(dc, &c);
    int ch = c.bottom - c.top;
    RECT ic = { c.left + ui_px(20), c.top + (ch - ui_px(44)) / 2, c.left + ui_px(64), c.top + (ch + ui_px(44)) / 2 };
    ui_round_fill(dc, &ic, ui_px(12), g_ui.accent_soft);
    ui_glyph(dc, GLYPH_GAME, &ic, FONT_ICON_LARGE, g_ui.accent);

    RECT btn;
    GetWindowRect(g_focus, &btn);
    MapWindowPoints(NULL, g_panel, (POINT *)&btn, 2);
    int tx = ic.right + ui_px(16);
    int right = btn.left - ui_px(20);
    int mid = (c.top + c.bottom) / 2;
    RECT nr = { tx, c.top, right, mid - ui_px(1) };
    ui_text(dc, "Valorant", &nr, FONT_HEADING, g_ui.text, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    int nw = ui_text_width(dc, "Valorant", FONT_HEADING);
    char pill[96];
    if (g_game_pid) _snprintf(pill, sizeof(pill), "Running \xC2\xB7 PID %lu", (unsigned long)g_game_pid);
    else _snprintf(pill, sizeof(pill), "Not running");
    pill[sizeof(pill) - 1] = '\0';
    ui_pill(dc, tx + nw + ui_px(10), mid - ui_px(12), pill, g_game_pid ? TONE_GOOD : TONE_NEUTRAL, 0);

    char line[300];
    int bg = affinity_background_core_count();
    if (g_cores < 4) {
        _snprintf(line, sizeof(line), "Core focus needs at least 4 logical processors \xE2\x80\x94 this PC has %d.", g_cores);
    } else if (g_pinned_now > 0) {
        _snprintf(line, sizeof(line), "Core focus is on: %d background process%s confined to cores 0\xE2\x80\x93%d, leaving %d cores to the game.",
                  g_pinned_now, g_pinned_now == 1 ? " is" : "es are", bg - 1, g_cores - bg);
    } else {
        _snprintf(line, sizeof(line), "Core focus is off. Turn it on to move background apps onto cores 0\xE2\x80\x93%d so the game gets the other %d to itself.",
                  bg - 1, g_cores - bg);
    }
    line[sizeof(line) - 1] = '\0';
    RECT lr = { tx, mid + ui_px(3), right, c.bottom };
    ui_text(dc, line, &lr, FONT_SMALL, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    ui_search_paint(dc, g_search);
    ui_card(dc, &g_list_card);
}

/* ---- layout & lifecycle ---- */

static void size_columns(void) {
    /* Size against the window minus a scrollbar's width, so columns don't overflow into a
     * horizontal scrollbar the moment the vertical one appears. */
    RECT rc;
    GetWindowRect(g_list, &rc);
    rc.right = (rc.right - rc.left) - GetSystemMetricsForDpi(SM_CXVSCROLL, ui_dpi()) - ui_px(2);
    int fixed[COL_COUNT] = { 0, ui_px(76), ui_px(150), ui_px(96), ui_px(72), ui_px(230) };
    int used = 0;
    for (int i = 1; i < COL_COUNT; i++) used += fixed[i];
    /* When narrow, Notes and then CPU give up width before the process name does. */
    int deficit = ui_px(180) - (rc.right - used);
    if (deficit > 0) {
        int give = fixed[COL_NOTES] - ui_px(110);
        if (give > deficit) give = deficit;
        fixed[COL_NOTES] -= give;
        used -= give;
        deficit -= give;
    }
    if (deficit > 0) {
        int give = fixed[COL_CPU] - ui_px(104);
        if (give > deficit) give = deficit;
        fixed[COL_CPU] -= give;
        used -= give;
    }
    int name = rc.right - used;
    if (name < ui_px(140)) name = ui_px(140);
    ListView_SetColumnWidth(g_list, 0, name);
    for (int i = 1; i < COL_COUNT; i++) ListView_SetColumnWidth(g_list, i, fixed[i]);
}

static void proc_layout(int w, int h) {
    g_width = w;
    int pad = ui_px(28), bh = ui_px(36), gap = ui_px(16);

    int kfw = ui_button_width(g_kill_flagged);
    if (kfw < ui_px(200)) kfw = ui_px(200);
    MoveWindow(g_kill_flagged, w - pad - kfw, pad, kfw, bh, TRUE);
    int kw = ui_button_width(g_kill);
    MoveWindow(g_kill, w - pad - kfw - ui_px(8) - kw, pad, kw, bh, TRUE);

    int y = pad + ui_px(76);
    SetRect(&g_game_card, pad, y, w - pad, y + ui_px(84));
    int fw = ui_px(214);
    MoveWindow(g_focus, g_game_card.right - ui_px(20) - fw, g_game_card.top + (ui_px(84) - bh) / 2, fw, bh, TRUE);
    y = g_game_card.bottom + gap;

    int ch = ui_px(30);
    int chip_w[F_COUNT], chips_w = 0;
    for (int f = 0; f < F_COUNT; f++) {
        chip_w[f] = ui_button_width(g_chips[f]);
        if (f == F_FLAGGED && chip_w[f] < ui_px(158)) chip_w[f] = ui_px(158);
        if (f == F_PINNED && chip_w[f] < ui_px(96)) chip_w[f] = ui_px(96);
        chips_w += chip_w[f] + ui_px(8);
    }
    /* The search box gives up width first, then the Live toggle drops its label. */
    int live_full = ui_px(96);
    int right_w = bh + ui_px(8) + live_full;
    int sw = (w - pad * 2) - chips_w - right_w - ui_px(24);
    g_live_compact = sw < ui_px(180);
    if (g_live_compact) {
        right_w = bh + ui_px(8) + ch;
        sw = (w - pad * 2) - chips_w - right_w - ui_px(24);
    }
    if (sw > ui_px(260)) sw = ui_px(260);
    if (sw < ui_px(150)) sw = ui_px(150);
    ui_search_place(g_search, pad, y, sw, bh);
    int cx = pad + sw + ui_px(16);
    for (int f = 0; f < F_COUNT; f++) {
        MoveWindow(g_chips[f], cx, y + (bh - ch) / 2, chip_w[f], ch, TRUE);
        cx += chip_w[f] + ui_px(8);
    }
    MoveWindow(g_refresh, w - pad - bh, y, bh, bh, TRUE);
    int lw = g_live_compact ? ch : live_full;
    MoveWindow(g_live_btn, w - pad - bh - ui_px(8) - lw, y + (bh - ch) / 2, lw, ch, TRUE);
    ui_button_set_text(g_live_btn, g_live_compact ? "" : g_live ? "Live" : "Paused");

    y += bh + gap;
    SetRect(&g_list_card, pad, y, w - pad, h - ui_px(20));
    int inset = ui_px(6);
    MoveWindow(g_list, g_list_card.left + inset, g_list_card.top + inset,
               g_list_card.right - g_list_card.left - inset * 2, g_list_card.bottom - g_list_card.top - inset * 2, TRUE);
    size_columns();
}

static void proc_create(HWND panel) {
    g_panel = panel;
    g_list = ui_list(panel, ID_LIST, LVS_SINGLESEL, ROW_HEIGHT, 0);
    for (int i = 0; i < COL_COUNT; i++) {
        WCHAR label[32];
        ui_widen(COL_LABELS[i], label, 32);
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = (i == COL_PID || i == COL_MEM || i == COL_CORES) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        col.pszText = label;
        col.cx = ui_px(100);
        SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
    }
    ui_list_set_sort(g_list, g_sort_col, g_sort_dir);

    g_search = ui_search(panel, ID_SEARCH, "Search by name or PID");
    g_chips[F_ALL] = ui_button(panel, ID_CHIP_BASE + F_ALL, BTN_CHIP, 0, "All");
    g_chips[F_FLAGGED] = ui_button(panel, ID_CHIP_BASE + F_FLAGGED, BTN_CHIP, 0, "Background apps (0)");
    g_chips[F_PINNED] = ui_button(panel, ID_CHIP_BASE + F_PINNED, BTN_CHIP, 0, "Pinned (0)");
    ui_button_set_checked(g_chips[F_ALL], 1);
    g_live_btn = ui_button(panel, ID_LIVE, BTN_CHIP, GLYPH_PAUSE, "Live");
    ui_button_set_checked(g_live_btn, 1);
    g_refresh = ui_button(panel, ID_REFRESH, BTN_ICON, GLYPH_REFRESH, "");
    g_kill = ui_button(panel, ID_KILL, BTN_DANGER, GLYPH_CLOSE, "End process");
    g_kill_flagged = ui_button(panel, ID_KILL_FLAGGED, BTN_SECONDARY, GLYPH_ERASE, "Close background apps");
    g_focus = ui_button(panel, ID_FOCUS, BTN_PRIMARY, GLYPH_GAME, "Focus CPU on Valorant");
    ui_button_set_backdrop(g_focus, BACK_SURFACE);
}

static void set_live(int live) {
    g_live = live;
    ui_button_set_checked(g_live_btn, live);
    ui_button_set_glyph(g_live_btn, live ? GLYPH_PAUSE : GLYPH_PLAY);
    ui_button_set_text(g_live_btn, g_live_compact ? "" : live ? "Live" : "Paused");
    if (g_visible) {
        if (live) SetTimer(g_panel, TIMER_REFRESH, REFRESH_MS, NULL);
        else KillTimer(g_panel, TIMER_REFRESH);
    }
    refresh_data();
}

static void proc_show(void) {
    g_visible = 1;
    refresh_data();
    /* The first delta sample has no baseline; take a second one shortly after so CPU% fills in
     * without waiting a full refresh period. */
    SetTimer(g_panel, TIMER_FIRST, 450, NULL);
    if (g_live) SetTimer(g_panel, TIMER_REFRESH, REFRESH_MS, NULL);
}

static void proc_hide(void) {
    g_visible = 0;
    KillTimer(g_panel, TIMER_REFRESH);
    KillTimer(g_panel, TIMER_FIRST);
}

static void proc_timer(UINT_PTR id) {
    if (id == TIMER_FIRST) KillTimer(g_panel, TIMER_FIRST);
    if (!g_visible || IsIconic(gui_main_window())) return;
    refresh_data();
}

static int proc_command(WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    int id = LOWORD(wparam);
    if (id == ID_SEARCH && HIWORD(wparam) == EN_CHANGE) {
        ui_search_text(g_search, g_query, sizeof(g_query));
        build_rows();
        return 1;
    }
    if (id >= ID_CHIP_BASE && id < ID_CHIP_BASE + F_COUNT) {
        g_filter = id - ID_CHIP_BASE;
        for (int f = 0; f < F_COUNT; f++) ui_button_set_checked(g_chips[f], f == g_filter);
        build_rows();
        return 1;
    }
    switch (id) {
        case ID_KILL:         kill_selected(); return 1;
        case ID_KILL_FLAGGED: kill_flagged(); return 1;
        case ID_FOCUS:        gui_processes_focus_toggle(); return 1;
        case ID_LIVE:         set_live(!g_live); return 1;
        case ID_REFRESH:      refresh_data(); return 1;
    }
    return 0;
}

static int proc_notify(LPNMHDR hdr, LRESULT *res) {
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
                const ProcInfo *p = &g_procs[g_rows[row]];
                if (di->item.iSubItem == COL_PID) _snwprintf(di->item.pszText, di->item.cchTextMax, L"%lu", (unsigned long)p->pid);
                else ui_widen(p->name, di->item.pszText, di->item.cchTextMax);
                di->item.pszText[di->item.cchTextMax - 1] = 0;
            }
            return 1;
        }
        case LVN_ODFINDITEMW: {
            NMLVFINDITEMW *fi = (NMLVFINDITEMW *)hdr;
            *res = -1;
            if (fi->lvfi.psz && g_row_count > 0) {
                char p[64];
                ui_narrow(fi->lvfi.psz, p, sizeof(p));
                size_t n = strlen(p);
                for (int k = 0; k < g_row_count && n; k++) {
                    int r = (fi->iStart + k) % g_row_count;
                    if (_strnicmp(g_procs[g_rows[r]].name, p, n) == 0) { *res = r; break; }
                }
            }
            return 1;
        }
        case LVN_COLUMNCLICK: {
            int col = ((NMLISTVIEW *)hdr)->iSubItem;
            if (col == g_sort_col) g_sort_dir = -g_sort_dir;
            else {
                g_sort_col = col;
                g_sort_dir = (col == COL_NAME) ? 1 : -1;
            }
            ui_list_set_sort(g_list, g_sort_col, g_sort_dir);
            build_rows();
            return 1;
        }
        case LVN_ITEMCHANGED: {
            if (!g_rebuilding) {
                const ProcInfo *p = selected_proc();
                g_sel_pid = p ? p->pid : 0;
            }
            update_buttons();
            return 1;
        }
        case NM_SETFOCUS:
        case NM_KILLFOCUS:
            InvalidateRect(g_list, NULL, FALSE);
            return 0;
        case LVN_KEYDOWN:
            if (((NMLVKEYDOWN *)hdr)->wVKey == VK_DELETE) {
                kill_selected();
                return 1;
            }
            return 0;
    }
    return 0;
}

static void proc_context(HWND from, int x, int y) {
    if (from != g_list) return;
    const ProcInfo *p = selected_proc();
    if (!p) return;
    ProcInfo copy = *p;
    if (x == -1 && y == -1) {
        RECT rc;
        ui_list_row_rect(g_list, ui_list_selected(g_list), &rc);
        POINT pt = { rc.left + ui_px(60), rc.bottom };
        ClientToScreen(g_list, &pt);
        x = pt.x;
        y = pt.y;
    }
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"End process\tDel");
    AppendMenuW(m, MF_STRING, 2, L"Open file location");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"Copy name");
    AppendMenuW(m, MF_STRING, 4, L"Copy PID");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, g_panel, NULL);
    DestroyMenu(m);

    char pid[16];
    _snprintf(pid, sizeof(pid), "%lu", (unsigned long)copy.pid);
    pid[sizeof(pid) - 1] = '\0';
    switch (cmd) {
        case 1: kill_selected(); break;
        case 2: open_location(&copy); break;
        case 3: if (ui_copy_text(gui_main_window(), copy.name) == 0) gui_toast(TONE_GOOD, "Copied to the clipboard.", NULL, NULL); break;
        case 4: if (ui_copy_text(gui_main_window(), pid) == 0) gui_toast(TONE_GOOD, "Copied to the clipboard.", NULL, NULL); break;
    }
}

static void proc_focus_search(void) {
    SetFocus(g_search);
    SendMessageW(g_search, EM_SETSEL, 0, -1);
}

static void proc_refresh(void) { if (g_visible) refresh_data(); }

GuiPage g_page_processes = {
    "Processes", GLYPH_PROCESSES, proc_create, proc_layout, proc_paint, proc_show, proc_hide,
    proc_command, proc_notify, proc_timer, proc_context, proc_refresh, proc_refresh, proc_focus_search, NULL
};
