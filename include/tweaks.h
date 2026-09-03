#ifndef TWEAKS_H
#define TWEAKS_H

#include "common.h"

typedef enum {
    CAT_PERFORMANCE,
    CAT_TELEMETRY
} TweakCategory;

typedef struct {
    const char *id;           /* stable key, kept for display/logging purposes */
    const char *name;         /* short display name */
    const char *description;  /* one-line explanation shown in the menu */
    TweakCategory category;
    int requires_admin;
    int (*apply)(void);
    int (*revert)(void);
    int (*is_active)(void);   /* live query of actual system state: 1 active, 0 inactive */
    int enabled;               /* in-memory cache of is_active(), refreshed via tweaks_refresh_state() */
} Tweak;

extern Tweak g_tweaks[];
extern const int g_tweak_count;

/* Populates g_tweaks[i].enabled for every tweak from is_active(). Call on startup and whenever
 * the UI needs to reflect current reality (tab opened, after any apply/revert/disable-all). */
void tweaks_refresh_state(void);

/* Flips the tweak at `index`: if currently off, calls apply(); if on, calls revert().
 * On success, updates in-memory state and persists to config.ini. Returns 0 on success,
 * -1 if the underlying apply/revert failed (state is left unchanged). */
int tweaks_toggle(int index);

#endif
