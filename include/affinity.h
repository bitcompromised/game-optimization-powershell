#ifndef AFFINITY_H
#define AFFINITY_H

#include "common.h"

/* Core reservation ("game focus").
 *
 * Rather than touching the game -- which a kernel anti-cheat will usually refuse to let us
 * open for write anyway -- this confines every ordinary background process to a small set of
 * logical processors, leaving the rest of the CPU effectively uncontended for the game. The
 * game's own affinity is never modified; it keeps the full mask and the scheduler naturally
 * places its threads on the cores nothing else is allowed to run on.
 *
 * Affinity lives in the process, not the registry: nothing here survives a reboot, and
 * processes started after a focus pass are unaffected until you run it again. */

typedef struct {
    int   total_cores;           /* logical processors in this processor group */
    int   background_cores;      /* how many were left for background work */
    int   adjusted;              /* processes whose affinity actually changed */
    int   denied;                /* processes Windows refused to let us modify */
    int   skipped;               /* protected / exempt processes left alone on purpose */
    int   game_found;
    DWORD game_pid;
    char  game_name[64];
    int   game_priority;         /* 1 raised/restored, 0 not attempted, -1 access denied */
} AffinityResult;

/* Logical processors available to this process's processor group (1..64). */
int affinity_cpu_count(void);

/* How many logical processors a focus pass would leave to background work, given the
 * machine's core count. Always >= 2 and always leaves at least 2 cores for the game. */
int affinity_background_core_count(void);

/* The exact mask a focus pass gives background processes. A process carrying precisely this
 * mask is one we pinned -- unlike "fewer cores than the machine has", which also matches apps
 * that restrict themselves (some games and launchers do). */
DWORD_PTR affinity_focus_mask(void);

/* Processes currently carrying the focus mask, i.e. what "Release" would put back. Opens every
 * process once, so it's cheap but not free -- call it on demand, not per frame. */
int affinity_count_pinned(void);

/* 1 if `name` is one of `game_exes` (case-insensitive). */
int affinity_is_game(const char *name, const char *const *game_exes, int game_exe_count);

/* Confines background processes to the low cores and raises `game_exes` to High priority.
 * `game_exes` is a NULL-terminated-by-count array of executable names (case-insensitive);
 * the first one found running is treated as the game. Fills `out`. Returns 0 on success,
 * -1 if the machine has too few cores to split meaningfully. */
int affinity_focus_game(const char *const *game_exes, int game_exe_count, AffinityResult *out);

/* Restores every process carrying the focus mask to the full system affinity mask and puts the
 * game back to Normal priority. Processes with an affinity they set themselves are left alone.
 * Fills `out`. Returns 0. */
int affinity_reset(const char *const *game_exes, int game_exe_count, AffinityResult *out);

/* The Valorant game client, then its launcher, most-specific first. */
extern const char *const AFFINITY_VALORANT_EXES[];
extern const int AFFINITY_VALORANT_EXE_COUNT;

#endif
