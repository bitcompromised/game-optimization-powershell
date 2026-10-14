#include "gui.h"
#include <process.h>

/* =======================================================================
 * Job runner
 *
 * One worker thread at a time. The worker reports progress by posting heap-allocated step
 * messages; the UI thread frees them. `ctx` must be malloc'd (or NULL): the runner frees it
 * after done() has run, or immediately if the job can't start.
 * ===================================================================== */

static GuiJob *g_job;
static HANDLE g_thread;

typedef struct {
    int current, total;
    char text[192];
} StepMsg;

static unsigned __stdcall job_thread(void *arg) {
    GuiJob *job = (GuiJob *)arg;
    job->work(job);
    PostMessageW(gui_main_window(), WM_APP_JOB_DONE, 0, (LPARAM)job);
    return 0;
}

int gui_job_start(const char *label, GuiJobFn work, GuiJobFn done, void *ctx) {
    if (g_job) {
        gui_toast(TONE_WARN, "Still working on the previous change \xE2\x80\x94 try again in a moment.", NULL, NULL);
        free(ctx);
        return -1;
    }

    GuiJob *job = (GuiJob *)calloc(1, sizeof(GuiJob));
    if (!job) {
        free(ctx);
        return -1;
    }
    job->work = work;
    job->done = done;
    job->ctx = ctx;
    strncpy(job->label, label, sizeof(job->label) - 1);

    g_job = job;
    gui_progress(0, 0, label);

    uintptr_t t = _beginthreadex(NULL, 0, job_thread, job, 0, NULL);
    if (!t) {
        g_job = NULL;
        gui_progress_end();
        free(job->ctx);
        free(job);
        gui_toast(TONE_BAD, "Couldn't start a background task.", NULL, NULL);
        return -1;
    }
    g_thread = (HANDLE)t;
    return 0;
}

void gui_job_step(GuiJob *job, int current, int total, const char *text) {
    (void)job;
    StepMsg *s = (StepMsg *)malloc(sizeof(StepMsg));
    if (!s) return;
    s->current = current;
    s->total = total;
    strncpy(s->text, text, sizeof(s->text) - 1);
    s->text[sizeof(s->text) - 1] = '\0';
    if (!PostMessageW(gui_main_window(), WM_APP_JOB_PROGRESS, 0, (LPARAM)s)) free(s);
}

int gui_job_busy(void) { return g_job != NULL; }

void gui_job_message(UINT msg, WPARAM wparam, LPARAM lparam) {
    (void)wparam;
    if (msg == WM_APP_JOB_PROGRESS) {
        StepMsg *s = (StepMsg *)lparam;
        if (g_job) gui_progress(s->current, s->total, s->text);
        free(s);
        return;
    }
    if (msg == WM_APP_JOB_DONE) {
        GuiJob *job = (GuiJob *)lparam;
        if (g_thread) {
            WaitForSingleObject(g_thread, INFINITE);
            CloseHandle(g_thread);
            g_thread = NULL;
        }
        g_job = NULL;
        gui_progress_end();
        if (job->done) job->done(job);
        free(job->ctx);
        free(job);
        gui_job_finished();
    }
}

/* =======================================================================
 * Tweak changes
 * ===================================================================== */

#define MAX_TWEAKS 128

typedef struct {
    int indices[MAX_TWEAKS];
    int targets[MAX_TWEAKS];     /* desired state per entry */
    int count;
    int before[MAX_TWEAKS];      /* every tweak's state before the job, to find what changed */
    int attempted;
    int failed;
    int skipped_admin;
    int is_undo;
} TweakJob;

static volatile LONG g_in_progress = -1;

/* What the most recent job changed, and the state to put each back to. */
static int g_undo_index[MAX_TWEAKS];
static int g_undo_target[MAX_TWEAKS];
static int g_undo_count;

int gui_tweak_in_progress(void) { return (int)g_in_progress; }

static void tweak_work(GuiJob *job) {
    TweakJob *t = (TweakJob *)job->ctx;
    for (int i = 0; i < g_tweak_count && i < MAX_TWEAKS; i++) t->before[i] = g_tweaks[i].enabled;

    for (int k = 0; k < t->count; k++) {
        int i = t->indices[k];
        Tweak *tw = &g_tweaks[i];
        if (tw->enabled == t->targets[k]) continue;

        char step[192];
        _snprintf(step, sizeof(step), "%s %s\xE2\x80\xA6",
                  t->targets[k] ? "Turning on" : "Turning off", tw->name);
        step[sizeof(step) - 1] = '\0';
        InterlockedExchange(&g_in_progress, i);
        gui_job_step(job, k, t->count, step);

        t->attempted++;
        if (tweaks_toggle(i) != 0) t->failed++;
    }
    InterlockedExchange(&g_in_progress, -1);

    /* Some tweaks affect each other (the two power-plan tweaks share the active scheme), and
     * a re-read is the only honest way to report what actually stuck. Off the UI thread it
     * costs nothing visible. */
    gui_job_step(job, t->count, t->count, "Re-reading system state\xE2\x80\xA6");
    tweaks_refresh_state();
}

static void undo_last(void);

static void tweak_done(GuiJob *job) {
    TweakJob *t = (TweakJob *)job->ctx;

    int changed_index[MAX_TWEAKS], changed_before[MAX_TWEAKS], changed = 0;
    for (int i = 0; i < g_tweak_count && i < MAX_TWEAKS; i++) {
        if (g_tweaks[i].enabled != t->before[i]) {
            changed_index[changed] = i;
            changed_before[changed] = t->before[i];
            changed++;
        }
    }

    char msg[320];
    UiTone tone = t->failed ? TONE_WARN : TONE_GOOD;
    if (t->count == 1 && t->skipped_admin == 0) {
        Tweak *tw = &g_tweaks[t->indices[0]];
        int target = t->targets[0];
        if (tw->enabled == target) {
            _snprintf(msg, sizeof(msg), "%s \xE2\x80\x9C%s\xE2\x80\x9D%s",
                      t->is_undo ? "Undid the change to" : target ? "Turned on" : "Turned off", tw->name,
                      (tw->requires_reboot && gui_restart_pending_for(t->indices[0]))
                          ? " \xE2\x80\x94 restart Windows to finish" : "");
        } else {
            _snprintf(msg, sizeof(msg), "Couldn't %s \xE2\x80\x9C%s\xE2\x80\x9D \xE2\x80\x94 Windows kept the previous setting.",
                      target ? "turn on" : "turn off", tw->name);
        }
    } else {
        int ok = t->attempted - t->failed;
        int n = _snprintf(msg, sizeof(msg), "%s %d setting%s", t->is_undo ? "Undid" : "Changed", ok, ok == 1 ? "" : "s");
        if (t->failed && n > 0 && n < (int)sizeof(msg)) {
            n += _snprintf(msg + n, sizeof(msg) - n, " \xC2\xB7 %d couldn't be changed", t->failed);
        }
        if (t->skipped_admin && n > 0 && n < (int)sizeof(msg)) {
            _snprintf(msg + n, sizeof(msg) - n, " \xC2\xB7 %d skipped (need administrator)", t->skipped_admin);
            if (!t->failed) tone = TONE_WARN;
        }
    }
    msg[sizeof(msg) - 1] = '\0';

    int offer_undo = !t->is_undo && changed > 0;
    if (offer_undo) {
        g_undo_count = changed;
        memcpy(g_undo_index, changed_index, sizeof(int) * (size_t)changed);
        memcpy(g_undo_target, changed_before, sizeof(int) * (size_t)changed);
    } else if (t->is_undo) {
        g_undo_count = 0;
    }

    gui_toast(tone, msg, offer_undo ? "Undo" : NULL, offer_undo ? undo_last : NULL);
    gui_state_changed();
}

static void start_tweak_job(TweakJob *t, const char *label) {
    gui_job_start(label, tweak_work, tweak_done, t);
}

void gui_tweaks_set(const int *indices, int count, int want, const char *label) {
    if (!gui_ready()) {
        gui_toast(TONE_NEUTRAL, "Still scanning your system \xE2\x80\x94 one moment.", NULL, NULL);
        return;
    }
    TweakJob *t = (TweakJob *)calloc(1, sizeof(TweakJob));
    if (!t) return;

    int elevated = is_running_elevated();
    int last_admin = -1;
    for (int k = 0; k < count && t->count < MAX_TWEAKS; k++) {
        int i = indices[k];
        if (i < 0 || i >= g_tweak_count) continue;
        int target = want < 0 ? !g_tweaks[i].enabled : want;
        if (g_tweaks[i].enabled == target) continue;
        if (g_tweaks[i].requires_admin && !elevated) {
            t->skipped_admin++;
            last_admin = i;
            continue;
        }
        t->indices[t->count] = i;
        t->targets[t->count] = target;
        t->count++;
    }

    if (t->count == 0) {
        if (count == 1 && last_admin >= 0) {
            gui_offer_elevation(g_tweaks[last_admin].name);
        } else if (t->skipped_admin) {
            char msg[200];
            _snprintf(msg, sizeof(msg),
                      "All %d remaining change%s need administrator rights \xE2\x80\x94 restart as administrator to apply them.",
                      t->skipped_admin, t->skipped_admin == 1 ? "" : "s");
            msg[sizeof(msg) - 1] = '\0';
            gui_toast(TONE_WARN, msg, "Restart as admin", gui_relaunch_elevated);
        } else {
            gui_toast(TONE_NEUTRAL, "Nothing to change \xE2\x80\x94 already set that way.", NULL, NULL);
        }
        free(t);
        return;
    }
    start_tweak_job(t, label);
}

static void undo_last(void) {
    if (g_undo_count == 0) return;
    TweakJob *t = (TweakJob *)calloc(1, sizeof(TweakJob));
    if (!t) return;
    t->is_undo = 1;

    /* Turn things off before turning things on: when two tweaks share one setting (the power
     * plans), reverting one and then re-applying the other lands on the right final state. */
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < g_undo_count; k++) {
            if (g_undo_target[k] != pass) continue;
            t->indices[t->count] = g_undo_index[k];
            t->targets[t->count] = g_undo_target[k];
            t->count++;
        }
    }
    start_tweak_job(t, "Undoing the last change");
}
