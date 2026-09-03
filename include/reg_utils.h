#ifndef REG_UTILS_H
#define REG_UTILS_H

#include "common.h"

/* Set a REG_DWORD value, creating the key path if it does not exist. Returns 0 on success. */
int reg_set_dword(HKEY root, const char *subkey, const char *value_name, DWORD data);

/* Set a REG_SZ value, creating the key path if it does not exist. Returns 0 on success. */
int reg_set_string(HKEY root, const char *subkey, const char *value_name, const char *data);

/* Set a REG_BINARY value, creating the key path if it does not exist. Returns 0 on success. */
int reg_set_binary(HKEY root, const char *subkey, const char *value_name, const BYTE *data, DWORD len);

/* Read a REG_DWORD value. Returns 0 on success and fills *out, non-zero if missing/error. */
int reg_get_dword(HKEY root, const char *subkey, const char *value_name, DWORD *out);

/* Read a REG_SZ value into out (size out_size). Returns 0 on success, non-zero if missing/error. */
int reg_get_string(HKEY root, const char *subkey, const char *value_name, char *out, size_t out_size);

/* Convenience for is_active() checks: returns 1 if the REG_DWORD value exists and equals
 * `expected`, 0 otherwise (including if the value/key doesn't exist). */
int reg_dword_equals(HKEY root, const char *subkey, const char *value_name, DWORD expected);

/* Delete a value (used to "revert" a tweak back to default/unset). Returns 0 on success
 * or if the value did not exist. */
int reg_delete_value(HKEY root, const char *subkey, const char *value_name);

/* Iterate immediate subkey names of `subkey`. Calls cb(full_subkey_path, ctx) for each.
 * Used for per-network-adapter tweaks under Tcpip\Parameters\Interfaces. */
typedef void (*reg_subkey_cb)(const char *subkey_path, void *ctx);
void reg_for_each_subkey(HKEY root, const char *subkey, reg_subkey_cb cb, void *ctx);

#endif
