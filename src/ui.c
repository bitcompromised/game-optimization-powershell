#include "ui.h"
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

UiPalette g_ui;

static UiThemeMode g_mode = THEME_SYSTEM;
static int g_palette_built;
static UINT g_dpi = 96;
static HFONT g_fonts[FONT_COUNT];
static ULONG_PTR g_gdip_token;

static char g_face_text[LF_FACESIZE] = "Segoe UI";
static char g_face_display[LF_FACESIZE] = "Segoe UI";
static char g_face_icons[LF_FACESIZE] = "Segoe MDL2 Assets";

/* =======================================================================
 * Tracked controls: fonts are re-sent after a DPI change, lists/edits re-skinned after a
 * theme change. Dead handles are recycled lazily.
 * ===================================================================== */

enum { TRACK_PLAIN, TRACK_LIST, TRACK_EDIT };

typedef struct { HWND hwnd; int font; int kind; } Tracked;

#define UI_MAX_TRACKED 256
static Tracked g_tracked[UI_MAX_TRACKED];
static int g_tracked_count;

static void track(HWND h, int font, int kind) {
    int free_slot = -1;
    for (int i = 0; i < g_tracked_count; i++) {
        if (g_tracked[i].hwnd == h) {
            if (font >= 0) g_tracked[i].font = font;
            if (kind != TRACK_PLAIN) g_tracked[i].kind = kind;
            return;
        }
        if (free_slot < 0 && !IsWindow(g_tracked[i].hwnd)) free_slot = i;
    }
    if (free_slot < 0) {
        if (g_tracked_count >= UI_MAX_TRACKED) return;
        free_slot = g_tracked_count++;
    }
    g_tracked[free_slot].hwnd = h;
    g_tracked[free_slot].font = font;
    g_tracked[free_slot].kind = kind;
}

/* =======================================================================
 * Palette
 * ===================================================================== */

COLORREF ui_mix(COLORREF a, COLORREF b, int pa) {
    int pb = 100 - pa;
    return RGB((GetRValue(a) * pa + GetRValue(b) * pb) / 100,
               (GetGValue(a) * pa + GetGValue(b) * pb) / 100,
               (GetBValue(a) * pa + GetBValue(b) * pb) / 100);
}

static void build_palette(int dark) {
    UiPalette p;
    memset(&p, 0, sizeof(p));
    p.dark = dark;
    if (dark) {
        p.bg = HEXRGB(0x111318);       p.sidebar = HEXRGB(0x0B0D11);
        p.surface = HEXRGB(0x181B22);  p.surface2 = HEXRGB(0x20242D); p.surface3 = HEXRGB(0x2A2F3A);
        p.border = HEXRGB(0x262A34);   p.border_strong = HEXRGB(0x363C49);
        p.text = HEXRGB(0xE9EBF1);     p.text_dim = HEXRGB(0x9BA2B1); p.text_faint = HEXRGB(0x646B7A);
        p.accent = HEXRGB(0x5B8CFF);   p.accent_hover = HEXRGB(0x77A0FF);
        p.accent_pressed = HEXRGB(0x4A78E6); p.accent_text = HEXRGB(0xFFFFFF);
        p.good = HEXRGB(0x3DD68C);     p.warn = HEXRGB(0xF5B84A);     p.bad = HEXRGB(0xFF6B6B);
    } else {
        p.bg = HEXRGB(0xF3F4F7);       p.sidebar = HEXRGB(0xE8EAF0);
        p.surface = HEXRGB(0xFFFFFF);  p.surface2 = HEXRGB(0xF1F3F7); p.surface3 = HEXRGB(0xE4E8EF);
        p.border = HEXRGB(0xE0E3EA);   p.border_strong = HEXRGB(0xCBD0DA);
        p.text = HEXRGB(0x151922);     p.text_dim = HEXRGB(0x596172); p.text_faint = HEXRGB(0x99A0AD);
        p.accent = HEXRGB(0x2F6BFF);   p.accent_hover = HEXRGB(0x4A7EFF);
        p.accent_pressed = HEXRGB(0x2459D9); p.accent_text = HEXRGB(0xFFFFFF);
        p.good = HEXRGB(0x12924A);     p.warn = HEXRGB(0xB26B00);     p.bad = HEXRGB(0xD6303A);
    }
    int soft = dark ? 20 : 12;
    p.accent_soft = ui_mix(p.accent, p.surface, soft);
    p.good_soft = ui_mix(p.good, p.surface, soft);
    p.warn_soft = ui_mix(p.warn, p.surface, soft);
    p.bad_soft = ui_mix(p.bad, p.surface, soft);
    g_ui = p;
    g_palette_built = 1;
}

COLORREF ui_tone_color(UiTone tone) {
    switch (tone) {
        case TONE_ACCENT: return g_ui.accent;
        case TONE_GOOD:   return g_ui.good;
        case TONE_WARN:   return g_ui.warn;
        case TONE_BAD:    return g_ui.bad;
        default:          return g_ui.text_dim;
    }
}

COLORREF ui_tone_soft(UiTone tone) {
    switch (tone) {
        case TONE_ACCENT: return g_ui.accent_soft;
        case TONE_GOOD:   return g_ui.good_soft;
        case TONE_WARN:   return g_ui.warn_soft;
        case TONE_BAD:    return g_ui.bad_soft;
        default:          return g_ui.surface3;
    }
}

typedef struct { COLORREF c; HBRUSH b; } CachedBrush;
static CachedBrush g_brushes[32];
static int g_brush_count;

static void clear_brushes(void) {
    for (int i = 0; i < g_brush_count; i++) DeleteObject(g_brushes[i].b);
    g_brush_count = 0;
}

HBRUSH ui_brush(COLORREF c) {
    for (int i = 0; i < g_brush_count; i++) {
        if (g_brushes[i].c == c) return g_brushes[i].b;
    }
    /* A full cache means a theme change left stale colors behind; brushes handed out earlier
     * are only used within the WM_CTLCOLOR* that asked for them, so recycling is safe. */
    if (g_brush_count >= (int)(sizeof(g_brushes) / sizeof(g_brushes[0]))) clear_brushes();
    g_brushes[g_brush_count].c = c;
    g_brushes[g_brush_count].b = CreateSolidBrush(c);
    return g_brushes[g_brush_count++].b;
}

static int system_prefers_dark(void) {
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueA(HKEY_CURRENT_USER,
                     "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     "AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &size) != ERROR_SUCCESS) {
        return 0;
    }
    return v == 0;
}

static long windows_build(void) {
    char build[32];
    DWORD size = sizeof(build);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     "CurrentBuildNumber", RRF_RT_REG_SZ, NULL, build, &size) != ERROR_SUCCESS) {
        return 0;
    }
    return atol(build);
}

/* Popup menus and scrollbars only go dark if the process opts in through uxtheme's unnamed
 * SetPreferredAppMode export (ordinal 135, Windows 10 1903+). There is no documented
 * equivalent; without it context menus stay light on a dark window. */
typedef int  (WINAPI *SetPreferredAppModeFn)(int mode);
typedef void (WINAPI *FlushMenuThemesFn)(void);

static void set_app_dark_mode(int dark) {
    static long build = -1;
    if (build < 0) build = windows_build();
    if (build < 18362) return;

    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    SetPreferredAppModeFn set_mode =
        (SetPreferredAppModeFn)(void (*)(void))GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemesFn flush =
        (FlushMenuThemesFn)(void (*)(void))GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set_mode) set_mode(dark ? 2 : 3);   /* ForceDark / ForceLight */
    if (flush) flush();
}

int ui_reload_theme(void) {
    int dark = g_mode == THEME_DARK ? 1 : g_mode == THEME_LIGHT ? 0 : system_prefers_dark();
    int changed = !g_palette_built || dark != g_ui.dark;
    build_palette(dark);
    set_app_dark_mode(dark);
    clear_brushes();
    return changed;
}

void ui_set_theme_mode(UiThemeMode mode) {
    g_mode = mode;
    ui_reload_theme();
}

UiThemeMode ui_theme_mode(void) { return g_mode; }

void ui_style_toplevel(HWND hwnd) {
    BOOL dark = g_ui.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = g_ui.bg, text = g_ui.text, border = g_ui.border_strong;
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
}

/* =======================================================================
 * DPI and fonts
 * ===================================================================== */

static int CALLBACK font_found_cb(const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM lp) {
    (void)lf; (void)tm; (void)type;
    *(int *)lp = 1;
    return 0;
}

static int font_installed(const char *face) {
    LOGFONTA lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    strncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    int found = 0;
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExA(dc, &lf, font_found_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static HFONT make_font(const char *face, int px, int weight) {
    return CreateFontA(-ui_px(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

static void create_fonts(HFONT *out) {
    out[FONT_BODY]        = make_font(g_face_text, 14, FW_NORMAL);
    out[FONT_BODY_BOLD]   = make_font(g_face_text, 14, FW_SEMIBOLD);
    out[FONT_SMALL]       = make_font(g_face_text, 12, FW_NORMAL);
    out[FONT_SMALL_BOLD]  = make_font(g_face_text, 12, FW_SEMIBOLD);
    out[FONT_HEADING]     = make_font(g_face_display, 17, FW_SEMIBOLD);
    out[FONT_TITLE]       = make_font(g_face_display, 26, FW_SEMIBOLD);
    out[FONT_DISPLAY]     = make_font(g_face_display, 30, FW_SEMIBOLD);
    out[FONT_ICON]        = make_font(g_face_icons, 16, FW_NORMAL);
    out[FONT_ICON_SMALL]  = make_font(g_face_icons, 12, FW_NORMAL);
    out[FONT_ICON_LARGE]  = make_font(g_face_icons, 20, FW_NORMAL);
}

typedef struct {
    int hot;
    int row_height;      /* logical px */
    int fill_width;
    int tracking;
    HIMAGELIST himl;
    HWND header;
    char empty[256];
} ListSkin;

static LRESULT CALLBACK list_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static void list_apply_row_height(HWND list, ListSkin *s);

void ui_set_dpi(UINT dpi) {
    if (dpi == 0) dpi = 96;
    int first = (g_fonts[0] == NULL);
    if (!first && dpi == g_dpi) return;
    g_dpi = dpi;

    HFONT old[FONT_COUNT];
    memcpy(old, g_fonts, sizeof(old));
    create_fonts(g_fonts);

    for (int i = 0; i < g_tracked_count; i++) {
        Tracked *t = &g_tracked[i];
        if (!IsWindow(t->hwnd)) continue;
        if (t->font >= 0) SendMessageW(t->hwnd, WM_SETFONT, (WPARAM)g_fonts[t->font], TRUE);
        if (t->kind == TRACK_LIST) {
            DWORD_PTR ref = 0;
            if (GetWindowSubclass(t->hwnd, list_proc, 1, &ref)) list_apply_row_height(t->hwnd, (ListSkin *)ref);
        }
    }
    if (!first) {
        for (int i = 0; i < FONT_COUNT; i++) DeleteObject(old[i]);
    }
}

UINT ui_dpi(void) { return g_dpi; }
int ui_px(int v) { return MulDiv(v, (int)g_dpi, 96); }

HFONT ui_font(UiFont f) { return g_fonts[f]; }

int ui_font_height(UiFont f) {
    HDC dc = GetDC(NULL);
    HFONT old = (HFONT)SelectObject(dc, g_fonts[f]);
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return tm.tmHeight;
}

void ui_use_font(HWND control, UiFont f) {
    SendMessageW(control, WM_SETFONT, (WPARAM)g_fonts[f], TRUE);
    track(control, (int)f, TRACK_PLAIN);
}

void ui_init(void) {
    GdiplusStartupInput in;
    memset(&in, 0, sizeof(in));
    in.GdiplusVersion = 1;
    GdiplusStartup(&g_gdip_token, &in, NULL);

    if (font_installed("Segoe UI Variable Text")) strcpy(g_face_text, "Segoe UI Variable Text");
    if (font_installed("Segoe UI Variable Display")) strcpy(g_face_display, "Segoe UI Variable Display");
    if (font_installed("Segoe Fluent Icons")) strcpy(g_face_icons, "Segoe Fluent Icons");

    ui_reload_theme();
}

void ui_shutdown(void) {
    for (int i = 0; i < FONT_COUNT; i++) {
        if (g_fonts[i]) DeleteObject(g_fonts[i]);
        g_fonts[i] = NULL;
    }
    clear_brushes();
    if (g_gdip_token) GdiplusShutdown(g_gdip_token);
    g_gdip_token = 0;
}

/* =======================================================================
 * Text
 * ===================================================================== */

/* UTF-8 first; strings that aren't valid UTF-8 came from an ANSI API (process or service
 * names), so they're converted from the active code page instead. */
int ui_widen(const char *s, WCHAR *out, int cap) {
    if (cap <= 0) return 0;
    if (!s) { out[0] = 0; return 0; }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap);
    if (n == 0 && GetLastError() == ERROR_NO_UNICODE_TRANSLATION) {
        n = MultiByteToWideChar(CP_ACP, 0, s, -1, out, cap);
    }
    if (n == 0) {
        /* Too long for the buffer: keep what fits. */
        out[cap - 1] = 0;
        return (int)wcslen(out);
    }
    return n - 1;
}

void ui_narrow(const WCHAR *w, char *out, int cap) {
    if (cap <= 0) return;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) == 0) out[0] = '\0';
    out[cap - 1] = '\0';
}

/* Converts into a stack buffer when it fits, otherwise a heap one the caller frees. */
static WCHAR *widen_tmp(const char *s, WCHAR *stack, int cap) {
    int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (need <= cap) {
        ui_widen(s, stack, cap);
        return stack;
    }
    WCHAR *heap = (WCHAR *)malloc((size_t)need * sizeof(WCHAR));
    if (!heap) {
        ui_widen(s, stack, cap);
        return stack;
    }
    ui_widen(s, heap, need);
    return heap;
}

void ui_text(HDC dc, const char *s, const RECT *rc, UiFont f, COLORREF c, UINT format) {
    if (!s || !*s) return;
    WCHAR stack[512];
    WCHAR *w = widen_tmp(s, stack, 512);
    HFONT old = (HFONT)SelectObject(dc, g_fonts[f]);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    RECT r = *rc;
    DrawTextW(dc, w, -1, &r, format | DT_NOPREFIX);
    SelectObject(dc, old);
    if (w != stack) free(w);
}

int ui_text_width(HDC dc, const char *s, UiFont f) {
    if (!s || !*s) return 0;
    WCHAR stack[512];
    WCHAR *w = widen_tmp(s, stack, 512);
    HFONT old = (HFONT)SelectObject(dc, g_fonts[f]);
    RECT r = { 0, 0, 0, 0 };
    DrawTextW(dc, w, -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    if (w != stack) free(w);
    return r.right - r.left;
}

int ui_text_height(HDC dc, const char *s, UiFont f, int width) {
    if (!s || !*s) return 0;
    WCHAR stack[512];
    WCHAR *w = widen_tmp(s, stack, 512);
    HFONT old = (HFONT)SelectObject(dc, g_fonts[f]);
    RECT r = { 0, 0, width, 0 };
    DrawTextW(dc, w, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    if (w != stack) free(w);
    return r.bottom - r.top;
}

void ui_glyph(HDC dc, WCHAR glyph, const RECT *rc, UiFont f, COLORREF c) {
    WCHAR s[2] = { glyph, 0 };
    HFONT old = (HFONT)SelectObject(dc, g_fonts[f]);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    RECT r = *rc;
    DrawTextW(dc, s, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

int ui_stristr(const char *hay, const char *needle) {
    if (!needle || !*needle) return 1;
    if (!hay) return 0;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (_strnicmp(p, needle, n) == 0) return 1;
    }
    return 0;
}

/* =======================================================================
 * Shapes (GDI+ for anything curved, so edges are anti-aliased)
 * ===================================================================== */

static ARGB argb(COLORREF c, BYTE a) {
    return ((ARGB)a << 24) | ((ARGB)GetRValue(c) << 16) | ((ARGB)GetGValue(c) << 8) | GetBValue(c);
}

static GpGraphics *gfx(HDC dc) {
    GpGraphics *g = NULL;
    if (GdipCreateFromHDC(dc, &g) != Ok) return NULL;
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    return g;
}

static GpPath *round_path(REAL x, REAL y, REAL w, REAL h, REAL r) {
    GpPath *p = NULL;
    GdipCreatePath(FillModeAlternate, &p);
    if (!p) return NULL;
    REAL d = r * 2;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d <= 0.5f) {
        GdipAddPathRectangle(p, x, y, w, h);
        return p;
    }
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p);
    return p;
}

void ui_fill_rect(HDC dc, const RECT *rc, COLORREF c) {
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, rc, NULL, 0, NULL);
}

void ui_hline(HDC dc, int x0, int x1, int y, COLORREF c) {
    RECT r = { x0, y, x1, y + 1 };
    ui_fill_rect(dc, &r, c);
}

static void fill_path_rect(GpGraphics *g, const RECT *rc, int radius, COLORREF c) {
    GpSolidFill *b = NULL;
    GdipCreateSolidFill(argb(c, 255), &b);
    GpPath *p = round_path((REAL)rc->left, (REAL)rc->top, (REAL)(rc->right - rc->left),
                           (REAL)(rc->bottom - rc->top), (REAL)radius);
    if (p && b) GdipFillPath(g, (GpBrush *)b, p);
    if (p) GdipDeletePath(p);
    if (b) GdipDeleteBrush((GpBrush *)b);
}

static void frame_path_rect(GpGraphics *g, const RECT *rc, int radius, COLORREF c, int width) {
    GpPen *pen = NULL;
    GdipCreatePen1(argb(c, 255), (REAL)width, UnitPixel, &pen);
    REAL half = width / 2.0f;
    GpPath *p = round_path(rc->left + half, rc->top + half, (rc->right - rc->left) - (REAL)width,
                           (rc->bottom - rc->top) - (REAL)width, radius - half);
    if (p && pen) GdipDrawPath(g, pen, p);
    if (p) GdipDeletePath(p);
    if (pen) GdipDeletePen(pen);
}

void ui_round_fill(HDC dc, const RECT *rc, int radius, COLORREF c) {
    GpGraphics *g = gfx(dc);
    if (!g) return;
    fill_path_rect(g, rc, radius, c);
    GdipDeleteGraphics(g);
}

void ui_round_frame(HDC dc, const RECT *rc, int radius, COLORREF c, int width) {
    GpGraphics *g = gfx(dc);
    if (!g) return;
    frame_path_rect(g, rc, radius, c, width);
    GdipDeleteGraphics(g);
}

void ui_round_fill_frame(HDC dc, const RECT *rc, int radius, COLORREF fill, COLORREF frame) {
    GpGraphics *g = gfx(dc);
    if (!g) return;
    fill_path_rect(g, rc, radius, fill);
    frame_path_rect(g, rc, radius, frame, 1);
    GdipDeleteGraphics(g);
}

void ui_ellipse(HDC dc, const RECT *rc, COLORREF c) {
    GpGraphics *g = gfx(dc);
    if (!g) return;
    GpSolidFill *b = NULL;
    GdipCreateSolidFill(argb(c, 255), &b);
    if (b) {
        GdipFillEllipseI(g, (GpBrush *)b, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top);
        GdipDeleteBrush((GpBrush *)b);
    }
    GdipDeleteGraphics(g);
}

void ui_ring(HDC dc, const RECT *rc, int thickness, double fraction, COLORREF track, COLORREF fill) {
    GpGraphics *g = gfx(dc);
    if (!g) return;
    REAL half = thickness / 2.0f;
    REAL x = rc->left + half, y = rc->top + half;
    REAL w = (rc->right - rc->left) - (REAL)thickness, h = (rc->bottom - rc->top) - (REAL)thickness;

    GpPen *pen = NULL;
    GdipCreatePen1(argb(track, 255), (REAL)thickness, UnitPixel, &pen);
    if (pen) {
        GdipDrawEllipse(g, pen, x, y, w, h);
        GdipDeletePen(pen);
    }
    if (fraction > 0.0) {
        if (fraction > 1.0) fraction = 1.0;
        pen = NULL;
        GdipCreatePen1(argb(fill, 255), (REAL)thickness, UnitPixel, &pen);
        if (pen) {
            GdipSetPenStartCap(pen, LineCapRound);
            GdipSetPenEndCap(pen, LineCapRound);
            GdipDrawArc(g, pen, x, y, w, h, -90.0f, (REAL)(360.0 * fraction));
            GdipDeletePen(pen);
        }
    }
    GdipDeleteGraphics(g);
}

void ui_sparkline(HDC dc, const RECT *rc, const float *values, int count, float max, COLORREF c) {
    if (count < 2 || max <= 0) return;
    GpGraphics *g = gfx(dc);
    if (!g) return;

    GpPointF pts[128 + 2];
    if (count > 128) {
        values += count - 128;
        count = 128;
    }
    REAL w = (REAL)(rc->right - rc->left), h = (REAL)(rc->bottom - rc->top);
    for (int i = 0; i < count; i++) {
        float v = values[i] / max;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        pts[i].X = rc->left + w * i / (count - 1);
        pts[i].Y = rc->bottom - 1 - (h - 2) * v;
    }

    /* Soft area under the line, then the line itself. */
    pts[count].X = (REAL)rc->right;
    pts[count].Y = (REAL)rc->bottom;
    pts[count + 1].X = (REAL)rc->left;
    pts[count + 1].Y = (REAL)rc->bottom;
    GpSolidFill *b = NULL;
    GdipCreateSolidFill(argb(c, g_ui.dark ? 40 : 34), &b);
    if (b) {
        GdipFillPolygon(g, (GpBrush *)b, pts, count + 2, FillModeAlternate);
        GdipDeleteBrush((GpBrush *)b);
    }
    GpPen *pen = NULL;
    GdipCreatePen1(argb(c, 255), (REAL)ui_px(2), UnitPixel, &pen);
    if (pen) {
        GdipSetPenLineJoin(pen, LineJoinRound);
        GdipDrawLines(g, pen, pts, count);
        GdipDeletePen(pen);
    }
    GdipDeleteGraphics(g);
}

void ui_progress(HDC dc, const RECT *rc, double fraction, COLORREF track, COLORREF fill) {
    int h = rc->bottom - rc->top;
    ui_round_fill(dc, rc, h / 2, track);
    if (fraction <= 0) return;
    if (fraction > 1) fraction = 1;
    RECT f = *rc;
    int w = (int)((rc->right - rc->left) * fraction + 0.5);
    if (w < h) w = h;
    f.right = f.left + w;
    ui_round_fill(dc, &f, h / 2, fill);
}

void ui_card(HDC dc, const RECT *rc) {
    ui_round_fill_frame(dc, rc, ui_px(10), g_ui.surface, g_ui.border);
}

int ui_pill_width(HDC dc, const char *text) {
    return ui_text_width(dc, text, FONT_SMALL_BOLD) + ui_px(16);
}

int ui_pill(HDC dc, int x, int cy, const char *text, UiTone tone, int anchor_right) {
    int w = ui_pill_width(dc, text);
    int h = ui_px(20);
    RECT r;
    r.left = anchor_right ? x - w : x;
    r.right = r.left + w;
    r.top = cy - h / 2;
    r.bottom = r.top + h;
    ui_round_fill(dc, &r, h / 2, ui_tone_soft(tone));
    ui_text(dc, text, &r, FONT_SMALL_BOLD, tone == TONE_NEUTRAL ? g_ui.text_dim : ui_tone_color(tone),
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return w;
}

void ui_switch(HDC dc, int x, int y, int on, int enabled, int hot) {
    int w = ui_px(UI_SWITCH_W), h = ui_px(UI_SWITCH_H);
    RECT track = { x, y, x + w, y + h };
    int knob = h - ui_px(on ? 8 : 10);
    RECT k;
    k.top = y + (h - knob) / 2;
    k.bottom = k.top + knob;

    if (on) {
        /* A locked switch keeps its real state readable, just muted. */
        COLORREF fill = !enabled ? ui_mix(g_ui.accent, g_ui.surface, 38) : hot ? g_ui.accent_hover : g_ui.accent;
        ui_round_fill(dc, &track, h / 2, fill);
        k.right = x + w - ui_px(4);
        k.left = k.right - knob;
        ui_ellipse(dc, &k, enabled ? g_ui.accent_text : ui_mix(g_ui.accent_text, g_ui.surface, 55));
    } else {
        COLORREF frame = !enabled ? g_ui.border_strong : hot ? g_ui.text : g_ui.text_dim;
        ui_round_fill(dc, &track, h / 2, hot && enabled ? g_ui.surface3 : g_ui.surface2);
        ui_round_frame(dc, &track, h / 2, frame, 1);
        k.left = x + ui_px(5);
        k.right = k.left + knob;
        ui_ellipse(dc, &k, frame);
    }
}

/* =======================================================================
 * Double buffering
 * ===================================================================== */

HDC ui_buffer_begin(UiBuffer *b, HDC target, const RECT *rc) {
    b->target = target;
    b->rc = *rc;
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    b->dc = CreateCompatibleDC(target);
    b->bmp = CreateCompatibleBitmap(target, w, h);
    b->old = (HBITMAP)SelectObject(b->dc, b->bmp);
    SetBkMode(b->dc, TRANSPARENT);
    return b->dc;
}

void ui_buffer_end(UiBuffer *b) {
    BitBlt(b->target, b->rc.left, b->rc.top, b->rc.right - b->rc.left, b->rc.bottom - b->rc.top,
           b->dc, 0, 0, SRCCOPY);
    SelectObject(b->dc, b->old);
    DeleteObject(b->bmp);
    DeleteDC(b->dc);
}

/* =======================================================================
 * Owner-drawn buttons
 * ===================================================================== */

typedef struct {
    UiButtonKind kind;
    UiBackdrop backdrop;
    UiTone tone;
    WCHAR glyph;
    int checked;
    int hot;
    char text[128];
    char sub[192];
} UiButton;

static LRESULT CALLBACK button_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    UiButton *b = (UiButton *)ref;
    switch (m) {
        case WM_MOUSEMOVE:
            if (!b->hot) {
                b->hot = 1;
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                InvalidateRect(h, NULL, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            b->hot = 0;
            InvalidateRect(h, NULL, FALSE);
            break;
        case WM_LBUTTONDBLCLK:
            /* Owner-drawn buttons turn a quick second click into BN_DOUBLECLICKED; treat it as a
             * click so rapid presses aren't swallowed. */
            return DefSubclassProc(h, WM_LBUTTONDOWN, w, l);
        case WM_ERASEBKGND:
            return 1;
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, button_proc, id);
            free(b);
            break;
    }
    return DefSubclassProc(h, m, w, l);
}

static UiButton *button_state(HWND h) {
    DWORD_PTR ref = 0;
    if (!h || !GetWindowSubclass(h, button_proc, 1, &ref)) return NULL;
    return (UiButton *)ref;
}

HWND ui_button(HWND parent, int id, UiButtonKind kind, WCHAR glyph, const char *text) {
    WCHAR wtext[128];
    ui_widen(text ? text : "", wtext, 128);
    HWND h = CreateWindowExW(0, L"BUTTON", wtext, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    UiButton *b = (UiButton *)calloc(1, sizeof(UiButton));
    b->kind = kind;
    b->glyph = glyph;
    b->backdrop = BACK_BG;
    b->tone = TONE_ACCENT;
    strncpy(b->text, text ? text : "", sizeof(b->text) - 1);
    SetWindowSubclass(h, button_proc, 1, (DWORD_PTR)b);
    ui_use_font(h, FONT_BODY);
    return h;
}

void ui_button_set_text(HWND h, const char *text) {
    UiButton *b = button_state(h);
    if (!b || strcmp(b->text, text) == 0) return;
    strncpy(b->text, text, sizeof(b->text) - 1);
    WCHAR w[128];
    ui_widen(text, w, 128);
    SetWindowTextW(h, w);
    InvalidateRect(h, NULL, FALSE);
}

void ui_button_set_sub(HWND h, const char *sub) {
    UiButton *b = button_state(h);
    if (!b || strcmp(b->sub, sub) == 0) return;
    strncpy(b->sub, sub, sizeof(b->sub) - 1);
    InvalidateRect(h, NULL, FALSE);
}

void ui_button_set_glyph(HWND h, WCHAR glyph) {
    UiButton *b = button_state(h);
    if (!b || b->glyph == glyph) return;
    b->glyph = glyph;
    InvalidateRect(h, NULL, FALSE);
}

void ui_button_set_tone(HWND h, UiTone tone) {
    UiButton *b = button_state(h);
    if (!b || b->tone == tone) return;
    b->tone = tone;
    InvalidateRect(h, NULL, FALSE);
}

void ui_button_set_kind(HWND h, UiButtonKind kind) {
    UiButton *b = button_state(h);
    if (!b || b->kind == kind) return;
    b->kind = kind;
    InvalidateRect(h, NULL, FALSE);
}

void ui_button_set_backdrop(HWND h, UiBackdrop backdrop) {
    UiButton *b = button_state(h);
    if (!b) return;
    b->backdrop = backdrop;
    InvalidateRect(h, NULL, FALSE);
}

void ui_button_set_checked(HWND h, int checked) {
    UiButton *b = button_state(h);
    if (!b || b->checked == !!checked) return;
    b->checked = !!checked;
    InvalidateRect(h, NULL, FALSE);
}

int ui_button_checked(HWND h) {
    UiButton *b = button_state(h);
    return b ? b->checked : 0;
}

int ui_button_width(HWND h) {
    UiButton *b = button_state(h);
    if (!b) return ui_px(100);
    /* Measure with the font the label is drawn in (see ui_draw_item). */
    UiFont font = b->kind == BTN_CHIP ? FONT_SMALL_BOLD
                : (b->kind == BTN_PRIMARY || b->kind == BTN_DANGER || b->kind == BTN_LINK) ? FONT_BODY_BOLD
                : FONT_BODY;
    HDC dc = GetDC(h);
    int text_w = ui_text_width(dc, b->text, font);
    ReleaseDC(h, dc);
    int pad = b->kind == BTN_CHIP ? ui_px(12) : b->kind == BTN_LINK ? ui_px(8) : ui_px(16);
    int glyph_w = b->glyph ? ui_px(16) + (b->text[0] ? ui_px(8) : 0) : 0;
    if (b->kind == BTN_ICON || !b->text[0]) return ui_px(34);
    return pad * 2 + glyph_w + text_w;
}

static COLORREF backdrop_color(const UiButton *b) {
    switch (b->backdrop) {
        case BACK_SURFACE:   return g_ui.surface;
        case BACK_SIDEBAR:   return g_ui.sidebar;
        case BACK_TONE_SOFT: return ui_tone_soft(b->tone);
        default:             return g_ui.bg;
    }
}

static void draw_tile(HDC dc, const RECT *rc, const UiButton *b, int hot, int pressed, int disabled) {
    int radius = ui_px(10);
    COLORREF fill = pressed ? g_ui.surface3 : hot ? g_ui.surface2 : g_ui.surface;
    COLORREF frame = hot && !disabled ? ui_mix(ui_tone_color(b->tone), g_ui.border, 45) : g_ui.border;
    ui_round_fill_frame(dc, rc, radius, fill, frame);

    int pad = ui_px(14);
    int icon = ui_px(36);
    RECT ic = { rc->left + pad, rc->top + (rc->bottom - rc->top - icon) / 2, 0, 0 };
    ic.right = ic.left + icon;
    ic.bottom = ic.top + icon;
    ui_round_fill(dc, &ic, ui_px(10), disabled ? g_ui.surface3 : ui_tone_soft(b->tone));
    ui_glyph(dc, b->glyph, &ic, FONT_ICON_LARGE, disabled ? g_ui.text_faint : ui_tone_color(b->tone));

    int tx = ic.right + ui_px(12);
    int title_h = ui_font_height(FONT_BODY_BOLD);
    int sub_h = b->sub[0] ? ui_font_height(FONT_SMALL) : 0;
    int block = title_h + (sub_h ? ui_px(2) + sub_h : 0);
    int ty = rc->top + (rc->bottom - rc->top - block) / 2;
    RECT tr = { tx, ty, rc->right - ui_px(10), ty + title_h };
    ui_text(dc, b->text, &tr, FONT_BODY_BOLD, disabled ? g_ui.text_faint : g_ui.text,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (sub_h) {
        RECT sr = { tx, tr.bottom + ui_px(2), rc->right - ui_px(10), tr.bottom + ui_px(2) + sub_h };
        ui_text(dc, b->sub, &sr, FONT_SMALL, g_ui.text_dim, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

int ui_draw_item(const DRAWITEMSTRUCT *dis) {
    if (dis->CtlType != ODT_BUTTON) return 0;
    UiButton *b = button_state(dis->hwndItem);
    if (!b) return 0;

    RECT rc = dis->rcItem;
    UiBuffer buf;
    HDC dc = ui_buffer_begin(&buf, dis->hDC, &rc);
    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    int disabled = (dis->itemState & ODS_DISABLED) != 0;
    int focus = (dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT);
    int hot = b->hot && !disabled;
    COLORREF back = backdrop_color(b);
    ui_fill_rect(dc, &rc, back);

    int h = rc.bottom - rc.top;
    int radius = ui_px(6);
    COLORREF fill = back, frame = CLR_NONE, fg = g_ui.text;
    UiFont font = FONT_BODY;

    switch (b->kind) {
        case BTN_TILE:
            draw_tile(dc, &rc, b, hot, pressed, disabled);
            if (focus) ui_round_frame(dc, &rc, ui_px(10), g_ui.accent, 2);
            ui_buffer_end(&buf);
            return 1;
        case BTN_PRIMARY:
            fill = disabled ? g_ui.surface3 : pressed ? g_ui.accent_pressed : hot ? g_ui.accent_hover : g_ui.accent;
            fg = disabled ? g_ui.text_faint : g_ui.accent_text;
            font = FONT_BODY_BOLD;
            break;
        case BTN_SECONDARY:
            fill = pressed ? g_ui.surface3 : hot ? ui_mix(g_ui.surface3, g_ui.surface2, 60) : g_ui.surface2;
            frame = g_ui.border_strong;
            fg = disabled ? g_ui.text_faint : g_ui.text;
            break;
        case BTN_DANGER:
            fill = disabled ? g_ui.surface2 : pressed ? ui_mix(g_ui.bad, back, 34) : hot ? ui_mix(g_ui.bad, back, 26) : g_ui.bad_soft;
            fg = disabled ? g_ui.text_faint : g_ui.bad;
            font = FONT_BODY_BOLD;
            break;
        case BTN_GHOST:
        case BTN_ICON:
            fill = pressed ? g_ui.surface3 : hot ? ui_mix(g_ui.surface3, back, 70) : back;
            fg = disabled ? g_ui.text_faint : hot ? g_ui.text : g_ui.text_dim;
            break;
        case BTN_LINK:
            fg = disabled ? g_ui.text_faint : hot ? g_ui.accent_hover : g_ui.accent;
            font = FONT_BODY_BOLD;
            break;
        case BTN_CHIP:
            radius = h / 2;
            font = FONT_SMALL_BOLD;
            if (b->checked) {
                fill = g_ui.accent_soft;
                frame = ui_mix(g_ui.accent, back, 55);
                fg = g_ui.accent;
            } else {
                fill = hot ? g_ui.surface2 : back;
                frame = g_ui.border_strong;
                fg = hot ? g_ui.text : g_ui.text_dim;
            }
            break;
    }

    if (fill != back || frame != CLR_NONE) {
        GpGraphics *g = gfx(dc);
        if (g) {
            if (fill != back) fill_path_rect(g, &rc, radius, fill);
            if (frame != CLR_NONE) frame_path_rect(g, &rc, radius, frame, 1);
            GdipDeleteGraphics(g);
        }
    }

    /* Glyph + label, centered as one group. */
    int glyph_w = b->glyph ? ui_px(16) : 0;
    int gap = (b->glyph && b->text[0]) ? ui_px(8) : 0;
    int text_w = ui_text_width(dc, b->text, font);
    int total = glyph_w + gap + text_w;
    int x = rc.left + (rc.right - rc.left - total) / 2;
    if (x < rc.left + ui_px(6)) x = rc.left + ui_px(6);
    if (b->glyph) {
        RECT gr = { x, rc.top, x + glyph_w, rc.bottom };
        ui_glyph(dc, b->glyph, &gr, b->kind == BTN_CHIP ? FONT_ICON_SMALL : FONT_ICON, fg);
    }
    if (b->text[0]) {
        RECT tr = { x + glyph_w + gap, rc.top, rc.right - ui_px(6), rc.bottom };
        ui_text(dc, b->text, &tr, font, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (focus) ui_round_frame(dc, &rc, radius, g_ui.accent, 2);
    ui_buffer_end(&buf);
    return 1;
}

/* =======================================================================
 * Search box
 * ===================================================================== */

typedef struct { RECT box; } SearchState;

static LRESULT CALLBACK search_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    switch (m) {
        case WM_GETDLGCODE: {
            /* Keep Escape for ourselves (clear the box) instead of letting IsDialogMessage turn
             * it into IDCANCEL on the main window. */
            LRESULT r = DefSubclassProc(h, m, w, l);
            MSG *msg = (MSG *)l;
            if (msg && msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE) r |= DLGC_WANTMESSAGE;
            return r;
        }
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                if (GetWindowTextLengthW(h) > 0) SetWindowTextW(h, L"");
                return 0;
            }
            break;
        case WM_CHAR:
            if (w == 27 || w == '\r') return 0;   /* no beep */
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, search_proc, id);
            free((void *)ref);
            break;
    }
    return DefSubclassProc(h, m, w, l);
}

HWND ui_search(HWND parent, int id, const char *cue) {
    HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    ui_use_font(e, FONT_BODY);
    WCHAR wcue[128];
    ui_widen(cue, wcue, 128);
    SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)wcue);
    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SetWindowSubclass(e, search_proc, 1, (DWORD_PTR)calloc(1, sizeof(SearchState)));
    track(e, FONT_BODY, TRACK_EDIT);
    return e;
}

void ui_search_place(HWND e, int x, int y, int w, int h) {
    DWORD_PTR ref = 0;
    if (!GetWindowSubclass(e, search_proc, 1, &ref)) return;
    SearchState *s = (SearchState *)ref;
    SetRect(&s->box, x, y, x + w, y + h);
    int th = ui_font_height(FONT_BODY) + 2;
    int left = x + ui_px(34);
    MoveWindow(e, left, y + (h - th) / 2, (x + w - ui_px(10)) - left, th, TRUE);
}

void ui_search_paint(HDC dc, HWND e) {
    DWORD_PTR ref = 0;
    if (!GetWindowSubclass(e, search_proc, 1, &ref)) return;
    SearchState *s = (SearchState *)ref;
    int focused = GetFocus() == e;
    ui_round_fill_frame(dc, &s->box, ui_px(8), g_ui.surface, focused ? g_ui.accent : g_ui.border_strong);
    RECT g = { s->box.left + ui_px(8), s->box.top, s->box.left + ui_px(30), s->box.bottom };
    ui_glyph(dc, GLYPH_SEARCH, &g, FONT_ICON_SMALL, focused ? g_ui.accent : g_ui.text_faint);
}

void ui_search_text(HWND e, char *out, size_t size) {
    WCHAR w[256];
    GetWindowTextW(e, w, 256);
    ui_narrow(w, out, (int)size);
}

/* =======================================================================
 * List views
 * ===================================================================== */

static LRESULT CALLBACK header_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

#define WM_UI_LIST_REPAINT (WM_APP + 40)

static ListSkin *list_skin(HWND list) {
    DWORD_PTR ref = 0;
    if (!list || !GetWindowSubclass(list, list_proc, 1, &ref)) return NULL;
    return (ListSkin *)ref;
}

static void list_apply_row_height(HWND list, ListSkin *s) {
    HIMAGELIST old = s->himl;
    s->himl = ImageList_Create(1, ui_px(s->row_height), ILC_COLOR32, 1, 0);
    ListView_SetImageList(list, s->himl, LVSIL_SMALL);
    if (old) ImageList_Destroy(old);
}

static void list_apply_theme(HWND list) {
    ListView_SetBkColor(list, g_ui.surface);
    ListView_SetTextBkColor(list, g_ui.surface);
    ListView_SetTextColor(list, g_ui.text);
    SetWindowTheme(list, g_ui.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    InvalidateRect(list, NULL, TRUE);
}

/* The list updates its scrollbars lazily, so predict whether a vertical one is about to appear
 * instead of trusting the current client width -- otherwise the column overhangs it and a
 * horizontal scrollbar shows up. Rows are painted to the full client width regardless. */
static void list_fit_column(HWND list, const ListSkin *s) {
    RECT rc;
    GetClientRect(list, &rc);
    int w = rc.right;
    int needs_vscroll = ListView_GetItemCount(list) * ui_px(s->row_height) > rc.bottom;
    if (needs_vscroll && !(GetWindowLongW(list, GWL_STYLE) & WS_VSCROLL)) {
        w -= GetSystemMetricsForDpi(SM_CXVSCROLL, g_dpi);
    }
    if (w > 0 && ListView_GetColumnWidth(list, 0) != w) ListView_SetColumnWidth(list, 0, w);
}

static void list_invalidate_row(HWND list, int row) {
    if (row < 0) return;
    RECT rc;
    if (ListView_GetItemRect(list, row, &rc, LVIR_BOUNDS)) {
        RECT client;
        GetClientRect(list, &client);
        rc.left = 0;
        if (rc.right < client.right) rc.right = client.right;
        InvalidateRect(list, &rc, FALSE);
    }
}

static LRESULT CALLBACK list_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    ListSkin *s = (ListSkin *)ref;
    switch (m) {
        case WM_MOUSEMOVE: {
            LVHITTESTINFO ht;
            memset(&ht, 0, sizeof(ht));
            ht.pt.x = GET_X_LPARAM(l);
            ht.pt.y = GET_Y_LPARAM(l);
            int row = ListView_SubItemHitTest(h, &ht);
            if (!(ht.flags & LVHT_ONITEM)) row = -1;
            if (row != s->hot) {
                list_invalidate_row(h, s->hot);
                s->hot = row;
                list_invalidate_row(h, row);
            }
            if (!s->tracking) {
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                s->tracking = 1;
            }
            break;
        }
        case WM_MOUSELEAVE:
            list_invalidate_row(h, s->hot);
            s->hot = -1;
            s->tracking = 0;
            break;
        case WM_SIZE: {
            LRESULT r = DefSubclassProc(h, m, w, l);
            if (s->fill_width) list_fit_column(h, s);
            /* The strip a vanished scrollbar leaves behind isn't repainted on its own. */
            InvalidateRect(h, NULL, TRUE);
            return r;
        }
        case LVM_SETITEMCOUNT: {
            /* A new count can show or hide the vertical scrollbar without a WM_SIZE, which would
             * leave the single column overhanging into a horizontal scrollbar. */
            LRESULT r = DefSubclassProc(h, m, w, l);
            if (s->fill_width) list_fit_column(h, s);
            /* The list hides a no-longer-needed scrollbar lazily and never repaints the strip it
             * occupied; repaint once that has happened. */
            PostMessageW(h, WM_UI_LIST_REPAINT, 0, 0);
            return r;
        }
        case WM_UI_LIST_REPAINT:
            if (s->fill_width) list_fit_column(h, s);
            RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
            return 0;
        case WM_GETDLGCODE: {
            LRESULT r = DefSubclassProc(h, m, w, l);
            MSG *msg = (MSG *)l;
            if (msg && msg->message == WM_KEYDOWN && msg->wParam == VK_RETURN) r |= DLGC_WANTMESSAGE;
            return r;
        }
        case WM_ERASEBKGND:
            if (ListView_GetItemCount(h) == 0) return 1;
            break;
        case WM_PAINT:
            if (ListView_GetItemCount(h) == 0) {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(h, &ps);
                RECT rc;
                GetClientRect(h, &rc);
                UiBuffer buf;
                HDC dc = ui_buffer_begin(&buf, hdc, &rc);
                ui_fill_rect(dc, &rc, g_ui.surface);
                if (s->empty[0]) {
                    RECT t = rc;
                    t.top += ui_px(36);
                    InflateRect(&t, -ui_px(24), 0);
                    ui_text(dc, s->empty, &t, FONT_BODY, g_ui.text_dim, DT_CENTER | DT_TOP | DT_WORDBREAK);
                }
                ui_buffer_end(&buf);
                EndPaint(h, &ps);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, list_proc, id);
            if (s->himl) ImageList_Destroy(s->himl);
            free(s);
            break;
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK header_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            UiBuffer buf;
            HDC dc = ui_buffer_begin(&buf, hdc, &rc);
            ui_fill_rect(dc, &rc, g_ui.surface);

            int n = (int)SendMessageW(h, HDM_GETITEMCOUNT, 0, 0);
            for (int i = 0; i < n; i++) {
                RECT ir;
                if (!SendMessageW(h, HDM_GETITEMRECT, i, (LPARAM)&ir)) continue;
                WCHAR text[64] = L"";
                HDITEMW it;
                memset(&it, 0, sizeof(it));
                it.mask = HDI_TEXT | HDI_FORMAT;
                it.pszText = text;
                it.cchTextMax = 64;
                SendMessageW(h, HDM_GETITEMW, i, (LPARAM)&it);

                char label[128];
                ui_narrow(text, label, sizeof(label));
                int right = (it.fmt & HDF_RIGHT) != 0;
                RECT tr = ir;
                tr.left += ui_px(12);
                tr.right -= ui_px(12);
                int sorted = (it.fmt & (HDF_SORTUP | HDF_SORTDOWN)) != 0;
                COLORREF color = sorted ? g_ui.text : g_ui.text_dim;
                int tw = ui_text_width(dc, label, FONT_SMALL_BOLD);
                ui_text(dc, label, &tr, FONT_SMALL_BOLD, color,
                        (right ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (sorted) {
                    RECT gr;
                    gr.top = tr.top;
                    gr.bottom = tr.bottom;
                    if (right) { gr.right = tr.right - tw - ui_px(4); gr.left = gr.right - ui_px(14); }
                    else       { gr.left = tr.left + tw + ui_px(4); gr.right = gr.left + ui_px(14); }
                    ui_glyph(dc, (it.fmt & HDF_SORTUP) ? GLYPH_CHEVRON_UP : GLYPH_CHEVRON_DOWN,
                             &gr, FONT_ICON_SMALL, g_ui.accent);
                }
            }
            ui_hline(dc, rc.left, rc.right, rc.bottom - 1, g_ui.border);
            ui_buffer_end(&buf);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, header_proc, id);
            break;
    }
    return DefSubclassProc(h, m, w, l);
}

HWND ui_list(HWND parent, int id, DWORD extra_style, int row_height, int fill_width) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS |
                  LVS_OWNERDATA | extra_style;
    if (fill_width) style |= LVS_NOCOLUMNHEADER;
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"", style, 0, 0, 0, 0, parent,
                              (HMENU)(INT_PTR)id, NULL, NULL);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    ListSkin *s = (ListSkin *)calloc(1, sizeof(ListSkin));
    s->hot = -1;
    s->row_height = row_height;
    s->fill_width = fill_width;
    SetWindowSubclass(lv, list_proc, 1, (DWORD_PTR)s);

    if (fill_width) {
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_WIDTH;
        col.cx = 100;
        SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    } else {
        s->header = ListView_GetHeader(lv);
        if (s->header) SetWindowSubclass(s->header, header_proc, 1, 0);
    }

    ui_use_font(lv, FONT_BODY);
    track(lv, FONT_BODY, TRACK_LIST);
    list_apply_theme(lv);
    list_apply_row_height(lv, s);
    return lv;
}

void ui_list_set_empty(HWND list, const char *text) {
    ListSkin *s = list_skin(list);
    if (!s || strcmp(s->empty, text) == 0) return;
    strncpy(s->empty, text, sizeof(s->empty) - 1);
    if (ListView_GetItemCount(list) == 0) InvalidateRect(list, NULL, FALSE);
}

int ui_list_hot(HWND list) {
    ListSkin *s = list_skin(list);
    return s ? s->hot : -1;
}

void ui_list_set_sort(HWND list, int column, int dir) {
    HWND header = ListView_GetHeader(list);
    if (!header) return;
    int n = (int)SendMessageW(header, HDM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        HDITEMW it;
        memset(&it, 0, sizeof(it));
        it.mask = HDI_FORMAT;
        SendMessageW(header, HDM_GETITEMW, i, (LPARAM)&it);
        it.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == column && dir > 0) it.fmt |= HDF_SORTUP;
        if (i == column && dir < 0) it.fmt |= HDF_SORTDOWN;
        SendMessageW(header, HDM_SETITEMW, i, (LPARAM)&it);
    }
    InvalidateRect(header, NULL, FALSE);
}

void ui_list_row_rect(HWND list, int row, RECT *rc) {
    SetRectEmpty(rc);
    if (!ListView_GetItemRect(list, row, rc, LVIR_BOUNDS)) return;
    RECT client;
    GetClientRect(list, &client);
    if (rc->left > 0) rc->left = 0;
    if (rc->right < client.right) rc->right = client.right;
}

void ui_list_row_bg(HDC dc, HWND list, const RECT *row, int index) {
    ui_fill_rect(dc, row, g_ui.surface);
    int selected = (ListView_GetItemState(list, index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    int hot = ui_list_hot(list) == index;
    if (!selected && !hot) return;

    RECT r = *row;
    RECT client;
    GetClientRect(list, &client);
    r.left = client.left + ui_px(6);
    r.right = client.right - ui_px(6);
    r.top += ui_px(2);
    r.bottom -= ui_px(2);
    if (selected) {
        int focused = GetFocus() == list;
        ui_round_fill(dc, &r, ui_px(6), focused ? g_ui.accent_soft : g_ui.surface3);
        RECT bar = { r.left, r.top + ui_px(8), r.left + ui_px(3), r.bottom - ui_px(8) };
        if (bar.bottom - bar.top < ui_px(8)) { bar.top = r.top + ui_px(4); bar.bottom = r.bottom - ui_px(4); }
        ui_round_fill(dc, &bar, ui_px(2), focused ? g_ui.accent : g_ui.text_faint);
    } else {
        ui_round_fill(dc, &r, ui_px(6), g_ui.surface2);
    }
}

/* The dark list theme draws column-divider lines through the empty space below the last row.
 * Called at CDDS_POSTPAINT -- still inside the list's own buffered paint -- it paints that
 * space over, so the lines never reach the screen. */
void ui_list_paint_tail(HWND list, HDC dc) {
    int count = ListView_GetItemCount(list);
    if (count <= 0) return;
    RECT client, last;
    GetClientRect(list, &client);
    int top = client.top;
    HWND header = ListView_GetHeader(list);
    if (header && IsWindowVisible(header)) {
        RECT hr;
        GetWindowRect(header, &hr);
        top += hr.bottom - hr.top;
    }
    if (!ListView_GetItemRect(list, count - 1, &last, LVIR_BOUNDS)) return;
    if (last.bottom < top) last.bottom = top;
    if (last.bottom >= client.bottom) return;
    RECT tail = { client.left, last.bottom, client.right, client.bottom };
    ui_fill_rect(dc, &tail, g_ui.surface);
}

int ui_list_selected(HWND list) {
    return ListView_GetNextItem(list, -1, LVNI_SELECTED);
}

void ui_list_select(HWND list, int row) {
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
    if (row < 0) return;
    ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetSelectionMark(list, row);
    /* Scrolling a list that hasn't been sized yet leaves its scroll position invalid. */
    RECT rc;
    GetClientRect(list, &rc);
    if (rc.bottom > 0) ListView_EnsureVisible(list, row, FALSE);
}

void ui_retheme_all(void) {
    for (int i = 0; i < g_tracked_count; i++) {
        Tracked *t = &g_tracked[i];
        if (!IsWindow(t->hwnd)) continue;
        if (t->kind == TRACK_LIST) list_apply_theme(t->hwnd);
        if (t->kind == TRACK_EDIT) SetWindowTheme(t->hwnd, g_ui.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
        InvalidateRect(t->hwnd, NULL, TRUE);
    }
}

/* =======================================================================
 * Modal dialog
 * ===================================================================== */

#define DLG_CLASS L"GameOptimizerDialog"
#define ID_DLG_INPUT   50
#define ID_DLG_DETAILS 51

typedef struct {
    const UiDialog *d;
    HWND input, details, ok, cancel;
    RECT icon, heading, body, input_box, details_box, footer;
    int result;
    int done;
} DlgState;

static void dlg_paint(HWND h, DlgState *st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    UiBuffer buf;
    HDC dc = ui_buffer_begin(&buf, hdc, &rc);
    ui_fill_rect(dc, &rc, g_ui.surface);
    ui_fill_rect(dc, &st->footer, g_ui.bg);
    ui_hline(dc, st->footer.left, st->footer.right, st->footer.top, g_ui.border);

    const UiDialog *d = st->d;
    if (d->glyph) {
        ui_ellipse(dc, &st->icon, ui_tone_soft(d->tone));
        ui_glyph(dc, d->glyph, &st->icon, FONT_ICON_LARGE, ui_tone_color(d->tone));
    }
    ui_text(dc, d->heading, &st->heading, FONT_HEADING, g_ui.text, DT_LEFT | DT_WORDBREAK);
    ui_text(dc, d->body, &st->body, FONT_BODY, g_ui.text_dim, DT_LEFT | DT_WORDBREAK);
    if (st->input) {
        int focused = GetFocus() == st->input;
        ui_round_fill_frame(dc, &st->input_box, ui_px(8), g_ui.surface2, focused ? g_ui.accent : g_ui.border_strong);
    }
    if (st->details) ui_round_fill_frame(dc, &st->details_box, ui_px(8), g_ui.surface2, g_ui.border);
    ui_buffer_end(&buf);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    DlgState *st = (DlgState *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)l)->lpCreateParams);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (st) { dlg_paint(h, st); return 0; }
            break;
        case WM_DRAWITEM:
            if (ui_draw_item((const DRAWITEMSTRUCT *)l)) return TRUE;
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)w, g_ui.text);
            SetBkColor((HDC)w, g_ui.surface2);
            return (LRESULT)ui_brush(g_ui.surface2);
        case WM_COMMAND:
            if (!st) break;
            if (LOWORD(w) == IDOK) {
                if (st->input && st->d->input && st->d->input_size > 0) {
                    WCHAR text[256];
                    GetWindowTextW(st->input, text, 256);
                    ui_narrow(text, st->d->input, (int)st->d->input_size);
                }
                st->result = 1;
                st->done = 1;
                return 0;
            }
            if (LOWORD(w) == IDCANCEL) {
                st->result = st->d->cancel ? 0 : 1;
                st->done = 1;
                return 0;
            }
            if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) InvalidateRect(h, NULL, FALSE);
            break;
        case WM_CLOSE:
            if (st) {
                st->result = st->d->cancel ? 0 : 1;
                st->done = 1;
            }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int ui_dialog(HWND owner, const UiDialog *d) {
    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = dlg_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = DLG_CLASS;
        RegisterClassExW(&wc);
        registered = 1;
    }

    DlgState st;
    memset(&st, 0, sizeof(st));
    st.d = d;

    /* Measure everything first so the window can be created at its final size. */
    int width = ui_px(480);
    int pad = ui_px(24);
    HDC mdc = GetDC(NULL);
    int text_x = pad;
    int y = pad;
    if (d->glyph) {
        SetRect(&st.icon, pad, pad, pad + ui_px(40), pad + ui_px(40));
        text_x = st.icon.right + ui_px(16);
    }
    int text_w = width - text_x - pad;
    int hh = ui_text_height(mdc, d->heading, FONT_HEADING, text_w);
    int heading_top = y;
    if (d->glyph && hh < ui_px(40)) heading_top = y + (ui_px(40) - hh) / 2;
    SetRect(&st.heading, text_x, heading_top, text_x + text_w, heading_top + hh);
    y = st.heading.bottom;
    if (d->glyph && st.icon.bottom > y) y = st.icon.bottom;

    if (d->body && *d->body) {
        y += ui_px(8);
        int bh = ui_text_height(mdc, d->body, FONT_BODY, text_w);
        SetRect(&st.body, text_x, y, text_x + text_w, y + bh);
        y = st.body.bottom;
    }
    if (d->input) {
        y += ui_px(16);
        SetRect(&st.input_box, text_x, y, text_x + text_w, y + ui_px(38));
        y = st.input_box.bottom;
    }
    if (d->details && *d->details) {
        int lines = 1;
        for (const char *p = d->details; *p; p++) if (*p == '\n') lines++;
        int line_h = ui_font_height(FONT_BODY);
        int dh = lines * line_h + ui_px(16);
        if (dh > ui_px(210)) dh = ui_px(210);
        y += ui_px(16);
        SetRect(&st.details_box, text_x, y, text_x + text_w, y + dh);
        y = st.details_box.bottom;
    }
    ReleaseDC(NULL, mdc);

    int btn_h = ui_px(34);
    int footer_h = btn_h + ui_px(20) * 2;
    y += pad;
    SetRect(&st.footer, 0, y, width, y + footer_h);
    int height = y + footer_h;

    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    RECT wr = { 0, 0, width, height };
    AdjustWindowRectExForDpi(&wr, style, FALSE, ex, ui_dpi());
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;

    RECT orc;
    if (!owner || !GetWindowRect(owner, &orc)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &orc, 0);
    int x = orc.left + ((orc.right - orc.left) - ww) / 2;
    int top = orc.top + ((orc.bottom - orc.top) - wh) / 2;

    WCHAR title[128];
    ui_widen(d->title ? d->title : APP_NAME, title, 128);
    HWND dlg = CreateWindowExW(ex, DLG_CLASS, title, style, x, top, ww, wh, owner, NULL, inst, &st);
    if (!dlg) return 0;
    ui_style_toplevel(dlg);

    if (d->input) {
        st.input = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   0, 0, 0, 0, dlg, (HMENU)ID_DLG_INPUT, inst, NULL);
        ui_use_font(st.input, FONT_BODY);
        SendMessageW(st.input, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        if ((int)d->input_size > 1) SendMessageW(st.input, EM_LIMITTEXT, d->input_size - 1, 0);
        if (d->input_cue) {
            WCHAR cue[128];
            ui_widen(d->input_cue, cue, 128);
            SendMessageW(st.input, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
        }
        if (d->input[0]) {
            WCHAR init[256];
            ui_widen(d->input, init, 256);
            SetWindowTextW(st.input, init);
        }
        int th = ui_font_height(FONT_BODY) + 2;
        int bh = st.input_box.bottom - st.input_box.top;
        MoveWindow(st.input, st.input_box.left + ui_px(12), st.input_box.top + (bh - th) / 2,
                   st.input_box.right - st.input_box.left - ui_px(24), th, TRUE);
    }
    if (d->details && *d->details) {
        WCHAR *wd = (WCHAR *)malloc((strlen(d->details) + 1) * sizeof(WCHAR));
        if (wd) ui_widen(d->details, wd, (int)strlen(d->details) + 1);
        st.details = CreateWindowExW(0, L"EDIT", wd ? wd : L"",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                     0, 0, 0, 0, dlg, (HMENU)ID_DLG_DETAILS, inst, NULL);
        free(wd);
        ui_use_font(st.details, FONT_BODY);
        SetWindowTheme(st.details, g_ui.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
        MoveWindow(st.details, st.details_box.left + ui_px(10), st.details_box.top + ui_px(6),
                   st.details_box.right - st.details_box.left - ui_px(14),
                   st.details_box.bottom - st.details_box.top - ui_px(12), TRUE);
    }

    st.ok = ui_button(dlg, IDOK, d->danger ? BTN_DANGER : BTN_PRIMARY, 0, d->ok ? d->ok : "OK");
    int ok_w = ui_button_width(st.ok);
    if (ok_w < ui_px(96)) ok_w = ui_px(96);
    int by = st.footer.top + ui_px(20);
    int bx = width - pad - ok_w;
    MoveWindow(st.ok, bx, by, ok_w, btn_h, TRUE);
    if (d->cancel) {
        st.cancel = ui_button(dlg, IDCANCEL, BTN_SECONDARY, 0, d->cancel);
        int cw = ui_button_width(st.cancel);
        if (cw < ui_px(96)) cw = ui_px(96);
        MoveWindow(st.cancel, bx - ui_px(8) - cw, by, cw, btn_h, TRUE);
    }

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.input ? st.input : (d->danger && st.cancel) ? st.cancel : st.ok);

    MSG msg;
    int quit = 0;
    while (!st.done) {
        int r = GetMessageW(&msg, NULL, 0, 0);
        if (r <= 0) { quit = 1; break; }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    if (owner) SetActiveWindow(owner);
    if (quit) PostQuitMessage((int)msg.wParam);
    return st.result;
}

void ui_message(HWND owner, UiTone tone, const char *heading, const char *body) {
    UiDialog d;
    memset(&d, 0, sizeof(d));
    d.heading = heading;
    d.body = body;
    d.tone = tone;
    d.glyph = tone == TONE_BAD ? GLYPH_ERROR : tone == TONE_WARN ? GLYPH_WARNING
            : tone == TONE_GOOD ? GLYPH_CHECK : GLYPH_INFO;
    ui_dialog(owner, &d);
}

int ui_copy_text(HWND owner, const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return -1;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(WCHAR));
    if (!mem) return -1;
    WCHAR *p = (WCHAR *)GlobalLock(mem);
    ui_widen(s, p, n);
    GlobalUnlock(mem);
    if (!OpenClipboard(owner)) {
        GlobalFree(mem);
        return -1;
    }
    EmptyClipboard();
    int ok = SetClipboardData(CF_UNICODETEXT, mem) != NULL;
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok ? 0 : -1;
}
