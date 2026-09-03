#include "gui.h"
#include <commctrl.h>
#include <shellapi.h>

#define WINDOW_CLASS "GameOptimizerMainWindow"
#define ID_TABS   100
#define ID_STATUS 101

#define TAB_HEIGHT   28
#define PAGE_MARGIN  10

static HWND g_main_wnd;
static HWND g_tabs;
static HWND g_status;
static HFONT g_font;
static HFONT g_font_bold;
static HFONT g_font_title;
static int g_current_page = -1;

static GuiPage *g_pages[PAGE_COUNT];

HWND gui_main_window(void) { return g_main_wnd; }
HFONT gui_font(void) { return g_font; }
HFONT gui_font_bold(void) { return g_font_bold; }
HFONT gui_font_title(void) { return g_font_title; }

void gui_apply_font(HWND control) {
    SendMessage(control, WM_SETFONT, (WPARAM)g_font, TRUE);
}

void gui_status(const char *text) {
    if (g_status) {
        SendMessageA(g_status, SB_SETTEXTA, 0, (LPARAM)text);
        UpdateWindow(g_status);
    }
}

void gui_refresh_tweak_state(void) {
    tweaks_refresh_state();
    if (g_current_page >= 0 && g_pages[g_current_page]->show) {
        g_pages[g_current_page]->show();
    }
}

int gui_bulk_toggle(TweakCategory category, int want_enabled) {
    int failed = 0;
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));

    for (int i = 0; i < g_tweak_count; i++) {
        if (g_tweaks[i].category != category) continue;
        if (g_tweaks[i].enabled == want_enabled) continue;

        char msg[256];
        _snprintf(msg, sizeof(msg), "%s %s...",
                  want_enabled ? "Applying" : "Reverting", g_tweaks[i].name);
        msg[sizeof(msg) - 1] = '\0';
        gui_status(msg);

        if (tweaks_toggle(i) != 0) failed++;
    }

    SetCursor(old);
    return failed;
}

static void create_fonts(void) {
    g_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_font_bold = CreateFontA(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_font_title = CreateFontA(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
}

static void layout_pages(void) {
    RECT rc;
    GetClientRect(g_main_wnd, &rc);

    int status_height = 0;
    if (g_status) {
        RECT sr;
        GetWindowRect(g_status, &sr);
        status_height = sr.bottom - sr.top;
    }

    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top - status_height;

    MoveWindow(g_tabs, 0, 0, width, height, TRUE);

    int px = PAGE_MARGIN;
    int py = TAB_HEIGHT + PAGE_MARGIN;
    int pw = width - PAGE_MARGIN * 2;
    int ph = height - TAB_HEIGHT - PAGE_MARGIN * 2;
    if (pw < 0) pw = 0;
    if (ph < 0) ph = 0;

    for (int i = 0; i < PAGE_COUNT; i++) {
        if (g_pages[i]->layout) g_pages[i]->layout(px, py, pw, ph);
    }
}

static void switch_to_page(int index) {
    if (index < 0 || index >= PAGE_COUNT || index == g_current_page) return;

    if (g_current_page >= 0 && g_pages[g_current_page]->hide) {
        g_pages[g_current_page]->hide();
    }
    g_current_page = index;
    if (g_pages[index]->show) g_pages[index]->show();
}

/* Keeps the tab strip and the visible page in sync when the page is changed by keyboard
 * rather than by clicking a tab. */
static void select_page(int index) {
    if (index < 0 || index >= PAGE_COUNT) return;
    SendMessage(g_tabs, TCM_SETCURSEL, index, 0);
    switch_to_page(index);
}

/* Ctrl+Tab / Ctrl+Shift+Tab cycle pages, Ctrl+1..6 jump straight to one. Returns 1 if the
 * key was consumed. */
static int handle_nav_key(MSG *msg) {
    if (msg->message != WM_KEYDOWN) return 0;
    if (!(GetKeyState(VK_CONTROL) & 0x8000)) return 0;

    if (msg->wParam == VK_TAB) {
        int dir = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
        select_page((g_current_page + dir + PAGE_COUNT) % PAGE_COUNT);
        return 1;
    }
    if (msg->wParam >= '1' && msg->wParam <= '0' + PAGE_COUNT) {
        select_page((int)(msg->wParam - '1'));
        return 1;
    }
    return 0;
}

static void add_tab(const char *text, int index) {
    TCITEMA item;
    memset(&item, 0, sizeof(item));
    item.mask = TCIF_TEXT;
    item.pszText = (char *)text;
    SendMessageA(g_tabs, TCM_INSERTITEMA, index, (LPARAM)&item);
}

static void prompt_for_elevation(void) {
    if (is_running_elevated()) return;

    int answer = MessageBoxA(NULL,
        "GameOptimizer is not running as Administrator.\n\n"
        "Tweaks that change system-wide settings or Windows services will fail without "
        "elevation, and they will show as OFF even after you try to enable them.\n\n"
        "Relaunch as Administrator now?",
        APP_NAME, MB_YESNO | MB_ICONWARNING);

    if (answer != IDYES) return;

    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

    HINSTANCE result = ShellExecuteA(NULL, "runas", exe_path, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result > 32) {
        ExitProcess(0);
    }

    MessageBoxA(NULL, "Elevation was cancelled. Continuing without Administrator rights -- "
                      "tweaks marked \"admin\" will not work.",
                APP_NAME, MB_OK | MB_ICONINFORMATION);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE: {
            g_main_wnd = hwnd;

            g_tabs = CreateWindowExA(0, WC_TABCONTROLA, "",
                                     WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                     0, 0, 0, 0, hwnd, (HMENU)ID_TABS, NULL, NULL);
            gui_apply_font(g_tabs);

            g_status = CreateWindowExA(0, STATUSCLASSNAMEA, "",
                                       WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                       0, 0, 0, 0, hwnd, (HMENU)ID_STATUS, NULL, NULL);
            gui_apply_font(g_status);

            add_tab("Dashboard",   PAGE_DASHBOARD);
            add_tab("Performance", PAGE_PERFORMANCE);
            add_tab("Privacy",     PAGE_TELEMETRY);
            add_tab("Services",    PAGE_SERVICES);
            add_tab("Processes",   PAGE_PROCESSES);
            add_tab("Profiles",    PAGE_PROFILES);

            for (int i = 0; i < PAGE_COUNT; i++) {
                if (g_pages[i]->create) g_pages[i]->create(hwnd);
            }

            layout_pages();
            switch_to_page(PAGE_DASHBOARD);
            return 0;
        }

        case WM_SIZE:
            if (g_status) SendMessage(g_status, WM_SIZE, 0, 0);
            if (g_tabs) layout_pages();
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO *mmi = (MINMAXINFO *)lparam;
            mmi->ptMinTrackSize.x = 760;
            mmi->ptMinTrackSize.y = 560;
            return 0;
        }

        case WM_TIMER:
            gui_processes_tick();
            return 0;

        case WM_COMMAND:
            if (g_current_page >= 0 && g_pages[g_current_page]->command &&
                g_pages[g_current_page]->command(wparam, lparam)) {
                return 0;
            }
            break;

        case WM_NOTIFY: {
            LPNMHDR hdr = (LPNMHDR)lparam;
            if (hdr->idFrom == ID_TABS && hdr->code == TCN_SELCHANGE) {
                switch_to_page((int)SendMessage(g_tabs, TCM_GETCURSEL, 0, 0));
                return 0;
            }
            if (g_current_page >= 0 && g_pages[g_current_page]->notify &&
                g_pages[g_current_page]->notify(hdr)) {
                return 0;
            }
            break;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)prev; (void)cmdline;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES |
                ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    create_fonts();
    prompt_for_elevation();

    g_pages[PAGE_DASHBOARD]   = &g_page_dashboard;
    g_pages[PAGE_PERFORMANCE] = &g_page_performance;
    g_pages[PAGE_TELEMETRY]   = &g_page_telemetry;
    g_pages[PAGE_SERVICES]    = &g_page_services;
    g_pages[PAGE_PROCESSES]   = &g_page_processes;
    g_pages[PAGE_PROFILES]    = &g_page_profiles;

    tweaks_refresh_state();

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, WINDOW_CLASS,
                                APP_NAME " v" APP_VERSION " - Windows 11 Game Optimizer",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 940, 680,
                                NULL, NULL, instance, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (handle_nav_key(&msg)) continue;
        if (!IsDialogMessage(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}
