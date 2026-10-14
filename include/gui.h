#ifndef GUI_H
#define GUI_H

#include "common.h"
#include "tweaks.h"
#include "sysinfo.h"
#include "ui.h"

/* Page indices, in sidebar order. */
enum {
    PAGE_DASHBOARD = 0,
    PAGE_PERFORMANCE,
    PAGE_TELEMETRY,
    PAGE_SERVICES,
    PAGE_PROCESSES,
    PAGE_PROFILES,
    PAGE_COUNT
};

/* Every page lives in its own panel window (a child of the main window) that the shell shows
 * and hides as a unit. The panel forwards its messages here:
 *   create        build child controls parented to `panel` (visible; the panel starts hidden)
 *   layout        the panel was resized; position controls
 *   paint         paint everything that isn't a control, into a buffered DC already filled
 *                 with the page background
 *   show / hide   the page became visible / was switched away from
 *   command, notify, timer, context_menu   the corresponding window messages
 *   refresh       live state changed (a scan or job finished) -- re-read and repaint
 *   reload        F5
 *   focus_search  Ctrl+F
 *   badge         short text for the sidebar (e.g. "29/33"); return 0 for none
 * Any of them may be NULL. */
typedef struct GuiPage {
    const char *title;
    WCHAR glyph;
    void (*create)(HWND panel);
    void (*layout)(int width, int height);
    void (*paint)(HDC dc, const RECT *client);
    void (*show)(void);
    void (*hide)(void);
    int  (*command)(WPARAM wparam, LPARAM lparam);
    int  (*notify)(LPNMHDR hdr, LRESULT *result);
    void (*timer)(UINT_PTR id);
    void (*context_menu)(HWND from, int x, int y);
    void (*refresh)(void);
    void (*reload)(void);
    void (*focus_search)(void);
    int  (*badge)(char *buf, size_t size);
    /* Optional: total height the page wants at this width. When it exceeds the panel, the
     * panel scrolls and keeps `scroll_y` current; the page offsets its layout by it. */
    int  (*content_height)(int width);
    int  scroll_y;
} GuiPage;

extern GuiPage g_page_dashboard;
extern GuiPage g_page_performance;
extern GuiPage g_page_telemetry;
extern GuiPage g_page_services;
extern GuiPage g_page_processes;
extern GuiPage g_page_profiles;

/* ---- shell (gui_main.c) ---- */
HWND gui_main_window(void);
HWND gui_page_panel(int page);
void gui_select_page(int page);
int  gui_current_page(void);

void gui_status(const char *text);
/* Transient message in the status bar, optionally with one action link (e.g. "Undo"). */
void gui_toast(UiTone tone, const char *text, const char *action, void (*on_action)(void));

/* 1 once the startup scan has read the real state of every tweak. */
int  gui_ready(void);
const SysInfo *gui_sysinfo(void);
int  gui_sysinfo_ready(void);

/* Re-reads nothing itself: tells the sidebar, banners and visible page that state changed. */
void gui_state_changed(void);

/* Restart-required bookkeeping: tweaks whose state differs from what it was at startup. */
int  gui_pending_restart_count(void);
int  gui_restart_pending_for(int tweak_index);

/* Offers to relaunch elevated (explaining `what` needs it). Returns 1 if a relaunch started. */
int  gui_offer_elevation(const char *what);
void gui_relaunch_elevated(void);

/* ---- background jobs (gui_jobs.c) ----
 * Slow work (PowerShell, bcdedit, service stops) runs on a worker thread so the window stays
 * responsive. One job at a time; progress shows in the status bar. */
#define WM_APP_JOB_PROGRESS (WM_APP + 1)
#define WM_APP_JOB_DONE     (WM_APP + 2)

typedef struct GuiJob GuiJob;
typedef void (*GuiJobFn)(GuiJob *job);
struct GuiJob {
    GuiJobFn work;      /* worker thread */
    GuiJobFn done;      /* UI thread, after work returns */
    void *ctx;          /* owned by the job's callbacks */
    char label[96];
};

int  gui_job_start(const char *label, GuiJobFn work, GuiJobFn done, void *ctx);
void gui_job_step(GuiJob *job, int current, int total, const char *text);   /* worker thread */
int  gui_job_busy(void);
void gui_job_message(UINT msg, WPARAM wparam, LPARAM lparam);             /* main window */

/* Turns the given tweaks on (want=1), off (want=0) or flips each (want=-1) in the background,
 * then reports the outcome with an Undo action. Tweaks that need Administrator are skipped
 * when not elevated. */
void gui_tweaks_set(const int *indices, int count, int want, const char *label);
/* The tweak a running job is changing right now, or -1. */
int  gui_tweak_in_progress(void);
/* Re-reads every tweak (and system info if `with_sysinfo`) in the background. */
void gui_rescan(int with_sysinfo);

/* Shell hooks the job runner drives (gui_main.c). */
void gui_progress(int current, int total, const char *text);
void gui_progress_end(void);
void gui_job_finished(void);

/* ---- cross-page actions ---- */
void gui_processes_focus_toggle(void);
int  gui_processes_pinned(void);            /* last known pinned count; -1 before any check */

#endif
