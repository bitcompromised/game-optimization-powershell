#include "gui.h"
#include "ps_utils.h"
#include "svc_monitor.h"
#include "svc_utils.h"
#include "affinity.h"

#define ID_RESCAN       300
#define ID_REPORT       301
#define ID_TILE_PERF    302
#define ID_TILE_PRIVACY 303
#define ID_TILE_FOCUS   304
#define ID_TILE_REVERT  305
#define ID_RUN_TRIM     306
#define ID_RUN_DNS      307
#define ID_RUN_TEMP     308

#define TIMER_SAMPLE 1
#define SAMPLE_MS    1000
#define HISTORY      60

static HWND g_panel;
static HWND g_rescan, g_report, g_tiles[4], g_runs[3];
static RECT g_score, g_cpu, g_mem, g_sys, g_maint;
static int g_width, g_height;
static int g_oy;          /* current scroll offset, applied to everything painted */
static int g_visible;

#define ROW1_H     172
#define TILE_H     80
#define BOTTOM_MIN 250
#define SYS_STACKED_H 246   /* narrow layout: System above Maintenance */
#define MAINT_MIN_H   220

/* Below this content width the meters and tiles reflow instead of squeezing their text. */
static int is_narrow(int w) { return w - ui_px(28) * 2 < ui_px(860); }

static float g_cpu_hist[HISTORY], g_mem_hist[HISTORY];
static int g_hist;
static ULONGLONG g_prev_idle, g_prev_total;
static double g_cpu_now;
static MEMORYSTATUSEX g_memstat;
static int g_services_disabled = -1, g_services_known;

typedef struct {
    WCHAR glyph;
    const char *title;
    const char *sub;
} MaintRow;

static const MaintRow MAINT[3] = {
    { GLYPH_DRIVE, "Optimize SSDs (TRIM)", "Re-sends TRIM to every NTFS volume" },
    { GLYPH_GLOBE, "Flush DNS cache", "Clears stale name lookups" },
    { GLYPH_ERASE, "Clean temp files", "Empties your and Windows' temp folders" },
};

/* ---- live samples ---- */

static ULONGLONG ft64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static void sample(void) {
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        ULONGLONG i = ft64(idle), total = ft64(kernel) + ft64(user);   /* kernel time includes idle */
        if (g_prev_total && total > g_prev_total) {
            ULONGLONG dt = total - g_prev_total, di = i - g_prev_idle;
            g_cpu_now = dt ? 100.0 * (double)(dt - (di > dt ? dt : di)) / (double)dt : 0.0;
        }
        g_prev_idle = i;
        g_prev_total = total;
    }
    g_memstat.dwLength = sizeof(g_memstat);
    GlobalMemoryStatusEx(&g_memstat);

    if (g_hist == HISTORY) {
        memmove(g_cpu_hist, g_cpu_hist + 1, sizeof(float) * (HISTORY - 1));
        memmove(g_mem_hist, g_mem_hist + 1, sizeof(float) * (HISTORY - 1));
        g_hist--;
    }
    g_cpu_hist[g_hist] = (float)g_cpu_now;
    g_mem_hist[g_hist] = (float)g_memstat.dwMemoryLoad;
    g_hist++;
}

static void count_services(void) {
    g_services_known = 0;
    g_services_disabled = 0;
    for (int i = 0; i < KNOWN_SERVICE_COUNT; i++) {
        DWORD start;
        if (svc_query_start_type(KNOWN_SERVICES[i].service_name, &start) != 0) continue;
        g_services_known++;
        if (start == SERVICE_DISABLED) g_services_disabled++;
    }
}

static void tweak_counts(TweakCategory cat, int *on, int *total) {
    *on = *total = 0;
    for (int i = 0; i < g_tweak_count; i++) {
        if (g_tweaks[i].category != cat) continue;
        (*total)++;
        *on += g_tweaks[i].enabled;
    }
}

/* "11th Gen Intel(R) Core(TM) i7-1165G7 @ 2.80GHz" -> "Intel Core i7-1165G7" */
static void short_cpu_name(const char *in, char *out, size_t size) {
    char tmp[160];
    size_t k = 0;
    for (const char *p = in; *p && k < sizeof(tmp) - 1; p++) {
        if (strncmp(p, "(R)", 3) == 0 || strncmp(p, "(r)", 3) == 0) { p += 2; continue; }
        if (strncmp(p, "(TM)", 4) == 0 || strncmp(p, "(tm)", 4) == 0) { p += 3; continue; }
        tmp[k++] = *p;
    }
    tmp[k] = '\0';
    char *at = strstr(tmp, " @");
    if (at) *at = '\0';
    char *cpu = strstr(tmp, " CPU");
    if (cpu && cpu[4] == '\0') *cpu = '\0';
    const char *start = tmp;
    const char *gen = strstr(tmp, "Gen ");
    if (gen && gen - tmp < 8) start = gen + 4;
    size_t o = 0;
    int space = 0;
    for (const char *p = start; *p && o < size - 1; p++) {
        if (*p == ' ') { if (!space && o) out[o++] = ' '; space = 1; }
        else { out[o++] = *p; space = 0; }
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

/* ---- tiles ---- */

static void update_tiles(void) {
    char sub[160];
    int on, total;

    tweak_counts(CAT_PERFORMANCE, &on, &total);
    if (!gui_ready()) _snprintf(sub, sizeof(sub), "Reading your settings\xE2\x80\xA6");
    else if (on < total) _snprintf(sub, sizeof(sub), "%d of %d are off", total - on, total);
    else _snprintf(sub, sizeof(sub), "All %d are active", total);
    sub[sizeof(sub) - 1] = '\0';
    ui_button_set_sub(g_tiles[0], sub);

    tweak_counts(CAT_TELEMETRY, &on, &total);
    if (!gui_ready()) _snprintf(sub, sizeof(sub), "Reading your settings\xE2\x80\xA6");
    else if (on < total) _snprintf(sub, sizeof(sub), "%d of %d are off", total - on, total);
    else _snprintf(sub, sizeof(sub), "All %d are on", total);
    sub[sizeof(sub) - 1] = '\0';
    ui_button_set_sub(g_tiles[1], sub);

    int pinned = gui_processes_pinned();
    int cores = affinity_cpu_count();
    if (pinned > 0) {
        ui_button_set_text(g_tiles[2], "Release CPU cores");
        _snprintf(sub, sizeof(sub), "%d process%s pinned", pinned, pinned == 1 ? "" : "es");
        ui_button_set_tone(g_tiles[2], TONE_WARN);
    } else {
        ui_button_set_text(g_tiles[2], "Focus on Valorant");
        if (cores >= 4) {
            _snprintf(sub, sizeof(sub), "Reserve %d of %d cores", cores - affinity_background_core_count(), cores);
        } else {
            _snprintf(sub, sizeof(sub), "Needs at least 4 logical processors");
        }
        ui_button_set_tone(g_tiles[2], TONE_ACCENT);
    }
    sub[sizeof(sub) - 1] = '\0';
    ui_button_set_sub(g_tiles[2], sub);
}

/* ---- actions ---- */

static void revert_everything(void) {
    if (!gui_ready()) return;
    int idx[128], n = 0;
    for (int i = 0; i < g_tweak_count && n < 128; i++) {
        if (g_tweaks[i].enabled) idx[n++] = i;
    }
    if (n == 0) {
        gui_toast(TONE_NEUTRAL, "Nothing to revert \xE2\x80\x94 every tweak is already at the Windows default.", NULL, NULL);
        return;
    }
    char body[400];
    _snprintf(body, sizeof(body),
              "%d performance and privacy tweaks are on. Each will be put back to the Windows default.\n\n"
              "Settings that need administrator rights are skipped when GameOptimizer isn't elevated. "
              "You can undo this from the status bar afterwards.", n);
    body[sizeof(body) - 1] = '\0';
    UiDialog d;
    memset(&d, 0, sizeof(d));
    d.heading = "Revert everything?";
    d.body = body;
    d.ok = "Revert everything";
    d.cancel = "Cancel";
    d.danger = 1;
    d.tone = TONE_WARN;
    d.glyph = GLYPH_UNDO;
    if (ui_dialog(gui_main_window(), &d)) gui_tweaks_set(idx, n, 0, "Reverting everything");
}

static void apply_category(TweakCategory cat) {
    int idx[128], n = 0, runnable = 0, total = 0;
    int elevated = is_running_elevated();
    for (int i = 0; i < g_tweak_count && n < 128; i++) {
        if (g_tweaks[i].category != cat) continue;
        total++;
        if (g_tweaks[i].enabled) continue;
        idx[n++] = i;
        if (!g_tweaks[i].requires_admin || elevated) runnable++;
    }
    if (!gui_ready()) return;
    if (n == 0) {
        gui_toast(TONE_GOOD, cat == CAT_PERFORMANCE ? "Every performance tweak is already active."
                                                    : "Every privacy protection is already on.", NULL, NULL);
        return;
    }
    if (runnable == 0) {
        gui_offer_elevation(cat == CAT_PERFORMANCE ? "Apply all performance tweaks" : "Block all tracking");
        return;
    }
    char body[400];
    int len = _snprintf(body, sizeof(body), "%d of %d %s will be turned on.", runnable, total,
                        cat == CAT_PERFORMANCE ? "performance tweaks" : "privacy protections");
    if (n > runnable && len > 0) {
        len += _snprintf(body + len, sizeof(body) - len,
                         "\n\n%d more need administrator rights and will be skipped.", n - runnable);
    }
    if (len > 0) _snprintf(body + len, sizeof(body) - len, "\n\nYou can undo this from the status bar afterwards.");
    body[sizeof(body) - 1] = '\0';
    char ok[48];
    _snprintf(ok, sizeof(ok), "Turn on %d", runnable);
    ok[sizeof(ok) - 1] = '\0';

    UiDialog d;
    memset(&d, 0, sizeof(d));
    d.heading = cat == CAT_PERFORMANCE ? "Apply every performance tweak?" : "Block all Windows tracking?";
    d.body = body;
    d.ok = ok;
    d.cancel = "Cancel";
    d.tone = cat == CAT_PERFORMANCE ? TONE_ACCENT : TONE_GOOD;
    d.glyph = cat == CAT_PERFORMANCE ? GLYPH_BOLT : GLYPH_SHIELD;
    if (ui_dialog(gui_main_window(), &d)) gui_tweaks_set(idx, n, 1, "Applying");
}

typedef struct {
    int kind;
    int rc;
    long freed_mb;
} MaintJob;

static void maint_work(GuiJob *job) {
    MaintJob *m = (MaintJob *)job->ctx;
    switch (m->kind) {
        case 0:
            gui_job_step(job, 0, 1, "Optimizing drives (TRIM) \xE2\x80\x94 this can take a minute\xE2\x80\xA6");
            m->rc = ps_run("Get-Volume | Where-Object { $_.DriveLetter -and $_.FileSystemType -eq 'NTFS' } | "
                           "ForEach-Object { Optimize-Volume -DriveLetter $_.DriveLetter -ReTrim -ErrorAction SilentlyContinue }\r\n");
            break;
        case 1:
            gui_job_step(job, 0, 1, "Flushing the DNS cache\xE2\x80\xA6");
            m->rc = ps_run("ipconfig /flushdns | Out-Null\r\n");
            break;
        default: {
            gui_job_step(job, 0, 1, "Cleaning temp files\xE2\x80\xA6");
            const char *script =
                "$dirs = @($env:TEMP, \"$env:SystemRoot\\Temp\")\r\n"
                "function Size($d) { (Get-ChildItem -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue | "
                "Where-Object { -not $_.PSIsContainer } | Measure-Object -Property Length -Sum).Sum }\r\n"
                "$before = 0; foreach ($d in $dirs) { $before += [double](Size $d) }\r\n"
                "foreach ($d in $dirs) { Remove-Item -Path (Join-Path $d '*') -Recurse -Force -ErrorAction SilentlyContinue }\r\n"
                "$after = 0; foreach ($d in $dirs) { $after += [double](Size $d) }\r\n"
                "$__result = [math]::Max(0, [math]::Round(($before - $after) / 1MB))\r\n";
            m->rc = ps_capture_int(script, &m->freed_mb);
            break;
        }
    }
}

static void maint_done(GuiJob *job) {
    MaintJob *m = (MaintJob *)job->ctx;
    char msg[200];
    if (m->rc != 0) {
        gui_toast(TONE_WARN, "The maintenance command didn't complete successfully.", NULL, NULL);
        return;
    }
    switch (m->kind) {
        case 0: gui_toast(TONE_GOOD, "Drive optimization finished.", NULL, NULL); break;
        case 1: gui_toast(TONE_GOOD, "DNS cache cleared.", NULL, NULL); break;
        default:
            _snprintf(msg, sizeof(msg), "Freed %ld MB of temp files%s.", m->freed_mb,
                      is_running_elevated() ? "" : " (files in use and Windows' own temp folder were skipped)");
            msg[sizeof(msg) - 1] = '\0';
            gui_toast(TONE_GOOD, msg, NULL, NULL);
            break;
    }
}

static void run_maintenance(int kind) {
    if (kind != 1) {
        UiDialog d;
        memset(&d, 0, sizeof(d));
        d.heading = kind == 0 ? "Optimize your SSDs now?" : "Delete temporary files?";
        d.body = kind == 0
            ? "Windows re-sends TRIM for every NTFS volume so the drive can reclaim freed blocks. "
              "It's safe and runs in the background; large drives can take a minute."
            : "Deletes everything in your temp folder and Windows' temp folder. Files that apps have "
              "open right now are skipped automatically.";
        d.ok = kind == 0 ? "Optimize" : "Clean up";
        d.cancel = "Cancel";
        d.tone = TONE_ACCENT;
        d.glyph = MAINT[kind].glyph;
        if (!ui_dialog(gui_main_window(), &d)) return;
    }
    MaintJob *m = (MaintJob *)calloc(1, sizeof(MaintJob));
    if (!m) return;
    m->kind = kind;
    gui_job_start(MAINT[kind].title, maint_work, maint_done, m);
}

static void copy_report(void) {
    size_t cap = 16384, len = 0;
    char *r = (char *)malloc(cap);
    if (!r) return;
    r[0] = '\0';
#define ADD(...) do { if (len < cap) { int w_ = _snprintf(r + len, cap - len, __VA_ARGS__); if (w_ > 0) len += (size_t)w_; else len = cap; } } while (0)
    const SysInfo *s = gui_sysinfo();
    SYSTEMTIME now;
    GetLocalTime(&now);
    ADD("GameOptimizer %s system report \xE2\x80\x94 %04d-%02d-%02d %02d:%02d\r\n\r\n", APP_VERSION,
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
    if (gui_sysinfo_ready()) {
        ADD("Windows:     %s %s (build %s)\r\n", s->os_name, s->os_display_version, s->os_build);
        ADD("CPU:         %s (%lu cores / %lu threads)\r\n", s->cpu_name, (unsigned long)s->cpu_cores, (unsigned long)s->cpu_threads);
        ADD("GPU:         %s\r\n", s->gpu_name);
        ADD("Memory:      %llu MB\r\n", (unsigned long long)s->ram_total_mb);
        ADD("System disk: %llu GB free of %llu GB\r\n", (unsigned long long)s->disk_free_gb, (unsigned long long)s->disk_total_gb);
        ADD("Power plan:  %s\r\n", s->power_plan);
    }
    ADD("Privileges:  %s\r\n", is_running_elevated() ? "Administrator" : "Standard user");
    for (int c = 0; c < 2; c++) {
        TweakCategory cat = c == 0 ? CAT_PERFORMANCE : CAT_TELEMETRY;
        int on, total;
        tweak_counts(cat, &on, &total);
        ADD("\r\n%s (%d of %d on)\r\n", c == 0 ? "PERFORMANCE" : "PRIVACY", on, total);
        for (int i = 0; i < g_tweak_count; i++) {
            if (g_tweaks[i].category != cat) continue;
            ADD("  [%s] %s\r\n", g_tweaks[i].enabled ? "on " : "off", g_tweaks[i].name);
        }
    }
#undef ADD
    r[cap - 1] = '\0';
    if (ui_copy_text(gui_main_window(), r) == 0) {
        gui_toast(TONE_GOOD, "System report copied \xE2\x80\x94 paste it anywhere.", NULL, NULL);
    }
    free(r);
}

/* ---- painting ---- */

static void card_title(HDC dc, const RECT *card, const char *title) {
    RECT r = { card->left + ui_px(20), card->top + ui_px(16), card->right - ui_px(20), card->top + ui_px(34) };
    ui_text(dc, title, &r, FONT_SMALL_BOLD, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

static void bar_row(HDC dc, int x, int y, int w, const char *label, int value, int total, COLORREF color) {
    RECT lr = { x, y, x + w, y + ui_px(18) };
    ui_text(dc, label, &lr, FONT_SMALL, g_ui.text_dim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    char num[32];
    if (total > 0) _snprintf(num, sizeof(num), "%d/%d", value, total);
    else _snprintf(num, sizeof(num), "\xE2\x80\x94");
    num[sizeof(num) - 1] = '\0';
    ui_text(dc, num, &lr, FONT_SMALL_BOLD, g_ui.text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT bar = { x, lr.bottom + ui_px(4), x + w, lr.bottom + ui_px(10) };
    ui_progress(dc, &bar, total > 0 ? (double)value / total : 0.0, g_ui.surface3, color);
}

static void paint_score(HDC dc) {
    RECT c = g_score;
    ui_card(dc, &c);
    int ch = c.bottom - c.top;
    int d = ch - ui_px(44);
    if (d > ui_px(108)) d = ui_px(108);
    RECT ring = { c.left + ui_px(22), c.top + (ch - d) / 2, c.left + ui_px(22) + d, c.top + (ch - d) / 2 + d };

    int pon, ptot, ton, ttot;
    tweak_counts(CAT_PERFORMANCE, &pon, &ptot);
    tweak_counts(CAT_TELEMETRY, &ton, &ttot);
    double frac = (ptot + ttot) ? (double)(pon + ton) / (ptot + ttot) : 0.0;
    COLORREF color = frac >= 0.8 ? g_ui.good : frac >= 0.5 ? g_ui.accent : g_ui.warn;
    ui_ring(dc, &ring, ui_px(10), gui_ready() ? frac : 0.0, g_ui.surface3, color);
    char pct[16];
    if (gui_ready()) _snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100 + 0.5));
    else _snprintf(pct, sizeof(pct), "\xE2\x80\xA6");
    pct[sizeof(pct) - 1] = '\0';
    ui_text(dc, pct, &ring, FONT_DISPLAY, g_ui.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int x = ring.right + ui_px(22);
    int w = c.right - ui_px(20) - x;
    RECT t = { x, c.top + ui_px(18), c.right - ui_px(20), c.top + ui_px(36) };
    ui_text(dc, "OPTIMIZATION SCORE", &t, FONT_SMALL_BOLD, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE);
    char line[96];
    if (gui_ready()) _snprintf(line, sizeof(line), "%d of %d active", pon + ton, ptot + ttot);
    else _snprintf(line, sizeof(line), "Scanning\xE2\x80\xA6");
    line[sizeof(line) - 1] = '\0';
    RECT l = { x, t.bottom + ui_px(2), c.right - ui_px(20), t.bottom + ui_px(22) };
    ui_text(dc, line, &l, FONT_BODY_BOLD, g_ui.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    int y = l.bottom + ui_px(10);
    int step = (c.bottom - ui_px(16) - y) / 3;
    if (step > ui_px(34)) step = ui_px(34);
    bar_row(dc, x, y, w, "Performance", gui_ready() ? pon : 0, gui_ready() ? ptot : 0, g_ui.accent);
    bar_row(dc, x, y + step, w, "Privacy", gui_ready() ? ton : 0, gui_ready() ? ttot : 0, g_ui.good);
    bar_row(dc, x, y + step * 2, w, "Services off", g_services_disabled < 0 ? 0 : g_services_disabled,
            g_services_disabled < 0 ? 0 : g_services_known, g_ui.warn);
}

static void paint_meter(HDC dc, const RECT *card, const char *title, double value, const char *sub,
                        const float *hist, COLORREF color) {
    ui_card(dc, card);
    card_title(dc, card, title);
    char v[16];
    _snprintf(v, sizeof(v), "%d%%", (int)(value + 0.5));
    v[sizeof(v) - 1] = '\0';
    RECT vr = { card->left + ui_px(20), card->top + ui_px(36), card->right - ui_px(20), card->top + ui_px(78) };
    ui_text(dc, g_hist ? v : "\xE2\x80\x94", &vr, FONT_DISPLAY, g_ui.text, DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT sr = { card->left + ui_px(20), vr.bottom, card->right - ui_px(20), vr.bottom + ui_px(18) };
    ui_text(dc, sub, &sr, FONT_SMALL, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT spark = { card->left + ui_px(20), sr.bottom + ui_px(10), card->right - ui_px(20), card->bottom - ui_px(16) };
    if (spark.bottom - spark.top > ui_px(12)) {
        ui_hline(dc, spark.left, spark.right, spark.bottom - 1, g_ui.border);
        ui_sparkline(dc, &spark, hist, g_hist, 100.0f, color);
    }
}

static void kv_row(HDC dc, int x, int y, int w, const char *key, const char *value) {
    RECT k = { x, y, x + ui_px(104), y + ui_px(22) };
    ui_text(dc, key, &k, FONT_SMALL, g_ui.text_dim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT v = { k.right, y, x + w, y + ui_px(22) };
    ui_text(dc, value, &v, FONT_BODY, g_ui.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_system(HDC dc) {
    ui_card(dc, &g_sys);
    card_title(dc, &g_sys, "THIS PC");
    const SysInfo *s = gui_sysinfo();
    int x = g_sys.left + ui_px(20), w = g_sys.right - ui_px(20) - x;
    int y = g_sys.top + ui_px(42);
    int step = ui_px(26);

    HRGN clip = CreateRectRgn(g_sys.left, g_sys.top, g_sys.right, g_sys.bottom - ui_px(8));
    SelectClipRgn(dc, clip);

    char buf[256];
    if (!gui_sysinfo_ready()) {
        RECT r = { x, y, x + w, y + step };
        ui_text(dc, "Reading hardware information\xE2\x80\xA6", &r, FONT_BODY, g_ui.text_dim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        _snprintf(buf, sizeof(buf), "%s %s \xC2\xB7 build %s", s->os_name, s->os_display_version, s->os_build);
        buf[sizeof(buf) - 1] = '\0';
        kv_row(dc, x, y, w, "Windows", buf); y += step;
        char cpu[128];
        short_cpu_name(s->cpu_name, cpu, sizeof(cpu));
        _snprintf(buf, sizeof(buf), "%s \xC2\xB7 %lu cores / %lu threads", cpu, (unsigned long)s->cpu_cores, (unsigned long)s->cpu_threads);
        buf[sizeof(buf) - 1] = '\0';
        kv_row(dc, x, y, w, "Processor", buf); y += step;
        kv_row(dc, x, y, w, "Graphics", s->gpu_name); y += step;
        _snprintf(buf, sizeof(buf), "%.1f GB", s->ram_total_mb / 1024.0);
        buf[sizeof(buf) - 1] = '\0';
        kv_row(dc, x, y, w, "Memory", buf); y += step;
        _snprintf(buf, sizeof(buf), "%llu GB free of %llu GB", (unsigned long long)s->disk_free_gb, (unsigned long long)s->disk_total_gb);
        buf[sizeof(buf) - 1] = '\0';
        kv_row(dc, x, y, w, "System drive", buf); y += step;
        kv_row(dc, x, y, w, "Power plan", s->power_plan); y += step;
    }
    ULONGLONG minutes = GetTickCount64() / 60000ULL;
    if (minutes >= 60 * 24) _snprintf(buf, sizeof(buf), "%llud %lluh %llum", minutes / 1440, (minutes / 60) % 24, minutes % 60);
    else _snprintf(buf, sizeof(buf), "%lluh %llum", minutes / 60, minutes % 60);
    buf[sizeof(buf) - 1] = '\0';
    if (gui_sysinfo_ready()) kv_row(dc, x, y, w, "Uptime", buf);

    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

static int maint_row_height(void) {
    int h = (g_maint.bottom - g_maint.top - ui_px(50)) / 3;
    if (h > ui_px(54)) h = ui_px(54);
    return h;
}

static void paint_maintenance(HDC dc) {
    ui_card(dc, &g_maint);
    card_title(dc, &g_maint, "MAINTENANCE");
    int rh = maint_row_height();
    int y = g_maint.top + ui_px(42);
    for (int i = 0; i < 3; i++) {
        RECT row = { g_maint.left + ui_px(20), y + i * rh, g_maint.right - ui_px(20), y + (i + 1) * rh };
        if (i > 0) ui_hline(dc, row.left, row.right, row.top, g_ui.border);
        RECT ic = { row.left, row.top + (rh - ui_px(32)) / 2, row.left + ui_px(32), row.top + (rh + ui_px(32)) / 2 };
        ui_round_fill(dc, &ic, ui_px(8), g_ui.surface2);
        ui_glyph(dc, MAINT[i].glyph, &ic, FONT_ICON, g_ui.text_dim);
        RECT btn;
        GetWindowRect(g_runs[i], &btn);
        MapWindowPoints(NULL, g_panel, (POINT *)&btn, 2);
        int tx = ic.right + ui_px(12);
        int mid = (row.top + row.bottom) / 2;
        RECT t = { tx, row.top, btn.left - ui_px(10), mid + ui_px(1) };
        ui_text(dc, MAINT[i].title, &t, FONT_BODY_BOLD, g_ui.text, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT s = { tx, mid + ui_px(2), btn.left - ui_px(10), row.bottom };
        ui_text(dc, MAINT[i].sub, &s, FONT_SMALL, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void dash_paint(HDC dc, const RECT *client) {
    (void)client;
    int pad = ui_px(28);
    RECT title = { pad, g_oy + pad - ui_px(4), g_width / 2, g_oy + pad + ui_px(34) };
    ui_text(dc, "Dashboard", &title, FONT_TITLE, g_ui.text, DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT sub = { pad, title.bottom + ui_px(4), g_width - pad, title.bottom + ui_px(26) };
    ui_text(dc, "Everything here is read live from Windows \xE2\x80\x94 nothing is cached between runs.",
            &sub, FONT_BODY, g_ui.text_dim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    paint_score(dc);

    char msub[128];
    if (gui_sysinfo_ready()) {
        char cpu[128];
        short_cpu_name(gui_sysinfo()->cpu_name, cpu, sizeof(cpu));
        _snprintf(msub, sizeof(msub), "%s", cpu);
    } else {
        _snprintf(msub, sizeof(msub), "%lu logical processors", (unsigned long)affinity_cpu_count());
    }
    msub[sizeof(msub) - 1] = '\0';
    paint_meter(dc, &g_cpu, "CPU", g_cpu_now, msub, g_cpu_hist, g_ui.accent);

    double free_gb = g_memstat.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
    double total_gb = g_memstat.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    _snprintf(msub, sizeof(msub), "%.1f GB free of %.1f GB", free_gb, total_gb);
    msub[sizeof(msub) - 1] = '\0';
    paint_meter(dc, &g_mem, "MEMORY", g_memstat.dwMemoryLoad, msub, g_mem_hist, g_ui.good);

    paint_system(dc);
    paint_maintenance(dc);
}

/* ---- layout & lifecycle ---- */

static int dash_content_height(int w) {
    int pad = ui_px(28), gap = ui_px(16);
    int narrow = is_narrow(w);
    int h = pad + ui_px(76) + ui_px(ROW1_H) + gap;
    if (narrow) h += ui_px(ROW1_H) + gap;
    h += (ui_px(TILE_H) + gap) * (narrow ? 2 : 1);
    if (narrow) return h + ui_px(SYS_STACKED_H) + gap + ui_px(MAINT_MIN_H) + ui_px(20);
    return h + ui_px(BOTTOM_MIN) + ui_px(20);
}

static void dash_layout(int w, int h) {
    g_width = w;
    g_height = h;
    g_oy = -g_page_dashboard.scroll_y;
    int pad = ui_px(28), gap = ui_px(16), bh = ui_px(36);
    int narrow = is_narrow(w);

    int rw = ui_button_width(g_rescan), cw = ui_button_width(g_report);
    MoveWindow(g_rescan, w - pad - rw, g_oy + pad, rw, bh, TRUE);
    MoveWindow(g_report, w - pad - rw - ui_px(8) - cw, g_oy + pad, cw, bh, TRUE);

    int y = g_oy + pad + ui_px(76);
    int row1 = ui_px(ROW1_H);
    int inner = w - pad * 2;
    if (!narrow) {
        /* The score card carries more text than the two meters, so it gets the extra width. */
        int col = (inner - gap * 2) * 30 / 100;
        int score_w = inner - (col * 2 + gap * 2);
        SetRect(&g_score, pad, y, pad + score_w, y + row1);
        SetRect(&g_cpu, g_score.right + gap, y, g_score.right + gap + col, y + row1);
        SetRect(&g_mem, g_cpu.right + gap, y, w - pad, y + row1);
        y += row1 + gap;
    } else {
        SetRect(&g_score, pad, y, w - pad, y + row1);
        y += row1 + gap;
        int half = (inner - gap) / 2;
        SetRect(&g_cpu, pad, y, pad + half, y + row1);
        SetRect(&g_mem, pad + half + gap, y, w - pad, y + row1);
        y += row1 + gap;
    }

    int th = ui_px(TILE_H);
    int per_row = narrow ? 2 : 4;
    int tw = (inner - gap * (per_row - 1)) / per_row;
    for (int i = 0; i < 4; i++) {
        int x = pad + (i % per_row) * (tw + gap);
        int ty = y + (i / per_row) * (th + gap);
        MoveWindow(g_tiles[i], x, ty, (i % per_row) == per_row - 1 ? w - pad - x : tw, th, TRUE);
    }
    y += (4 / per_row) * (th + gap);

    /* Fill the remaining height when everything fits; otherwise the panel scrolls and the
     * bottom cards keep a readable minimum. */
    if (narrow) {
        SetRect(&g_sys, pad, y, w - pad, y + ui_px(SYS_STACKED_H));
        y = g_sys.bottom + gap;
        int remaining = (h - ui_px(20)) - y;
        SetRect(&g_maint, pad, y, w - pad, y + (remaining > ui_px(MAINT_MIN_H) ? remaining : ui_px(MAINT_MIN_H)));
    } else {
        int remaining = (h - ui_px(20)) - y;
        int bottom = y + (remaining > ui_px(BOTTOM_MIN) ? remaining : ui_px(BOTTOM_MIN));
        int sys_w = (inner - gap) * 55 / 100;
        SetRect(&g_sys, pad, y, pad + sys_w, bottom);
        SetRect(&g_maint, g_sys.right + gap, y, w - pad, bottom);
    }

    int rh = maint_row_height();
    int ry = g_maint.top + ui_px(42);
    int btn_w = ui_px(76), btn_h = ui_px(30);
    for (int i = 0; i < 3; i++) {
        MoveWindow(g_runs[i], g_maint.right - ui_px(20) - btn_w, ry + i * rh + (rh - btn_h) / 2, btn_w, btn_h, TRUE);
    }
}

static void dash_create(HWND panel) {
    g_panel = panel;
    g_rescan = ui_button(panel, ID_RESCAN, BTN_SECONDARY, GLYPH_REFRESH, "Re-scan");
    g_report = ui_button(panel, ID_REPORT, BTN_GHOST, GLYPH_COPY, "Copy report");

    g_tiles[0] = ui_button(panel, ID_TILE_PERF, BTN_TILE, GLYPH_BOLT, "Boost performance");
    g_tiles[1] = ui_button(panel, ID_TILE_PRIVACY, BTN_TILE, GLYPH_SHIELD, "Block tracking");
    g_tiles[2] = ui_button(panel, ID_TILE_FOCUS, BTN_TILE, GLYPH_GAME, "Focus on Valorant");
    g_tiles[3] = ui_button(panel, ID_TILE_REVERT, BTN_TILE, GLYPH_UNDO, "Revert everything");
    ui_button_set_tone(g_tiles[0], TONE_ACCENT);
    ui_button_set_tone(g_tiles[1], TONE_GOOD);
    ui_button_set_tone(g_tiles[2], TONE_ACCENT);
    ui_button_set_tone(g_tiles[3], TONE_BAD);
    ui_button_set_sub(g_tiles[3], "Windows defaults");

    for (int i = 0; i < 3; i++) {
        g_runs[i] = ui_button(panel, ID_RUN_TRIM + i, BTN_SECONDARY, 0, "Run");
        ui_button_set_backdrop(g_runs[i], BACK_SURFACE);
    }
    sample();
    update_tiles();
}

static void dash_refresh(void) {
    count_services();
    update_tiles();
    InvalidateRect(g_panel, NULL, FALSE);
}

static void dash_show(void) {
    g_visible = 1;
    sample();
    dash_refresh();
    if (gui_ready()) gui_status("Ready \xE2\x80\x94 every state shown is read live from Windows");
    SetTimer(g_panel, TIMER_SAMPLE, SAMPLE_MS, NULL);
}

static void dash_hide(void) {
    g_visible = 0;
    KillTimer(g_panel, TIMER_SAMPLE);
}

static void dash_timer(UINT_PTR id) {
    if (id != TIMER_SAMPLE || !g_visible || IsIconic(gui_main_window())) return;
    sample();
    InvalidateRect(g_panel, &g_cpu, FALSE);
    InvalidateRect(g_panel, &g_mem, FALSE);
    InvalidateRect(g_panel, &g_sys, FALSE);
}

static int dash_command(WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    switch (LOWORD(wparam)) {
        case ID_RESCAN:       gui_rescan(1); return 1;
        case ID_REPORT:       copy_report(); return 1;
        case ID_TILE_PERF:    apply_category(CAT_PERFORMANCE); return 1;
        case ID_TILE_PRIVACY: apply_category(CAT_TELEMETRY); return 1;
        case ID_TILE_FOCUS:   gui_processes_focus_toggle(); update_tiles(); return 1;
        case ID_TILE_REVERT:  revert_everything(); return 1;
        case ID_RUN_TRIM:     run_maintenance(0); return 1;
        case ID_RUN_DNS:      run_maintenance(1); return 1;
        case ID_RUN_TEMP:     run_maintenance(2); return 1;
    }
    return 0;
}

static void dash_reload(void) { gui_rescan(1); }

static int dash_badge(char *buf, size_t size) {
    if (!gui_ready()) return 0;
    int pon, ptot, ton, ttot;
    tweak_counts(CAT_PERFORMANCE, &pon, &ptot);
    tweak_counts(CAT_TELEMETRY, &ton, &ttot);
    _snprintf(buf, size, "%d%%", (ptot + ttot) ? (pon + ton) * 100 / (ptot + ttot) : 0);
    buf[size - 1] = '\0';
    return 1;
}

GuiPage g_page_dashboard = {
    "Dashboard", GLYPH_HOME, dash_create, dash_layout, dash_paint, dash_show, dash_hide,
    dash_command, NULL, dash_timer, NULL, dash_refresh, dash_reload, NULL, dash_badge,
    dash_content_height
};
