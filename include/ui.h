#ifndef UI_H
#define UI_H

/* Small UI kit on top of plain Win32: a light/dark palette, DPI-scaled fonts, anti-aliased
 * shape drawing (GDI+ flat API), owner-drawn buttons, skinned list views and themed modal
 * dialogs. Every string passed in here is UTF-8. */

#include "common.h"
#include <commctrl.h>

#define HEXRGB(v) RGB(((v) >> 16) & 0xFF, ((v) >> 8) & 0xFF, (v) & 0xFF)

typedef enum { TONE_NEUTRAL, TONE_ACCENT, TONE_GOOD, TONE_WARN, TONE_BAD } UiTone;

typedef struct {
    int dark;
    COLORREF bg, sidebar, surface, surface2, surface3, border, border_strong;
    COLORREF text, text_dim, text_faint;
    COLORREF accent, accent_hover, accent_pressed, accent_text, accent_soft;
    COLORREF good, good_soft, warn, warn_soft, bad, bad_soft;
} UiPalette;

extern UiPalette g_ui;

typedef enum { THEME_SYSTEM = 0, THEME_DARK = 1, THEME_LIGHT = 2 } UiThemeMode;

void ui_init(void);
void ui_shutdown(void);
void        ui_set_theme_mode(UiThemeMode mode);
UiThemeMode ui_theme_mode(void);
/* Re-reads the Windows light/dark setting (for THEME_SYSTEM). Returns 1 if the palette changed. */
int  ui_reload_theme(void);
/* Dark/light title bar and caption colors for a top-level window. */
void ui_style_toplevel(HWND hwnd);
/* Re-applies colors/window themes to every list and edit the kit created (after a theme change). */
void ui_retheme_all(void);

COLORREF ui_mix(COLORREF a, COLORREF b, int percent_a);
COLORREF ui_tone_color(UiTone tone);
COLORREF ui_tone_soft(UiTone tone);
HBRUSH   ui_brush(COLORREF c);   /* cached; never delete the result */

/* ---- DPI ---- */
void ui_set_dpi(UINT dpi);       /* recreates fonts and re-sends them to tracked controls */
UINT ui_dpi(void);
int  ui_px(int logical);

typedef enum {
    FONT_BODY, FONT_BODY_BOLD, FONT_SMALL, FONT_SMALL_BOLD, FONT_HEADING, FONT_TITLE, FONT_DISPLAY,
    FONT_ICON, FONT_ICON_SMALL, FONT_ICON_LARGE, FONT_COUNT
} UiFont;

HFONT ui_font(UiFont f);
int   ui_font_height(UiFont f);
/* WM_SETFONT now, and again automatically whenever the DPI changes. */
void  ui_use_font(HWND control, UiFont f);

/* ---- text ---- */
int  ui_widen(const char *utf8, WCHAR *out, int capacity);
void ui_narrow(const WCHAR *w, char *out, int capacity);
void ui_text(HDC dc, const char *s, const RECT *rc, UiFont f, COLORREF c, UINT format);
int  ui_text_width(HDC dc, const char *s, UiFont f);
int  ui_text_height(HDC dc, const char *s, UiFont f, int width);
void ui_glyph(HDC dc, WCHAR glyph, const RECT *rc, UiFont f, COLORREF c);   /* centered */
int  ui_stristr(const char *haystack, const char *needle);                 /* 1 if found */

/* ---- shapes ---- */
void ui_fill_rect(HDC dc, const RECT *rc, COLORREF c);
void ui_hline(HDC dc, int x0, int x1, int y, COLORREF c);
void ui_round_fill(HDC dc, const RECT *rc, int radius, COLORREF c);
void ui_round_frame(HDC dc, const RECT *rc, int radius, COLORREF c, int width);
void ui_round_fill_frame(HDC dc, const RECT *rc, int radius, COLORREF fill, COLORREF frame);
void ui_ellipse(HDC dc, const RECT *rc, COLORREF c);
void ui_ring(HDC dc, const RECT *rc, int thickness, double fraction, COLORREF track, COLORREF fill);
void ui_sparkline(HDC dc, const RECT *rc, const float *values, int count, float max, COLORREF c);
void ui_progress(HDC dc, const RECT *rc, double fraction, COLORREF track, COLORREF fill);
void ui_card(HDC dc, const RECT *rc);
/* Rounded status label. Anchored at x (left edge, or right edge with anchor_right) and
 * vertically centered on cy. Returns its width. */
int  ui_pill(HDC dc, int x, int cy, const char *text, UiTone tone, int anchor_right);
int  ui_pill_width(HDC dc, const char *text);
/* Toggle switch whose top-left is (x, y); UI_SWITCH_W x UI_SWITCH_H logical px. */
#define UI_SWITCH_W 40
#define UI_SWITCH_H 20
void ui_switch(HDC dc, int x, int y, int on, int enabled, int hot);

/* ---- flicker-free painting ---- */
typedef struct { HDC dc, target; HBITMAP bmp, old; RECT rc; } UiBuffer;
HDC  ui_buffer_begin(UiBuffer *b, HDC target, const RECT *rc);
void ui_buffer_end(UiBuffer *b);

/* ---- owner-drawn buttons ---- */
typedef enum {
    BTN_PRIMARY, BTN_SECONDARY, BTN_DANGER, BTN_GHOST, BTN_CHIP, BTN_TILE, BTN_LINK, BTN_ICON
} UiButtonKind;

/* What sits behind a button's rounded corners. */
typedef enum { BACK_BG, BACK_SURFACE, BACK_SIDEBAR, BACK_TONE_SOFT } UiBackdrop;

HWND ui_button(HWND parent, int id, UiButtonKind kind, WCHAR glyph, const char *text);
void ui_button_set_text(HWND b, const char *text);
void ui_button_set_sub(HWND b, const char *sub);          /* tiles: second line */
void ui_button_set_glyph(HWND b, WCHAR glyph);
void ui_button_set_tone(HWND b, UiTone tone);              /* tiles and BACK_TONE_SOFT */
void ui_button_set_kind(HWND b, UiButtonKind kind);
void ui_button_set_backdrop(HWND b, UiBackdrop backdrop);
void ui_button_set_checked(HWND b, int checked);           /* chips */
int  ui_button_checked(HWND b);
int  ui_button_width(HWND b);                              /* natural width for its label */
int  ui_draw_item(const DRAWITEMSTRUCT *dis);              /* call from WM_DRAWITEM */

/* ---- search box (an EDIT plus a painted frame) ---- */
HWND ui_search(HWND parent, int id, const char *cue);
void ui_search_place(HWND edit, int x, int y, int w, int h);
void ui_search_paint(HDC dc, HWND edit);                   /* from the parent's paint */
void ui_search_text(HWND edit, char *out, size_t size);

/* ---- list views ---- */
/* Owner-data report list that the caller paints row-by-row via NM_CUSTOMDRAW. With
 * `fill_width` it has no header and a single column that always spans the client width. */
HWND ui_list(HWND parent, int id, DWORD extra_style, int row_height, int fill_width);
void ui_list_set_empty(HWND list, const char *text);
int  ui_list_hot(HWND list);
void ui_list_set_sort(HWND list, int column, int dir);     /* dir: 1 up, -1 down, 0 none */
void ui_list_row_bg(HDC dc, HWND list, const RECT *row, int index);
int  ui_list_selected(HWND list);
void ui_list_select(HWND list, int row);
void ui_list_row_rect(HWND list, int row, RECT *rc);       /* clipped to the client width */
/* Call at CDDS_POSTPAINT (request it with CDRF_NOTIFYPOSTPAINT): keeps the empty space below
 * the last row free of the dark theme's column-divider lines. */
void ui_list_paint_tail(HWND list, HDC dc);

/* ---- dialogs ---- */
typedef struct {
    const char *title;       /* caption; defaults to APP_NAME */
    const char *heading;
    const char *body;
    const char *details;     /* optional scrolling box, "\r\n"-separated lines */
    const char *ok;          /* defaults to "OK" */
    const char *cancel;      /* NULL = no cancel button */
    UiTone tone;
    WCHAR glyph;             /* 0 = no icon */
    int danger;              /* style the OK button as destructive */
    char *input;             /* optional single-line input (in: initial text, out: result) */
    size_t input_size;
    const char *input_cue;
} UiDialog;

int  ui_dialog(HWND owner, const UiDialog *d);   /* 1 = OK */
void ui_message(HWND owner, UiTone tone, const char *heading, const char *body);

int  ui_copy_text(HWND owner, const char *utf8);

/* ---- icon glyphs (Segoe Fluent Icons / Segoe MDL2 Assets) ---- */
#define GLYPH_HOME          0xE80F
#define GLYPH_SPEED         0xEC4A
#define GLYPH_SHIELD        0xEA18
#define GLYPH_SERVICES      0xE90F
#define GLYPH_PROCESSES     0xE9D9
#define GLYPH_PROFILES      0xE8F1
#define GLYPH_SEARCH        0xE721
#define GLYPH_REFRESH       0xE72C
#define GLYPH_CLOSE         0xE711
#define GLYPH_CHECK         0xE73E
#define GLYPH_WARNING       0xE7BA
#define GLYPH_INFO          0xE946
#define GLYPH_ERROR         0xEA39
#define GLYPH_POWER         0xE7E8
#define GLYPH_UNDO          0xE7A7
#define GLYPH_DELETE        0xE74D
#define GLYPH_ADD           0xE710
#define GLYPH_COPY          0xE8C8
#define GLYPH_FOLDER        0xE838
#define GLYPH_LOCK          0xE72E
#define GLYPH_SUN           0xE706
#define GLYPH_MOON          0xE708
#define GLYPH_CHEVRON_DOWN  0xE70D
#define GLYPH_CHEVRON_UP    0xE70E
#define GLYPH_CHEVRON_RIGHT 0xE76C
#define GLYPH_BOLT          0xE945
#define GLYPH_GAME          0xE7FC
#define GLYPH_DRIVE         0xEDA2
#define GLYPH_GLOBE         0xE774
#define GLYPH_ERASE         0xE75C
#define GLYPH_PLAY          0xE768
#define GLYPH_PAUSE         0xE769
#define GLYPH_ADMIN         0xE7EF
#define GLYPH_SAVE          0xE74E
#define GLYPH_STOP          0xE71A

#endif
