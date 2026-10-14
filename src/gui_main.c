#include "gui.h"
#include "config.h"
#include <shellapi.h>
#include <windowsx.h>
#include <uxtheme.h>

#define MAIN_CLASS    L"GameOptimizerMainWindow"
#define PANEL_CLASS   L"GameOptimizerPanel"
#define SIDEBAR_CLASS L"GameOptimizerSidebar"
#define STATUS_CLASS  L"GameOptimizerStatus"
#define BANNER_CLASS  L"GameOptimizerBanner"

#define SIDEBAR_W   232
#define STATUS_H    38
#define BANNER_H    50
#define NAV_TOP     96
#define NAV_H       40
#define NAV_GAP     2

#define ID_TOAST_ACTION  10
#define ID_BANNER_ACTION 11
#define ID_BANNER_CLOSE  12
#define TIMER_TOAST      1

static HWND g_main, g_sidebar, g_statusbar;
static GuiPage *g_pages[PAGE_COUNT];
static HWND g_panels[PAGE_COUNT];
static int g_current = -1;

static int g_ready;
static SysInfo g_sys;
static int g_sys_ready;
static int g_boot_state[128];     /* tweak states at startup, for restart-pending tracking */
static int g_close_pending;

/* =======================================================================
 * Small accessors
 * ===================================================================== */

HWND gui_main_window(void) { return g_main; }
HWND gui_page_panel(int page) { return (page >= 0 && page < PAGE_COUNT) ? g_panels[page] : NULL; }
int  gui_current_page(void) { return g_current; }
int  gui_ready(void) { return g_ready; }
const SysInfo *gui_sysinfo(void) { return &g_sys; }
int  gui_sysinfo_ready(void) { return g_sys_ready; }

int gui_restart_pending_for(int i) {
    if (!g_ready || i < 0 || i >= g_tweak_count || i >= 128) return 0;
    return g_tweaks[i].requires_reboot && g_tweaks[i].enabled != g_boot_state[i];
}

int gui_pending_restart_count(void) {
    int n = 0;
    for (int i = 0; i < g_tweak_count; i++) n += gui_restart_pending_for(i);
    return n;
}

/* =======================================================================
 * Status bar: page status, job progress, toasts, privilege pill
 * ===================================================================== */

static char g_status_text[256] = "Starting\xE2\x80\xA6";
static char g_toast_text[320];
static UiTone g_toast_tone;
static int g_toast_active;
static void (*g_toast_action)(void);
static HWND g_toast_btn;
static int g_prog_active, g_prog_cur, g_prog_total;
static char g_prog_text[192];
static RECT g_priv_pill;

void gui_status(const char *text) {
    strncpy(g_status_text, text, sizeof(g_status_text) - 1);
    g_status_text[sizeof(g_status_text) - 1] = '\0';
    if (g_statusbar && !g_toast_active && !g_prog_active) InvalidateRect(g_statusbar, NULL, FALSE);
}

static void place_toast_button(void) {
    if (!g_toast_btn) return;
    if (!g_toast_active || !g_toast_action || g_prog_active) {
        ShowWindow(g_toast_btn, SW_HIDE);
        return;
    }
    HDC dc = GetDC(g_statusbar);
    int tw = ui_text_width(dc, g_toast_text, FONT_BODY);
    ReleaseDC(g_statusbar, dc);
    RECT rc;
    GetClientRect(g_statusbar, &rc);
    int x = ui_px(20) + ui_px(24) + tw + ui_px(10);
    int bw = ui_button_width(g_toast_btn);
    int max_x = g_priv_pill.left - bw - ui_px(12);
    if (x > max_x) x = max_x;
    int bh = ui_px(26);
    MoveWindow(g_toast_btn, x, (rc.bottom - bh) / 2 + ui_px(1), bw, bh, TRUE);
    ShowWindow(g_toast_btn, SW_SHOW);
}

void gui_toast(UiTone tone, const char *text, const char *action, void (*on_action)(void)) {
    strncpy(g_toast_text, text, sizeof(g_toast_text) - 1);
    g_toast_text[sizeof(g_toast_text) - 1] = '\0';
    g_toast_tone = tone;
    g_toast_active = 1;
    g_toast_action = action ? on_action : NULL;
    if (g_toast_btn && action) ui_button_set_text(g_toast_btn, action);
    if (g_statusbar) {
        place_toast_button();
        SetTimer(g_statusbar, TIMER_TOAST, action ? 12000 : 7000, NULL);
        InvalidateRect(g_statusbar, NULL, FALSE);
    }
}

static void clear_toast(void) {
    g_toast_active = 0;
    g_toast_action = NULL;
    if (g_statusbar) {
        KillTimer(g_statusbar, TIMER_TOAST);
        place_toast_button();
        InvalidateRect(g_statusbar, NULL, FALSE);
    }
}

static void repaint_current_page(void) {
    if (g_current >= 0) RedrawWindow(g_panels[g_current], NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void gui_progress(int current, int total, const char *text) {
    if (!g_prog_active) clear_toast();
    g_prog_active = 1;
    g_prog_cur = current;
    g_prog_total = total;
    strncpy(g_prog_text, text, sizeof(g_prog_text) - 1);
    g_prog_text[sizeof(g_prog_text) - 1] = '\0';
    if (g_statusbar) InvalidateRect(g_statusbar, NULL, FALSE);
    /* Rows show which tweak is mid-change. */
    repaint_current_page();
}

void gui_progress_end(void) {
    g_prog_active = 0;
    if (g_statusbar) {
        place_toast_button();
        InvalidateRect(g_statusbar, NULL, FALSE);
    }
}

void gui_job_finished(void) {
    if (g_close_pending) DestroyWindow(g_main);
}

static void status_paint(HWND h) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    UiBuffer buf;
    HDC dc = ui_buffer_begin(&buf, hdc, &rc);
    ui_fill_rect(dc, &rc, g_ui.bg);
    ui_hline(dc, rc.left, rc.right, rc.top, g_ui.border);

    int elevated = is_running_elevated();
    const char *priv = elevated ? "Administrator" : "Standard user";
    int pw = ui_pill_width(dc, priv) + ui_px(18);
    int cy = (rc.bottom + rc.top) / 2 + ui_px(1);
    SetRect(&g_priv_pill, rc.right - ui_px(16) - pw, cy - ui_px(10), rc.right - ui_px(16), cy + ui_px(10));
    ui_round_fill(dc, &g_priv_pill, ui_px(10), elevated ? g_ui.good_soft : g_ui.warn_soft);
    RECT pg = { g_priv_pill.left + ui_px(6), g_priv_pill.top, g_priv_pill.left + ui_px(22), g_priv_pill.bottom };
    ui_glyph(dc, elevated ? GLYPH_ADMIN : GLYPH_LOCK, &pg, FONT_ICON_SMALL, elevated ? g_ui.good : g_ui.warn);
    RECT pt = { pg.right, g_priv_pill.top, g_priv_pill.right - ui_px(8), g_priv_pill.bottom };
    ui_text(dc, priv, &pt, FONT_SMALL_BOLD, elevated ? g_ui.good : g_ui.warn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int x = ui_px(20);
    int right = g_priv_pill.left - ui_px(16);
    if (g_prog_active) {
        int bw = ui_px(140);
        RECT bar = { x, cy - ui_px(3), x + bw, cy + ui_px(3) };
        double frac = g_prog_total > 0 ? (double)g_prog_cur / g_prog_total : 0.0;
        ui_progress(dc, &bar, frac < 0.04 ? 0.04 : frac, g_ui.surface3, g_ui.accent);
        RECT tr = { bar.right + ui_px(12), rc.top, right, rc.bottom };
        ui_text(dc, g_prog_text, &tr, FONT_BODY, g_ui.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if (g_toast_active) {
        RECT gr = { x, rc.top, x + ui_px(18), rc.bottom };
        WCHAR glyph = g_toast_tone == TONE_GOOD ? GLYPH_CHECK : g_toast_tone == TONE_BAD ? GLYPH_ERROR
                    : g_toast_tone == TONE_WARN ? GLYPH_WARNING : GLYPH_INFO;
        ui_glyph(dc, glyph, &gr, FONT_ICON, ui_tone_color(g_toast_tone == TONE_NEUTRAL ? TONE_ACCENT : g_toast_tone));
        RECT tr = { x + ui_px(24), rc.top, right, rc.bottom };
        if (g_toast_action && g_toast_btn && IsWindowVisible(g_toast_btn)) {
            RECT br;
            GetWindowRect(g_toast_btn, &br);
            MapWindowPoints(NULL, h, (POINT *)&br, 2);
            tr.right = br.left - ui_px(6);
        }
        ui_text(dc, g_toast_text, &tr, FONT_BODY, g_ui.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        RECT tr = { x, rc.top, right, rc.bottom };
        ui_text(dc, g_status_text, &tr, FONT_BODY, g_ui.text_dim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    ui_buffer_end(&buf);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK status_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            g_toast_btn = ui_button(h, ID_TOAST_ACTION, BTN_LINK, 0, "Undo");
            ShowWindow(g_toast_btn, SW_HIDE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            status_paint(h);
            return 0;
        case WM_SIZE:
            place_toast_button();
            InvalidateRect(h, NULL, FALSE);
            return 0;
        case WM_TIMER:
            if (w == TIMER_TOAST) clear_toast();
            return 0;
        case WM_DRAWITEM:
            if (ui_draw_item((const DRAWITEMSTRUCT *)l)) return TRUE;
            break;
        case WM_COMMAND:
            if (LOWORD(w) == ID_TOAST_ACTION) {
                void (*action)(void) = g_toast_action;
                clear_toast();
                if (action) action();
                return 0;
            }
            break;
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            if (!is_running_elevated() && PtInRect(&g_priv_pill, p)) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONUP: {
            POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            if (!is_running_elevated() && PtInRect(&g_priv_pill, p)) {
                gui_offer_elevation("Changing system-wide settings and services");
            }
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

/* =======================================================================
 * Banners: not elevated, restart pending
 * ===================================================================== */

typedef struct {
    HWND hwnd, action, close;
    UiTone tone;
    WCHAR glyph;
    char text[400];
    int visible;
    int dismissed;
} Banner;

enum { BANNER_ADMIN, BANNER_RESTART, BANNER_COUNT };
static Banner g_banners[BANNER_COUNT];

static void request_restart(void);

static LRESULT CALLBACK banner_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Banner *b = (Banner *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)l)->lpCreateParams);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            UiBuffer buf;
            HDC dc = ui_buffer_begin(&buf, hdc, &rc);
            ui_fill_rect(dc, &rc, ui_tone_soft(b->tone));
            ui_hline(dc, rc.left, rc.right, rc.bottom - 1, ui_mix(ui_tone_color(b->tone), g_ui.bg, 22));
            RECT gr = { ui_px(20), rc.top, ui_px(44), rc.bottom };
            ui_glyph(dc, b->glyph, &gr, FONT_ICON, ui_tone_color(b->tone));
            RECT ar;
            GetWindowRect(b->action, &ar);
            MapWindowPoints(NULL, h, (POINT *)&ar, 2);
            RECT tr = { ui_px(52), rc.top, ar.left - ui_px(16), rc.bottom };
            ui_text(dc, b->text, &tr, FONT_BODY, g_ui.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            ui_buffer_end(&buf);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE: {
            int width = LOWORD(l), height = HIWORD(l);
            int bh = ui_px(30);
            int x = width - ui_px(16);
            if (b->close) {
                x -= ui_px(30);
                MoveWindow(b->close, x, (height - ui_px(30)) / 2, ui_px(30), ui_px(30), TRUE);
                x -= ui_px(8);
            }
            int aw = ui_button_width(b->action);
            MoveWindow(b->action, x - aw, (height - bh) / 2, aw, bh, TRUE);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        case WM_DRAWITEM:
            if (ui_draw_item((const DRAWITEMSTRUCT *)l)) return TRUE;
            break;
        case WM_COMMAND:
            if (LOWORD(w) == ID_BANNER_CLOSE) {
                b->dismissed = 1;
                gui_state_changed();
                return 0;
            }
            if (LOWORD(w) == ID_BANNER_ACTION) {
                if (b == &g_banners[BANNER_ADMIN]) gui_relaunch_elevated();
                else request_restart();
                return 0;
            }
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

static void create_banner(Banner *b, UiTone tone, WCHAR glyph, const char *action, int closable) {
    b->tone = tone;
    b->glyph = glyph;
    b->hwnd = CreateWindowExW(0, BANNER_CLASS, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0,
                              g_main, NULL, GetModuleHandleW(NULL), b);
    b->action = ui_button(b->hwnd, ID_BANNER_ACTION, BTN_SECONDARY, 0, action);
    ui_button_set_backdrop(b->action, BACK_TONE_SOFT);
    ui_button_set_tone(b->action, tone);
    if (closable) {
        b->close = ui_button(b->hwnd, ID_BANNER_CLOSE, BTN_ICON, GLYPH_CLOSE, "");
        ui_button_set_backdrop(b->close, BACK_TONE_SOFT);
        ui_button_set_tone(b->close, tone);
    }
}

static int update_banners(void) {
    int changed = 0;

    Banner *a = &g_banners[BANNER_ADMIN];
    int want = !is_running_elevated() && !a->dismissed;
    if (want) {
        int admin = 0;
        for (int i = 0; i < g_tweak_count; i++) admin += g_tweaks[i].requires_admin;
        _snprintf(a->text, sizeof(a->text),
                  "Running without administrator rights \xE2\x80\x94 %d of %d tweaks and all service changes are unavailable.",
                  admin, g_tweak_count);
        a->text[sizeof(a->text) - 1] = '\0';
    }
    if (want != a->visible) { a->visible = want; changed = 1; }

    Banner *r = &g_banners[BANNER_RESTART];
    int pending = gui_pending_restart_count();
    want = pending > 0;
    if (want) {
        int n = _snprintf(r->text, sizeof(r->text), "Restart Windows to finish applying: ");
        int first = 1;
        for (int i = 0; i < g_tweak_count && n > 0 && n < (int)sizeof(r->text) - 4; i++) {
            if (!gui_restart_pending_for(i)) continue;
            n += _snprintf(r->text + n, sizeof(r->text) - n, "%s%s", first ? "" : ", ", g_tweaks[i].name);
            first = 0;
        }
        r->text[sizeof(r->text) - 1] = '\0';
    }
    if (want != r->visible) { r->visible = want; changed = 1; }

    for (int i = 0; i < BANNER_COUNT; i++) {
        if (g_banners[i].hwnd) InvalidateRect(g_banners[i].hwnd, NULL, FALSE);
    }
    return changed;
}

/* =======================================================================
 * Sidebar
 * ===================================================================== */

#define HOT_THEME 100
static int g_side_hot = -1;
static int g_side_tracking;

static RECT nav_rect(int i) {
    RECT r;
    r.left = ui_px(12);
    r.right = ui_px(SIDEBAR_W) - ui_px(12);
    r.top = ui_px(NAV_TOP) + i * ui_px(NAV_H + NAV_GAP);
    r.bottom = r.top + ui_px(NAV_H);
    return r;
}

static RECT theme_rect(void) {
    RECT rc, r;
    GetClientRect(g_sidebar, &rc);
    r.left = ui_px(12);
    r.right = ui_px(SIDEBAR_W) - ui_px(12);
    r.bottom = rc.bottom - ui_px(14);
    r.top = r.bottom - ui_px(38);
    return r;
}

static const char *theme_label(void) {
    switch (ui_theme_mode()) {
        case THEME_DARK:  return "Theme: Dark";
        case THEME_LIGHT: return "Theme: Light";
        default:          return g_ui.dark ? "Theme: System (dark)" : "Theme: System (light)";
    }
}

static void sidebar_paint(HWND h) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    UiBuffer buf;
    HDC dc = ui_buffer_begin(&buf, hdc, &rc);
    ui_fill_rect(dc, &rc, g_ui.sidebar);

    RECT logo = { ui_px(22), ui_px(26), ui_px(22) + ui_px(40), ui_px(26) + ui_px(40) };
    ui_round_fill(dc, &logo, ui_px(11), g_ui.accent);
    ui_glyph(dc, GLYPH_BOLT, &logo, FONT_ICON_LARGE, g_ui.accent_text);
    RECT name = { logo.right + ui_px(12), logo.top - ui_px(1), rc.right - ui_px(12), logo.top + ui_px(22) };
    ui_text(dc, APP_NAME, &name, FONT_HEADING, g_ui.text, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    RECT ver = { name.left, name.bottom + ui_px(1), name.right, logo.bottom + ui_px(2) };
    ui_text(dc, "v" APP_VERSION " \xC2\xB7 Windows 11", &ver, FONT_SMALL, g_ui.text_faint, DT_LEFT | DT_TOP | DT_SINGLELINE);

    for (int i = 0; i < PAGE_COUNT; i++) {
        RECT r = nav_rect(i);
        int sel = (i == g_current);
        int hot = (i == g_side_hot);
        if (sel) {
            ui_round_fill(dc, &r, ui_px(8), g_ui.surface2);
            RECT bar = { r.left, r.top + ui_px(11), r.left + ui_px(3), r.bottom - ui_px(11) };
            ui_round_fill(dc, &bar, ui_px(2), g_ui.accent);
        } else if (hot) {
            ui_round_fill(dc, &r, ui_px(8), ui_mix(g_ui.surface2, g_ui.sidebar, 55));
        }
        RECT gr = { r.left + ui_px(12), r.top, r.left + ui_px(34), r.bottom };
        ui_glyph(dc, g_pages[i]->glyph, &gr, FONT_ICON, sel ? g_ui.accent : hot ? g_ui.text : g_ui.text_dim);

        char badge[32] = "";
        int bw = 0;
        if (g_pages[i]->badge && g_pages[i]->badge(badge, sizeof(badge))) {
            bw = ui_text_width(dc, badge, FONT_SMALL) + ui_px(14);
            RECT br = { r.right - ui_px(8) - bw, r.top + ui_px(10), r.right - ui_px(8), r.bottom - ui_px(10) };
            ui_round_fill(dc, &br, ui_px(10), sel ? g_ui.surface3 : ui_mix(g_ui.surface2, g_ui.sidebar, 70));
            ui_text(dc, badge, &br, FONT_SMALL, sel ? g_ui.text : g_ui.text_dim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        RECT tr = { r.left + ui_px(44), r.top, r.right - ui_px(12) - bw, r.bottom };
        ui_text(dc, g_pages[i]->title, &tr, sel ? FONT_BODY_BOLD : FONT_BODY,
                sel || hot ? g_ui.text : g_ui.text_dim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    RECT tr = theme_rect();
    RECT hint = { tr.left + ui_px(4), tr.top - ui_px(58), tr.right, tr.top - ui_px(8) };
    if (hint.top > nav_rect(PAGE_COUNT).top) {
        ui_text(dc, "Ctrl+1\xE2\x80\x93" "6 switch pages\nCtrl+F search \xC2\xB7 F5 refresh", &hint, FONT_SMALL,
                g_ui.text_faint, DT_LEFT | DT_BOTTOM | DT_WORDBREAK);
    }
    if (g_side_hot == HOT_THEME) ui_round_fill(dc, &tr, ui_px(8), ui_mix(g_ui.surface2, g_ui.sidebar, 55));
    RECT tg = { tr.left + ui_px(12), tr.top, tr.left + ui_px(34), tr.bottom };
    ui_glyph(dc, g_ui.dark ? GLYPH_MOON : GLYPH_SUN, &tg, FONT_ICON, g_ui.text_dim);
    RECT tt = { tr.left + ui_px(44), tr.top, tr.right - ui_px(8), tr.bottom };
    ui_text(dc, theme_label(), &tt, FONT_BODY, g_side_hot == HOT_THEME ? g_ui.text : g_ui.text_dim,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    ui_buffer_end(&buf);
    EndPaint(h, &ps);
}

static int sidebar_hit(int x, int y) {
    POINT p = { x, y };
    for (int i = 0; i < PAGE_COUNT; i++) {
        RECT r = nav_rect(i);
        if (PtInRect(&r, p)) return i;
    }
    RECT t = theme_rect();
    if (PtInRect(&t, p)) return HOT_THEME;
    return -1;
}

static void theme_panels(void) {
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (g_panels[i]) SetWindowTheme(g_panels[i], g_ui.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    }
}

static void apply_theme_everywhere(void) {
    ui_retheme_all();
    theme_panels();
    ui_style_toplevel(g_main);
    RedrawWindow(g_main, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

static void cycle_theme(void) {
    UiThemeMode next = (UiThemeMode)((ui_theme_mode() + 1) % 3);
    ui_set_theme_mode(next);
    config_set("theme", (int)next);
    apply_theme_everywhere();
}

static LRESULT CALLBACK sidebar_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            sidebar_paint(h);
            return 0;
        case WM_MOUSEMOVE: {
            int hit = sidebar_hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (hit != g_side_hot) {
                g_side_hot = hit;
                InvalidateRect(h, NULL, FALSE);
            }
            if (!g_side_tracking) {
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                g_side_tracking = 1;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_side_tracking = 0;
            if (g_side_hot != -1) {
                g_side_hot = -1;
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (g_side_hot != -1) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            int hit = sidebar_hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (hit == HOT_THEME) cycle_theme();
            else if (hit >= 0) gui_select_page(hit);
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

/* =======================================================================
 * Page panels
 * ===================================================================== */

/* Pages that report a content height scroll when it doesn't fit (small or high-DPI screens). */
static void panel_sync_scroll(HWND h, GuiPage *page) {
    if (!page || !page->content_height) return;
    RECT rc;
    GetClientRect(h, &rc);
    int content = page->content_height(rc.right);
    int max = content - rc.bottom;
    if (max < 0) max = 0;
    if (page->scroll_y > max) page->scroll_y = max;
    if (page->scroll_y < 0) page->scroll_y = 0;
    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMax = content > 0 ? content - 1 : 0;
    si.nPage = (UINT)rc.bottom;
    si.nPos = page->scroll_y;
    SetScrollInfo(h, SB_VERT, &si, TRUE);
}

static void panel_scroll_to(HWND h, GuiPage *page, int y) {
    RECT rc;
    GetClientRect(h, &rc);
    int max = page->content_height(rc.right) - rc.bottom;
    if (max < 0) max = 0;
    if (y > max) y = max;
    if (y < 0) y = 0;
    if (y == page->scroll_y) return;
    page->scroll_y = y;
    SetScrollPos(h, SB_VERT, y, TRUE);
    if (page->layout) page->layout(rc.right, rc.bottom);
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    GuiPage *page = (GuiPage *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_VSCROLL:
            if (page && page->content_height) {
                RECT rc;
                GetClientRect(h, &rc);
                int y = page->scroll_y;
                switch (LOWORD(w)) {
                    case SB_LINEUP:   y -= ui_px(40); break;
                    case SB_LINEDOWN: y += ui_px(40); break;
                    case SB_PAGEUP:   y -= rc.bottom * 9 / 10; break;
                    case SB_PAGEDOWN: y += rc.bottom * 9 / 10; break;
                    case SB_TOP:      y = 0; break;
                    case SB_BOTTOM:   y = 1 << 20; break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION: {
                        SCROLLINFO si;
                        memset(&si, 0, sizeof(si));
                        si.cbSize = sizeof(si);
                        si.fMask = SIF_TRACKPOS;
                        GetScrollInfo(h, SB_VERT, &si);
                        y = si.nTrackPos;
                        break;
                    }
                }
                panel_scroll_to(h, page, y);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            if (page && page->content_height) {
                int delta = GET_WHEEL_DELTA_WPARAM(w);
                panel_scroll_to(h, page, page->scroll_y - delta * ui_px(48) / WHEEL_DELTA);
                return 0;
            }
            break;
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)l)->lpCreateParams);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            UiBuffer buf;
            HDC dc = ui_buffer_begin(&buf, hdc, &rc);
            ui_fill_rect(dc, &rc, g_ui.bg);
            if (page && page->paint) page->paint(dc, &rc);
            ui_buffer_end(&buf);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE: {
            panel_sync_scroll(h, page);
            /* Showing or hiding the scrollbar resizes the client area; lay out for the final size. */
            RECT rc;
            GetClientRect(h, &rc);
            if (page && page->layout) page->layout(rc.right, rc.bottom);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) InvalidateRect(h, NULL, FALSE);
            if (page && page->command && page->command(w, l)) return 0;
            break;
        case WM_NOTIFY: {
            LRESULT result = 0;
            if (page && page->notify && page->notify((LPNMHDR)l, &result)) return result;
            break;
        }
        case WM_DRAWITEM:
            if (ui_draw_item((const DRAWITEMSTRUCT *)l)) return TRUE;
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)w, g_ui.text);
            SetBkColor((HDC)w, g_ui.surface);
            return (LRESULT)ui_brush(g_ui.surface);
        case WM_CONTEXTMENU:
            if (page && page->context_menu) {
                page->context_menu((HWND)w, GET_X_LPARAM(l), GET_Y_LPARAM(l));
                return 0;
            }
            break;
        case WM_TIMER:
            if (page && page->timer) page->timer(w);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static BOOL CALLBACK find_list_cb(HWND h, LPARAM lp) {
    WCHAR cls[32];
    GetClassNameW(h, cls, 32);
    if (wcscmp(cls, WC_LISTVIEWW) == 0 && IsWindowVisible(h)) {
        *(HWND *)lp = h;
        return FALSE;
    }
    return TRUE;
}

void gui_select_page(int index) {
    if (index < 0 || index >= PAGE_COUNT || index == g_current) return;
    if (g_current >= 0) {
        if (g_pages[g_current]->hide) g_pages[g_current]->hide();
        ShowWindow(g_panels[g_current], SW_HIDE);
    }
    g_current = index;
    if (g_pages[index]->show) g_pages[index]->show();
    ShowWindow(g_panels[index], SW_SHOW);
    InvalidateRect(g_panels[index], NULL, FALSE);
    InvalidateRect(g_sidebar, NULL, FALSE);

    /* Land keyboard focus on the page's main list so arrows/Space work straight away. */
    HWND target = NULL;
    EnumChildWindows(g_panels[index], find_list_cb, (LPARAM)&target);
    if (!target) target = GetNextDlgTabItem(g_panels[index], NULL, FALSE);
    if (target) SetFocus(target);
}

/* =======================================================================
 * State changes, scanning, elevation, restart
 * ===================================================================== */

static void layout_all(void);

void gui_state_changed(void) {
    if (update_banners()) layout_all();
    if (g_sidebar) InvalidateRect(g_sidebar, NULL, FALSE);
    if (g_current >= 0 && g_pages[g_current]->refresh) g_pages[g_current]->refresh();
    if (g_current >= 0) InvalidateRect(g_panels[g_current], NULL, FALSE);
}

typedef struct {
    int with_sysinfo;
    int initial;
    SysInfo sys;
} ScanJob;

static void scan_work(GuiJob *job) {
    ScanJob *s = (ScanJob *)job->ctx;
    gui_job_step(job, 0, 2, s->initial ? "Scanning your system \xE2\x80\x94 reading every setting live\xE2\x80\xA6"
                                       : "Re-reading every setting\xE2\x80\xA6");
    tweaks_refresh_state();
    if (s->with_sysinfo) {
        gui_job_step(job, 1, 2, "Reading hardware information\xE2\x80\xA6");
        sysinfo_collect(&s->sys);
    }
}

static void scan_done(GuiJob *job) {
    ScanJob *s = (ScanJob *)job->ctx;
    if (s->with_sysinfo) {
        g_sys = s->sys;
        g_sys_ready = 1;
    }
    if (!g_ready) {
        for (int i = 0; i < g_tweak_count && i < 128; i++) g_boot_state[i] = g_tweaks[i].enabled;
        g_ready = 1;
        gui_status("Ready \xE2\x80\x94 every state shown is read live from Windows");
    } else {
        gui_toast(TONE_GOOD, "Re-scanned \xE2\x80\x94 everything shown is current.", NULL, NULL);
    }
    gui_state_changed();
}

void gui_rescan(int with_sysinfo) {
    ScanJob *s = (ScanJob *)calloc(1, sizeof(ScanJob));
    if (!s) return;
    s->with_sysinfo = with_sysinfo;
    s->initial = !g_ready;
    gui_job_start(s->initial ? "Scanning your system" : "Re-scanning", scan_work, scan_done, s);
}

static void save_window_state(void) {
    WINDOWPLACEMENT wp;
    wp.length = sizeof(wp);
    if (GetWindowPlacement(g_main, &wp)) {
        config_set("window_x", wp.rcNormalPosition.left);
        config_set("window_y", wp.rcNormalPosition.top);
        config_set("window_w", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
        config_set("window_h", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
        config_set("window_max", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    }
    if (g_current >= 0) config_set("last_page", g_current);
    config_set("theme", (int)ui_theme_mode());
    config_save();
}

void gui_relaunch_elevated(void) {
    if (gui_job_busy()) {
        gui_toast(TONE_WARN, "Wait for the current change to finish, then try again.", NULL, NULL);
        return;
    }
    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    save_window_state();
    HINSTANCE r = ShellExecuteW(g_main, L"runas", exe, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) {
        DestroyWindow(g_main);
    } else {
        gui_toast(TONE_WARN, "Elevation was cancelled \xE2\x80\x94 still running as a standard user.", NULL, NULL);
    }
}

int gui_offer_elevation(const char *what) {
    char body[600];
    _snprintf(body, sizeof(body),
              "\xE2\x80\x9C%s\xE2\x80\x9D changes system-wide settings, which Windows only allows from an app "
              "running as administrator.\n\nRestart GameOptimizer with administrator rights? It reopens on "
              "this page.", what);
    body[sizeof(body) - 1] = '\0';
    UiDialog d;
    memset(&d, 0, sizeof(d));
    d.heading = "Administrator rights needed";
    d.body = body;
    d.ok = "Restart as administrator";
    d.cancel = "Not now";
    d.tone = TONE_WARN;
    d.glyph = GLYPH_ADMIN;
    if (ui_dialog(g_main, &d)) {
        gui_relaunch_elevated();
        return 1;
    }
    return 0;
}

static void request_restart(void) {
    char body[700];
    int n = _snprintf(body, sizeof(body),
                      "Save your work first \xE2\x80\x94 Windows closes every open app.\n\n"
                      "These changes take effect after the restart:");
    for (int i = 0; i < g_tweak_count && n > 0 && n < (int)sizeof(body) - 8; i++) {
        if (gui_restart_pending_for(i)) n += _snprintf(body + n, sizeof(body) - n, "\n\xE2\x80\xA2 %s", g_tweaks[i].name);
    }
    body[sizeof(body) - 1] = '\0';

    UiDialog d;
    memset(&d, 0, sizeof(d));
    d.heading = "Restart Windows now?";
    d.body = body;
    d.ok = "Restart now";
    d.cancel = "Later";
    d.danger = 1;
    d.tone = TONE_WARN;
    d.glyph = GLYPH_POWER;
    if (!ui_dialog(g_main, &d)) return;

    HANDLE token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(NULL, L"SeShutdownPrivilege", &tp.Privileges[0].Luid)) {
            AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
        }
        CloseHandle(token);
    }
    save_window_state();
    /* Major: application, minor: reconfigure, planned. */
    if (!ExitWindowsEx(EWX_REBOOT, 0x00040000 | 0x00000004 | 0x80000000)) {
        ui_message(g_main, TONE_BAD, "Couldn't restart Windows",
                   "Windows refused the restart request. Restart from the Start menu instead.");
    }
}

/* =======================================================================
 * Main window
 * ===================================================================== */

static void layout_all(void) {
    if (!g_sidebar) return;
    RECT rc;
    GetClientRect(g_main, &rc);
    int sw = ui_px(SIDEBAR_W);
    int sh = ui_px(STATUS_H);
    int x = sw, w = rc.right - sw;
    if (w < 0) w = 0;

    HDWP dwp = BeginDeferWindowPos(PAGE_COUNT + BANNER_COUNT + 2);
    dwp = DeferWindowPos(dwp, g_sidebar, NULL, 0, 0, sw, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    int y = 0;
    for (int i = 0; i < BANNER_COUNT; i++) {
        Banner *b = &g_banners[i];
        if (b->visible) {
            dwp = DeferWindowPos(dwp, b->hwnd, NULL, x, y, w, ui_px(BANNER_H),
                                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            y += ui_px(BANNER_H);
        } else {
            dwp = DeferWindowPos(dwp, b->hwnd, NULL, 0, 0, 0, 0,
                                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
        }
    }
    dwp = DeferWindowPos(dwp, g_statusbar, NULL, x, rc.bottom - sh, w, sh, SWP_NOZORDER | SWP_NOACTIVATE);
    int ph = rc.bottom - sh - y;
    if (ph < 0) ph = 0;
    for (int i = 0; i < PAGE_COUNT; i++) {
        dwp = DeferWindowPos(dwp, g_panels[i], NULL, x, y, w, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    EndDeferWindowPos(dwp);
}

static int handle_accelerator(MSG *m) {
    if (m->message != WM_KEYDOWN) return 0;
    if (!g_main || GetAncestor(m->hwnd, GA_ROOT) != g_main) return 0;
    int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    if (ctrl && m->wParam == VK_TAB) {
        gui_select_page((g_current + (shift ? -1 : 1) + PAGE_COUNT) % PAGE_COUNT);
        return 1;
    }
    if (ctrl && m->wParam >= '1' && m->wParam < (WPARAM)('1' + PAGE_COUNT)) {
        gui_select_page((int)(m->wParam - '1'));
        return 1;
    }
    if (ctrl && m->wParam == 'F') {
        if (g_current >= 0 && g_pages[g_current]->focus_search) g_pages[g_current]->focus_search();
        return 1;
    }
    if (m->wParam == VK_F5) {
        if (g_current >= 0 && g_pages[g_current]->reload) g_pages[g_current]->reload();
        return 1;
    }
    return 0;
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            g_main = h;
            ui_set_dpi(GetDpiForWindow(h));
            ui_style_toplevel(h);
            HINSTANCE inst = GetModuleHandleW(NULL);

            g_sidebar = CreateWindowExW(0, SIDEBAR_CLASS, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, NULL, inst, NULL);
            g_statusbar = CreateWindowExW(0, STATUS_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                          0, 0, 0, 0, h, NULL, inst, NULL);
            create_banner(&g_banners[BANNER_ADMIN], TONE_WARN, GLYPH_ADMIN, "Restart as administrator", 1);
            create_banner(&g_banners[BANNER_RESTART], TONE_ACCENT, GLYPH_POWER, "Restart now", 0);

            for (int i = 0; i < PAGE_COUNT; i++) {
                g_panels[i] = CreateWindowExW(WS_EX_CONTROLPARENT, PANEL_CLASS, L"",
                                              WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                              0, 0, 0, 0, h, NULL, inst, g_pages[i]);
                if (g_pages[i]->create) g_pages[i]->create(g_panels[i]);
            }
            theme_panels();
            update_banners();
            layout_all();
            return 0;
        }

        case WM_SIZE:
            layout_all();
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO *mmi = (MINMAXINFO *)l;
            /* Small enough for a 1080p laptop at 150% scaling; the dashboard scrolls below
             * its natural height and reflows below its natural width. */
            mmi->ptMinTrackSize.x = ui_px(920);
            mmi->ptMinTrackSize.y = ui_px(560);
            return 0;
        }

        case WM_DPICHANGED: {
            ui_set_dpi(HIWORD(w));
            RECT *r = (RECT *)l;
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout_all();
            RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
            return 0;
        }

        case WM_SETTINGCHANGE:
            if (l && wcscmp((const WCHAR *)l, L"ImmersiveColorSet") == 0 &&
                ui_theme_mode() == THEME_SYSTEM && ui_reload_theme()) {
                apply_theme_everywhere();
            }
            break;

        case WM_APP_JOB_PROGRESS:
        case WM_APP_JOB_DONE:
            gui_job_message(m, w, l);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_CLOSE:
            if (gui_job_busy()) {
                g_close_pending = 1;
                gui_toast(TONE_NEUTRAL, "Finishing the current change, then closing\xE2\x80\xA6", NULL, NULL);
                return 0;
            }
            DestroyWindow(h);
            return 0;

        case WM_DESTROY:
            save_window_state();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void register_class(const WCHAR *name, WNDPROC proc, HCURSOR cursor, HICON icon, HICON icon_small) {
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = cursor;
    wc.hIcon = icon;
    wc.hIconSm = icon_small;
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

/* Restores the saved window rectangle if it's still on a connected monitor; otherwise a
 * default size centered on the primary work area. */
static void initial_placement(HWND h, int show) {
    int x, y, w, ht, maxed = 0;
    RECT saved;
    int have = config_get("window_x", &x) && config_get("window_y", &y) &&
               config_get("window_w", &w) && config_get("window_h", &ht);
    config_get("window_max", &maxed);
    if (have && w > 200 && ht > 200) {
        SetRect(&saved, x, y, x + w, y + ht);
        if (!MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) have = 0;
    }
    if (!have) {
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        w = ui_px(1200);
        ht = ui_px(780);
        if (w > work.right - work.left - ui_px(40)) w = work.right - work.left - ui_px(40);
        if (ht > work.bottom - work.top - ui_px(40)) ht = work.bottom - work.top - ui_px(40);
        x = work.left + (work.right - work.left - w) / 2;
        y = work.top + (work.bottom - work.top - ht) / 2;
    }
    SetWindowPos(h, NULL, x, y, w, ht, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(h, maxed ? SW_SHOWMAXIMIZED : show);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)prev; (void)cmdline;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    config_load();
    ui_init();
    int theme = 0;
    if (config_get("theme", &theme) && theme >= 0 && theme <= 2) ui_set_theme_mode((UiThemeMode)theme);

    g_pages[PAGE_DASHBOARD]   = &g_page_dashboard;
    g_pages[PAGE_PERFORMANCE] = &g_page_performance;
    g_pages[PAGE_TELEMETRY]   = &g_page_telemetry;
    g_pages[PAGE_SERVICES]    = &g_page_services;
    g_pages[PAGE_PROCESSES]   = &g_page_processes;
    g_pages[PAGE_PROFILES]    = &g_page_profiles;

    /* Load each size explicitly so the title bar gets the crisp 16/20/24 px artwork rather
     * than a downscaled 32 px one. */
    HICON icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON icon_small = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!icon) icon = LoadIcon(NULL, IDI_APPLICATION);
    if (!icon_small) icon_small = icon;
    HCURSOR arrow = LoadCursor(NULL, IDC_ARROW);
    register_class(MAIN_CLASS, main_proc, arrow, icon, icon_small);
    register_class(PANEL_CLASS, panel_proc, arrow, NULL, NULL);
    register_class(SIDEBAR_CLASS, sidebar_proc, arrow, NULL, NULL);
    register_class(STATUS_CLASS, status_proc, arrow, NULL, NULL);
    register_class(BANNER_CLASS, banner_proc, arrow, NULL, NULL);

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, MAIN_CLASS, L"GameOptimizer",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1200, 780, NULL, NULL, instance, NULL);
    if (!hwnd) return 1;

    initial_placement(hwnd, show);
    int page = 0;
    config_get("last_page", &page);
    if (page < 0 || page >= PAGE_COUNT) page = 0;
    gui_select_page(page);
    UpdateWindow(hwnd);

    gui_rescan(1);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (handle_accelerator(&msg)) continue;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    ui_shutdown();
    return (int)msg.wParam;
}
