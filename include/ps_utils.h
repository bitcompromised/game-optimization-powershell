#ifndef PS_UTILS_H
#define PS_UTILS_H

#include "common.h"

/* Runs a PowerShell script (possibly multi-line) hidden, via a temp .ps1 written to the
 * config dir and `powershell.exe -File`. Returns 0 if PowerShell launched and exited with
 * code 0, -1 otherwise (launch failure or non-zero exit). */
int ps_run(const char *script);

/* Runs `script`, which must assign the value to check to a PowerShell variable named
 * `$__result` (a trailing `Out-File` line is appended automatically). Parses the captured
 * output as an integer into *out. Returns 0 on success, -1 if the script failed to run or
 * produced no parsable output. */
int ps_capture_int(const char *script, long *out);

/* Same as ps_capture_int but captures the first non-blank line of `$__result`'s output as
 * text into out (size out_size). Returns 0 on success. */
int ps_capture_text(const char *script, char *out, size_t out_size);

#endif
