#ifndef GUI_H
#define GUI_H

#include "common.h"
#include "tweaks.h"

/* Page indices, matching the order tabs are inserted in gui_main.c. */
enum {
    PAGE_DASHBOARD = 0,
    PAGE_PERFORMANCE,
    PAGE_TELEMETRY,
    PAGE_SERVICES,
    PAGE_PROCESSES,
    PAGE_PROFILES,
    PAGE_COUNT
};

/* Every page implements this interface. `create` builds the page's child controls parented to
 * the main window, initially hidden. `show` is called when the page becomes visible (a good
 * point to refresh live data), `hide` when it is switched away from. `command` and `notify`
 * forward WM_COMMAND / WM_NOTIFY for the page's controls; both return 1 if handled. */
typedef struct {
    void (*create)(HWND parent);
    void (*layout)(int x, int y, int width, int height);
    void (*show)(void);
    void (*hide)(void);
    int  (*command)(WPARAM wparam, LPARAM lparam);
    int  (*notify)(LPNMHDR hdr);
} GuiPage;

extern GuiPage g_page_dashboard;
extern GuiPage g_page_performance;
extern GuiPage g_page_telemetry;
extern GuiPage g_page_services;
extern GuiPage g_page_processes;
extern GuiPage g_page_profiles;

/* Shared UI helpers implemented in gui_main.c. */
HFONT gui_font(void);          /* standard UI font (Segoe UI 9pt) */
HFONT gui_font_bold(void);
HFONT gui_font_title(void);
void  gui_apply_font(HWND control);
void  gui_status(const char *text);   /* writes to the bottom status bar */
HWND  gui_main_window(void);

/* Re-reads live tweak state and repaints whichever pages display it. Safe to call from any
 * page after an apply/revert. */
void gui_refresh_tweak_state(void);

/* Called from the main window's WM_TIMER so the process list auto-refreshes while visible. */
void gui_processes_tick(void);

/* Applies (or reverts) every tweak in `category` that isn't already in the requested state,
 * showing progress in the status bar. Returns the number that failed. */
int gui_bulk_toggle(TweakCategory category, int want_enabled);

#endif
