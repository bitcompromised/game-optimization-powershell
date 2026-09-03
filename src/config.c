#include "config.h"

static ConfigEntry g_entries[CONFIG_MAX_ENTRIES];
static int g_entry_count = 0;

void config_get_dir(char *out, size_t out_size) {
    const char *local_appdata = getenv("LOCALAPPDATA");
    if (!local_appdata) local_appdata = ".";
    _snprintf(out, out_size, "%s\\GameOptimizer", local_appdata);
    out[out_size - 1] = '\0';
    CreateDirectoryA(out, NULL);
}

static void config_get_path(char *out, size_t out_size) {
    char dir[MAX_PATH];
    config_get_dir(dir, sizeof(dir));
    _snprintf(out, out_size, "%s\\config.ini", dir);
    out[out_size - 1] = '\0';
}

void config_load(void) {
    g_entry_count = 0;

    char path[MAX_PATH];
    config_get_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f) && g_entry_count < CONFIG_MAX_ENTRIES) {
        trim_newline(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        const char *id = line;
        int value = atoi(eq + 1);

        strncpy(g_entries[g_entry_count].id, id, CONFIG_ID_MAX - 1);
        g_entries[g_entry_count].id[CONFIG_ID_MAX - 1] = '\0';
        g_entries[g_entry_count].value = value;
        g_entry_count++;
    }
    fclose(f);
}

void config_save(void) {
    char path[MAX_PATH];
    config_get_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "# %s config -- do not edit while the app is running\n", APP_NAME);
    for (int i = 0; i < g_entry_count; i++) {
        fprintf(f, "%s=%d\n", g_entries[i].id, g_entries[i].value);
    }
    fclose(f);
}

int config_get(const char *id, int *out) {
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].id, id) == 0) {
            *out = g_entries[i].value;
            return 1;
        }
    }
    return 0;
}

void config_set(const char *id, int value) {
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].id, id) == 0) {
            g_entries[i].value = value;
            return;
        }
    }
    if (g_entry_count < CONFIG_MAX_ENTRIES) {
        strncpy(g_entries[g_entry_count].id, id, CONFIG_ID_MAX - 1);
        g_entries[g_entry_count].id[CONFIG_ID_MAX - 1] = '\0';
        g_entries[g_entry_count].value = value;
        g_entry_count++;
    }
}
