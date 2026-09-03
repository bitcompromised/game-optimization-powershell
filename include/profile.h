#ifndef PROFILE_H
#define PROFILE_H

#include "common.h"
#include "proc_utils.h"

#define PROFILE_NAME_MAX 64
#define PROFILE_MAX_COUNT 64
#define PROFILE_MAX_SERVICES 64

typedef struct {
    ProcInfo procs_to_close[PROC_MAX];
    int procs_to_close_count;
    int procs_protected_count;    /* would-be closes skipped because they're protected */
    char services_to_stop[PROFILE_MAX_SERVICES][64];
    int services_to_stop_count;
} ProfileDiff;

typedef struct {
    int processes_closed;
    int processes_failed;
    int services_stopped;
    int services_failed;
} ProfileApplyResult;

/* Validates a user-supplied profile name: non-empty, <= PROFILE_NAME_MAX-1 chars,
 * and restricted to [A-Za-z0-9 _-] so it can't escape the profiles directory. */
int profile_name_is_valid(const char *name);

/* Lists saved profile names (without extension), sorted. Returns the count. */
int profile_list(char names[][PROFILE_NAME_MAX], int max_count);

/* Snapshots currently running processes (unique exe names) and currently-running
 * known services, and writes them to a new profile file. Returns 0 on success. */
int profile_save(const char *name);

/* Deletes a saved profile. Returns 0 on success. */
int profile_delete(const char *name);

/* Loads `name`'s saved process/service lists and compares against what's running now.
 * Fills `diff` with what *would* be closed/stopped, without changing anything.
 * Returns 0 on success, -1 if the profile doesn't exist. */
int profile_compute_diff(const char *name, ProfileDiff *diff);

/* Actually closes the processes and stops+disables the services listed in `diff`.
 * Fills `result` with counts. Returns 0 if everything succeeded, -1 if anything failed. */
int profile_execute_diff(const ProfileDiff *diff, ProfileApplyResult *result);

#endif
