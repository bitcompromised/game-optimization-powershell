#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

#define CONFIG_MAX_ENTRIES 128
#define CONFIG_ID_MAX 64

typedef struct {
    char id[CONFIG_ID_MAX];
    int value;
} ConfigEntry;

/* Loads config.ini from %LOCALAPPDATA%\GameOptimizer\config.ini into memory. Safe to call
 * even if the file doesn't exist yet (starts with zero entries). */
void config_load(void);

/* Writes the in-memory config entries back to disk. */
void config_save(void);

/* Look up a stored value for `id`. Returns 1 if found (and fills *out), 0 if not present. */
int config_get(const char *id, int *out);

/* Insert or update the value for `id` in memory (does not write to disk -- call config_save). */
void config_set(const char *id, int value);

/* Directory where config lives (%LOCALAPPDATA%\GameOptimizer). Ensures it exists. */
void config_get_dir(char *out, size_t out_size);

#endif
