#include "reg_utils.h"

int reg_set_dword(HKEY root, const char *subkey, const char *value_name, DWORD data) {
    HKEY hkey;
    LONG rc = RegCreateKeyExA(root, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
                               KEY_WRITE, NULL, &hkey, NULL);
    if (rc != ERROR_SUCCESS) return -1;
    rc = RegSetValueExA(hkey, value_name, 0, REG_DWORD, (const BYTE *)&data, sizeof(data));
    RegCloseKey(hkey);
    return (rc == ERROR_SUCCESS) ? 0 : -1;
}

int reg_set_string(HKEY root, const char *subkey, const char *value_name, const char *data) {
    HKEY hkey;
    LONG rc = RegCreateKeyExA(root, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
                               KEY_WRITE, NULL, &hkey, NULL);
    if (rc != ERROR_SUCCESS) return -1;
    rc = RegSetValueExA(hkey, value_name, 0, REG_SZ, (const BYTE *)data, (DWORD)strlen(data) + 1);
    RegCloseKey(hkey);
    return (rc == ERROR_SUCCESS) ? 0 : -1;
}

int reg_set_binary(HKEY root, const char *subkey, const char *value_name, const BYTE *data, DWORD len) {
    HKEY hkey;
    LONG rc = RegCreateKeyExA(root, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
                               KEY_WRITE, NULL, &hkey, NULL);
    if (rc != ERROR_SUCCESS) return -1;
    rc = RegSetValueExA(hkey, value_name, 0, REG_BINARY, data, len);
    RegCloseKey(hkey);
    return (rc == ERROR_SUCCESS) ? 0 : -1;
}

int reg_get_dword(HKEY root, const char *subkey, const char *value_name, DWORD *out) {
    HKEY hkey;
    LONG rc = RegOpenKeyExA(root, subkey, 0, KEY_READ, &hkey);
    if (rc != ERROR_SUCCESS) return -1;
    DWORD data = 0;
    DWORD size = sizeof(data);
    DWORD type = 0;
    rc = RegQueryValueExA(hkey, value_name, NULL, &type, (BYTE *)&data, &size);
    RegCloseKey(hkey);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return -1;
    *out = data;
    return 0;
}

int reg_get_string(HKEY root, const char *subkey, const char *value_name, char *out, size_t out_size) {
    out[0] = '\0';
    HKEY hkey;
    LONG rc = RegOpenKeyExA(root, subkey, 0, KEY_READ, &hkey);
    if (rc != ERROR_SUCCESS) return -1;
    DWORD size = (DWORD)out_size;
    DWORD type = 0;
    rc = RegQueryValueExA(hkey, value_name, NULL, &type, (BYTE *)out, &size);
    RegCloseKey(hkey);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return -1;
    out[out_size - 1] = '\0';
    return 0;
}

int reg_dword_equals(HKEY root, const char *subkey, const char *value_name, DWORD expected) {
    DWORD v;
    if (reg_get_dword(root, subkey, value_name, &v) != 0) return 0;
    return v == expected;
}

int reg_delete_value(HKEY root, const char *subkey, const char *value_name) {
    HKEY hkey;
    LONG rc = RegOpenKeyExA(root, subkey, 0, KEY_WRITE, &hkey);
    if (rc != ERROR_SUCCESS) {
        /* Key doesn't exist at all -- nothing to revert, treat as success. */
        return (rc == ERROR_FILE_NOT_FOUND) ? 0 : -1;
    }
    rc = RegDeleteValueA(hkey, value_name);
    RegCloseKey(hkey);
    if (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) return 0;
    return -1;
}

void reg_for_each_subkey(HKEY root, const char *subkey, reg_subkey_cb cb, void *ctx) {
    HKEY hkey;
    if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &hkey) != ERROR_SUCCESS) return;

    DWORD index = 0;
    char name[256];
    DWORD name_len;

    for (;;) {
        name_len = sizeof(name);
        LONG rc = RegEnumKeyExA(hkey, index, name, &name_len, NULL, NULL, NULL, NULL);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) break;

        char full_path[512];
        _snprintf(full_path, sizeof(full_path), "%s\\%s", subkey, name);
        full_path[sizeof(full_path) - 1] = '\0';
        cb(full_path, ctx);

        index++;
    }
    RegCloseKey(hkey);
}
