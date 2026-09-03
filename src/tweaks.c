#include "tweaks.h"
#include "reg_utils.h"
#include "svc_utils.h"
#include "task_utils.h"
#include "ps_utils.h"
#include <powrprof.h>

/* ---- Power scheme GUIDs (standard, built into every Windows install) ---- */
static const GUID GUID_POWER_HIGH_PERF =
    { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
static const GUID GUID_POWER_BALANCED =
    { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
static const GUID GUID_POWER_ULTIMATE =
    { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };

#define ULTIMATE_GUID_STR "e9a42b02-d5df-448d-aa00-03f14749eb61"
#define SUBGROUP_PROCESSOR "54533251-82be-4824-96c1-47b60b740d00"
#define SETTING_PERFBOOSTMODE "be337238-0d82-4146-a960-4f3749d470c7"
#define SUBGROUP_USB "2a737441-1930-4402-8d77-b2bebba308a3"
#define SETTING_USB_SUSPEND "48e6b7a6-50f5-4782-a5d4-53bb8f07e226"

#define NIC_CLASS_KEY "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}"
#define TCPIP_INTERFACES_KEY "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces"

/* =======================================================================
 * SHARED HELPERS
 * ===================================================================== */

static int active_scheme_is(const GUID *want) {
    GUID *active = NULL;
    if (PowerGetActiveScheme(NULL, &active) != ERROR_SUCCESS || !active) return 0;
    int match = (memcmp(active, want, sizeof(GUID)) == 0);
    LocalFree(active);
    return match;
}

/* Builds the registry path of a per-setting AC index under the *currently active* power
 * scheme, e.g. ...\PowerSchemes\{scheme}\{subgroup}\{setting}. Returns 0 on success. */
static int active_scheme_setting_key(const char *subgroup, const char *setting,
                                     char *out, size_t out_size) {
    GUID *active = NULL;
    if (PowerGetActiveScheme(NULL, &active) != ERROR_SUCCESS || !active) return -1;

    char guid_str[64];
    _snprintf(guid_str, sizeof(guid_str),
              "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              (unsigned long)active->Data1, active->Data2, active->Data3,
              active->Data4[0], active->Data4[1], active->Data4[2], active->Data4[3],
              active->Data4[4], active->Data4[5], active->Data4[6], active->Data4[7]);
    LocalFree(active);

    _snprintf(out, out_size,
              "SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes\\%s\\%s\\%s",
              guid_str, subgroup, setting);
    out[out_size - 1] = '\0';
    return 0;
}

static int active_scheme_ac_index_is(const char *subgroup, const char *setting, DWORD expected) {
    char key[512];
    if (active_scheme_setting_key(subgroup, setting, key, sizeof(key)) != 0) return 0;
    return reg_dword_equals(HKEY_LOCAL_MACHINE, key, "ACSettingIndex", expected);
}

/* Applies a value to the active scheme for both AC and DC, then re-activates it so the
 * change takes effect immediately. */
static int powercfg_set_setting(const char *subgroup, const char *setting, int value) {
    char script[512];
    _snprintf(script, sizeof(script),
              "powercfg /setacvalueindex SCHEME_CURRENT %s %s %d\r\n"
              "powercfg /setdcvalueindex SCHEME_CURRENT %s %s %d\r\n"
              "powercfg /setactive SCHEME_CURRENT\r\n",
              subgroup, setting, value, subgroup, setting, value);
    script[sizeof(script) - 1] = '\0';
    return ps_run(script);
}

/* --- Per-NIC registry iteration (used for adapter power mgmt / interrupt moderation) --- */

typedef struct {
    const char *value_name;
    const char *string_value;  /* NULL => use dword_value */
    DWORD dword_value;
    int deleting;              /* 1 => delete the value instead of setting it */
    int matched;               /* out: how many adapters had the expected value (query mode) */
    int query_mode;
    const char *expect_string;
    int failures;
} NicOp;

static void nic_subkey_cb(const char *subkey_path, void *ctx) {
    NicOp *op = (NicOp *)ctx;

    /* Only real adapter instance keys (0000, 0001, ...) have DriverDesc; skip Properties etc. */
    char driver_desc[256];
    if (reg_get_string(HKEY_LOCAL_MACHINE, subkey_path, "DriverDesc", driver_desc, sizeof(driver_desc)) != 0) {
        return;
    }

    if (op->query_mode) {
        char current[64];
        if (op->expect_string) {
            if (reg_get_string(HKEY_LOCAL_MACHINE, subkey_path, op->value_name, current, sizeof(current)) == 0 &&
                strcmp(current, op->expect_string) == 0) {
                op->matched++;
            }
        } else if (reg_dword_equals(HKEY_LOCAL_MACHINE, subkey_path, op->value_name, op->dword_value)) {
            op->matched++;
        }
        return;
    }

    int rc;
    if (op->deleting) {
        rc = reg_delete_value(HKEY_LOCAL_MACHINE, subkey_path, op->value_name);
    } else if (op->string_value) {
        rc = reg_set_string(HKEY_LOCAL_MACHINE, subkey_path, op->value_name, op->string_value);
    } else {
        rc = reg_set_dword(HKEY_LOCAL_MACHINE, subkey_path, op->value_name, op->dword_value);
    }
    if (rc != 0) op->failures++;
}

static int nic_set_dword(const char *value_name, DWORD value) {
    NicOp op;
    memset(&op, 0, sizeof(op));
    op.value_name = value_name;
    op.dword_value = value;
    reg_for_each_subkey(HKEY_LOCAL_MACHINE, NIC_CLASS_KEY, nic_subkey_cb, &op);
    return op.failures == 0 ? 0 : -1;
}

static int nic_set_string(const char *value_name, const char *value) {
    NicOp op;
    memset(&op, 0, sizeof(op));
    op.value_name = value_name;
    op.string_value = value;
    reg_for_each_subkey(HKEY_LOCAL_MACHINE, NIC_CLASS_KEY, nic_subkey_cb, &op);
    return op.failures == 0 ? 0 : -1;
}

static int nic_delete_value(const char *value_name) {
    NicOp op;
    memset(&op, 0, sizeof(op));
    op.value_name = value_name;
    op.deleting = 1;
    reg_for_each_subkey(HKEY_LOCAL_MACHINE, NIC_CLASS_KEY, nic_subkey_cb, &op);
    return op.failures == 0 ? 0 : -1;
}

static int nic_any_has_dword(const char *value_name, DWORD expected) {
    NicOp op;
    memset(&op, 0, sizeof(op));
    op.value_name = value_name;
    op.dword_value = expected;
    op.query_mode = 1;
    reg_for_each_subkey(HKEY_LOCAL_MACHINE, NIC_CLASS_KEY, nic_subkey_cb, &op);
    return op.matched > 0;
}

static int nic_any_has_string(const char *value_name, const char *expected) {
    NicOp op;
    memset(&op, 0, sizeof(op));
    op.value_name = value_name;
    op.expect_string = expected;
    op.query_mode = 1;
    reg_for_each_subkey(HKEY_LOCAL_MACHINE, NIC_CLASS_KEY, nic_subkey_cb, &op);
    return op.matched > 0;
}

/* --- Per-TCP/IP-interface registry iteration (Nagle / delayed ACK) --- */

typedef struct {
    int deleting;
    int query_mode;
    int matched;
    int failures;
} TcpOp;

static void tcp_subkey_cb(const char *subkey_path, void *ctx) {
    TcpOp *op = (TcpOp *)ctx;

    if (op->query_mode) {
        if (reg_dword_equals(HKEY_LOCAL_MACHINE, subkey_path, "TcpAckFrequency", 1) &&
            reg_dword_equals(HKEY_LOCAL_MACHINE, subkey_path, "TCPNoDelay", 1)) {
            op->matched++;
        }
        return;
    }

    int rc = 0;
    if (op->deleting) {
        rc |= reg_delete_value(HKEY_LOCAL_MACHINE, subkey_path, "TcpAckFrequency");
        rc |= reg_delete_value(HKEY_LOCAL_MACHINE, subkey_path, "TCPNoDelay");
    } else {
        rc |= reg_set_dword(HKEY_LOCAL_MACHINE, subkey_path, "TcpAckFrequency", 1);
        rc |= reg_set_dword(HKEY_LOCAL_MACHINE, subkey_path, "TCPNoDelay", 1);
    }
    if (rc != 0) op->failures++;
}

/* =======================================================================
 * PERFORMANCE TWEAKS
 * ===================================================================== */

static int apply_power_high_perf(void) {
    return (PowerSetActiveScheme(NULL, &GUID_POWER_HIGH_PERF) == ERROR_SUCCESS) ? 0 : -1;
}
static int revert_power_high_perf(void) {
    return (PowerSetActiveScheme(NULL, &GUID_POWER_BALANCED) == ERROR_SUCCESS) ? 0 : -1;
}
static int active_power_high_perf(void) {
    return active_scheme_is(&GUID_POWER_HIGH_PERF);
}

static int apply_power_ultimate(void) {
    /* Duplicating the well-known hidden Ultimate Performance scheme registers it under the
     * same GUID; it is not present out of the box on most Windows 11 SKUs. */
    int rc = ps_run("powercfg -duplicatescheme " ULTIMATE_GUID_STR " | Out-Null\r\n"
                    "powercfg /setactive " ULTIMATE_GUID_STR "\r\n");
    if (rc != 0) return -1;
    return active_scheme_is(&GUID_POWER_ULTIMATE) ? 0 : -1;
}
static int revert_power_ultimate(void) {
    return (PowerSetActiveScheme(NULL, &GUID_POWER_BALANCED) == ERROR_SUCCESS) ? 0 : -1;
}
static int active_power_ultimate(void) {
    return active_scheme_is(&GUID_POWER_ULTIMATE);
}

static int apply_boost_mode(void) {
    return powercfg_set_setting(SUBGROUP_PROCESSOR, SETTING_PERFBOOSTMODE, 2);
}
static int revert_boost_mode(void) {
    return powercfg_set_setting(SUBGROUP_PROCESSOR, SETTING_PERFBOOSTMODE, 1);
}
static int active_boost_mode(void) {
    return active_scheme_ac_index_is(SUBGROUP_PROCESSOR, SETTING_PERFBOOSTMODE, 2);
}

static int apply_usb_suspend_off(void) {
    return powercfg_set_setting(SUBGROUP_USB, SETTING_USB_SUSPEND, 0);
}
static int revert_usb_suspend_off(void) {
    return powercfg_set_setting(SUBGROUP_USB, SETTING_USB_SUSPEND, 1);
}
static int active_usb_suspend_off(void) {
    return active_scheme_ac_index_is(SUBGROUP_USB, SETTING_USB_SUSPEND, 0);
}

static int apply_hibernate_off(void) {
    return ps_run("powercfg /hibernate off\r\n");
}
static int revert_hibernate_off(void) {
    return ps_run("powercfg /hibernate on\r\n");
}
static int active_hibernate_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Power",
                            "HibernateEnabled", 0);
}

static int apply_dynamic_tick_off(void) {
    return ps_run("bcdedit /set disabledynamictick yes\r\n");
}
static int revert_dynamic_tick_off(void) {
    return ps_run("bcdedit /deletevalue disabledynamictick\r\n");
}
static int active_dynamic_tick_off(void) {
    long v = 0;
    const char *script =
        "$__result = 0\r\n"
        "$out = & bcdedit /enum '{current}' 2>$null\r\n"
        "if ($out -match 'disabledynamictick\\s+Yes') { $__result = 1 }\r\n";
    if (ps_capture_int(script, &v) != 0) return 0;
    return v == 1;
}

static int apply_nic_power_off(void) {
    /* 24 (0x18) clears both "allow the computer to turn off this device" and
     * "allow this device to wake the computer". */
    return nic_set_dword("PnPCapabilities", 24);
}
static int revert_nic_power_off(void) {
    return nic_delete_value("PnPCapabilities");
}
static int active_nic_power_off(void) {
    return nic_any_has_dword("PnPCapabilities", 24);
}

static int apply_nic_interrupt_moderation_off(void) {
    return nic_set_string("*InterruptModeration", "0");
}
static int revert_nic_interrupt_moderation_off(void) {
    return nic_set_string("*InterruptModeration", "1");
}
static int active_nic_interrupt_moderation_off(void) {
    return nic_any_has_string("*InterruptModeration", "0");
}

static int apply_nagle_off(void) {
    TcpOp op;
    memset(&op, 0, sizeof(op));
    reg_for_each_subkey(HKEY_LOCAL_MACHINE, TCPIP_INTERFACES_KEY, tcp_subkey_cb, &op);
    return op.failures == 0 ? 0 : -1;
}
static int revert_nagle_off(void) {
    TcpOp op;
    memset(&op, 0, sizeof(op));
    op.deleting = 1;
    reg_for_each_subkey(HKEY_LOCAL_MACHINE, TCPIP_INTERFACES_KEY, tcp_subkey_cb, &op);
    return op.failures == 0 ? 0 : -1;
}
static int active_nagle_off(void) {
    TcpOp op;
    memset(&op, 0, sizeof(op));
    op.query_mode = 1;
    reg_for_each_subkey(HKEY_LOCAL_MACHINE, TCPIP_INTERFACES_KEY, tcp_subkey_cb, &op);
    return op.matched > 0;
}

static int apply_game_mode(void) {
    return reg_set_dword(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\GameBar", "AutoGameModeEnabled", 1);
}
static int revert_game_mode(void) {
    return reg_delete_value(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\GameBar", "AutoGameModeEnabled");
}
static int active_game_mode(void) {
    return reg_dword_equals(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\GameBar", "AutoGameModeEnabled", 1);
}

static int apply_disable_gamedvr(void) {
    int rc = 0;
    rc |= reg_set_dword(HKEY_CURRENT_USER, "System\\GameConfigStore", "GameDVR_Enabled", 0);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", "AllowGameDVR", 0);
    return rc;
}
static int revert_disable_gamedvr(void) {
    int rc = 0;
    rc |= reg_delete_value(HKEY_CURRENT_USER, "System\\GameConfigStore", "GameDVR_Enabled");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", "AllowGameDVR");
    return rc;
}
static int active_disable_gamedvr(void) {
    return reg_dword_equals(HKEY_CURRENT_USER, "System\\GameConfigStore", "GameDVR_Enabled", 0);
}

static int apply_game_priority(void) {
    int rc = 0;
    const char *tasks_key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games";
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, tasks_key, "GPU Priority", 8);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, tasks_key, "Priority", 6);
    rc |= reg_set_string(HKEY_LOCAL_MACHINE, tasks_key, "Scheduling Category", "High");
    rc |= reg_set_string(HKEY_LOCAL_MACHINE, tasks_key, "SFIO Priority", "High");
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE,
                         "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                         "SystemResponsiveness", 0);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\PriorityControl",
                         "Win32PrioritySeparation", 38);
    return rc;
}
static int revert_game_priority(void) {
    int rc = 0;
    const char *tasks_key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games";
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, tasks_key, "GPU Priority");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, tasks_key, "Priority");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, tasks_key, "Scheduling Category");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, tasks_key, "SFIO Priority");
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE,
                         "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                         "SystemResponsiveness", 20);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\PriorityControl",
                         "Win32PrioritySeparation", 2);
    return rc;
}
static int active_game_priority(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE,
                            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                            "SystemResponsiveness", 0);
}

static int apply_hags(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", "HwSchMode", 2);
}
static int revert_hags(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", "HwSchMode", 1);
}
static int active_hags(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", "HwSchMode", 2);
}

static int apply_network_throttle(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                          "NetworkThrottlingIndex", 0xffffffff);
}
static int revert_network_throttle(void) {
    return reg_delete_value(HKEY_LOCAL_MACHINE,
                             "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                             "NetworkThrottlingIndex");
}
static int active_network_throttle(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE,
                            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                            "NetworkThrottlingIndex", 0xffffffff);
}

static int apply_visual_effects(void) {
    int rc = 0;
    static const BYTE perf_mask[8] = { 0x90, 0x12, 0x03, 0x80, 0x10, 0x00, 0x00, 0x00 };
    rc |= reg_set_binary(HKEY_CURRENT_USER, "Control Panel\\Desktop", "UserPreferencesMask", perf_mask, sizeof(perf_mask));
    rc |= reg_set_string(HKEY_CURRENT_USER, "Control Panel\\Desktop\\WindowMetrics", "MinAnimate", "0");
    rc |= reg_set_dword(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "TaskbarAnimations", 0);
    rc |= reg_set_dword(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "ListviewAlphaSelect", 0);
    rc |= reg_set_dword(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "ListviewShadow", 0);
    rc |= reg_set_dword(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\DWM", "EnableAeroPeek", 0);
    return rc;
}
static int revert_visual_effects(void) {
    int rc = 0;
    rc |= reg_delete_value(HKEY_CURRENT_USER, "Control Panel\\Desktop", "UserPreferencesMask");
    rc |= reg_delete_value(HKEY_CURRENT_USER, "Control Panel\\Desktop\\WindowMetrics", "MinAnimate");
    rc |= reg_delete_value(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "TaskbarAnimations");
    rc |= reg_delete_value(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "ListviewAlphaSelect");
    rc |= reg_delete_value(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", "ListviewShadow");
    rc |= reg_delete_value(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\DWM", "EnableAeroPeek");
    return rc;
}
static int active_visual_effects(void) {
    return reg_dword_equals(HKEY_CURRENT_USER,
                            "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                            "TaskbarAnimations", 0);
}

static int apply_disable_sysmain(void) {
    int rc = svc_set_start_type("SysMain", SERVICE_DISABLED);
    svc_stop("SysMain");
    return rc;
}
static int revert_disable_sysmain(void) {
    int rc = svc_set_start_type("SysMain", SERVICE_AUTO_START);
    svc_start("SysMain");
    return rc;
}
static int svc_is_disabled(const char *name) {
    DWORD start_type;
    if (svc_query_start_type(name, &start_type) != 0) return 0;
    return start_type == SERVICE_DISABLED;
}
static int active_disable_sysmain(void) { return svc_is_disabled("SysMain"); }

static int apply_disable_search_index(void) {
    int rc = svc_set_start_type("WSearch", SERVICE_DISABLED);
    svc_stop("WSearch");
    return rc;
}
static int revert_disable_search_index(void) {
    int rc = svc_set_start_type("WSearch", SERVICE_AUTO_START);
    svc_start("WSearch");
    return rc;
}
static int active_disable_search_index(void) { return svc_is_disabled("WSearch"); }

static int apply_background_apps(void) {
    return reg_set_dword(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications", "GlobalUserDisabled", 1);
}
static int revert_background_apps(void) {
    return reg_delete_value(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications", "GlobalUserDisabled");
}
static int active_background_apps(void) {
    return reg_dword_equals(HKEY_CURRENT_USER,
                            "Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications",
                            "GlobalUserDisabled", 1);
}

static int apply_startup_delay(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize", "StartupDelayInMSec", 0);
}
static int revert_startup_delay(void) {
    return reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize", "StartupDelayInMSec");
}
static int active_startup_delay(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE,
                            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize",
                            "StartupDelayInMSec", 0);
}

static int apply_driver_search_off(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\DriverSearching",
                          "SearchOrderConfig", 0);
}
static int revert_driver_search_off(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\DriverSearching",
                          "SearchOrderConfig", 1);
}
static int active_driver_search_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE,
                            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\DriverSearching",
                            "SearchOrderConfig", 0);
}

/* =======================================================================
 * TELEMETRY / PRIVACY TWEAKS
 * ===================================================================== */

static int apply_disable_diagtrack(void) {
    int rc = svc_set_start_type("DiagTrack", SERVICE_DISABLED);
    svc_stop("DiagTrack");
    return rc;
}
static int revert_disable_diagtrack(void) {
    int rc = svc_set_start_type("DiagTrack", SERVICE_DEMAND_START);
    svc_start("DiagTrack");
    return rc;
}
static int active_disable_diagtrack(void) { return svc_is_disabled("DiagTrack"); }

#define AUTOLOGGER_KEY \
    "SYSTEM\\CurrentControlSet\\Control\\WMI\\Autologger\\AutoLogger-Diagtrack-Listener"

static int apply_autologger_off(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE, AUTOLOGGER_KEY, "Start", 0);
}
static int revert_autologger_off(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE, AUTOLOGGER_KEY, "Start", 1);
}
static int active_autologger_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, AUTOLOGGER_KEY, "Start", 0);
}

static int apply_telemetry_level(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", "AllowTelemetry", 0);
}
static int revert_telemetry_level(void) {
    return reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", "AllowTelemetry");
}
static int active_telemetry_level(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", "AllowTelemetry", 0);
}

static int apply_advertising_id(void) {
    int rc = 0;
    rc |= reg_set_dword(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", "Enabled", 0);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\AdvertisingInfo", "DisabledByGroupPolicy", 1);
    return rc;
}
static int revert_advertising_id(void) {
    int rc = 0;
    rc |= reg_delete_value(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", "Enabled");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\AdvertisingInfo", "DisabledByGroupPolicy");
    return rc;
}
static int active_advertising_id(void) {
    return reg_dword_equals(HKEY_CURRENT_USER,
                            "Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", "Enabled", 0);
}

static int apply_cortana(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search", "AllowCortana", 0);
}
static int revert_cortana(void) {
    return reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search", "AllowCortana");
}
static int active_cortana(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search", "AllowCortana", 0);
}

#define SEARCH_HKCU_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Search"

static int apply_bing_search_off(void) {
    int rc = 0;
    rc |= reg_set_dword(HKEY_CURRENT_USER, SEARCH_HKCU_KEY, "BingSearchEnabled", 0);
    rc |= reg_set_dword(HKEY_CURRENT_USER, SEARCH_HKCU_KEY, "CortanaConsent", 0);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search",
                        "DisableWebSearch", 1);
    return rc;
}
static int revert_bing_search_off(void) {
    int rc = 0;
    rc |= reg_delete_value(HKEY_CURRENT_USER, SEARCH_HKCU_KEY, "BingSearchEnabled");
    rc |= reg_delete_value(HKEY_CURRENT_USER, SEARCH_HKCU_KEY, "CortanaConsent");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search",
                           "DisableWebSearch");
    return rc;
}
static int active_bing_search_off(void) {
    return reg_dword_equals(HKEY_CURRENT_USER, SEARCH_HKCU_KEY, "BingSearchEnabled", 0);
}

static int apply_activity_history(void) {
    int rc = 0;
    const char *key = "SOFTWARE\\Policies\\Microsoft\\Windows\\System";
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, key, "EnableActivityFeed", 0);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, key, "PublishUserActivities", 0);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, key, "UploadUserActivities", 0);
    return rc;
}
static int revert_activity_history(void) {
    int rc = 0;
    const char *key = "SOFTWARE\\Policies\\Microsoft\\Windows\\System";
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, key, "EnableActivityFeed");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, key, "PublishUserActivities");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, key, "UploadUserActivities");
    return rc;
}
static int active_activity_history(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\System",
                            "PublishUserActivities", 0);
}

static int apply_tailored_experiences(void) {
    return reg_set_dword(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Privacy", "TailoredExperiencesWithDiagnosticDataEnabled", 0);
}
static int revert_tailored_experiences(void) {
    return reg_delete_value(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Privacy", "TailoredExperiencesWithDiagnosticDataEnabled");
}
static int active_tailored_experiences(void) {
    return reg_dword_equals(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Privacy",
                            "TailoredExperiencesWithDiagnosticDataEnabled", 0);
}

static int apply_feedback_notifications(void) {
    int rc = 0;
    const char *key = "Software\\Microsoft\\Siuf\\Rules";
    rc |= reg_set_dword(HKEY_CURRENT_USER, key, "NumberOfSIUFInPeriod", 0);
    rc |= reg_set_dword(HKEY_CURRENT_USER, key, "PeriodInNanoSeconds", 0);
    return rc;
}
static int revert_feedback_notifications(void) {
    int rc = 0;
    const char *key = "Software\\Microsoft\\Siuf\\Rules";
    rc |= reg_delete_value(HKEY_CURRENT_USER, key, "NumberOfSIUFInPeriod");
    rc |= reg_delete_value(HKEY_CURRENT_USER, key, "PeriodInNanoSeconds");
    return rc;
}
static int active_feedback_notifications(void) {
    return reg_dword_equals(HKEY_CURRENT_USER, "Software\\Microsoft\\Siuf\\Rules", "NumberOfSIUFInPeriod", 0);
}

static const char *TELEMETRY_TASKS[] = {
    "\\Microsoft\\Windows\\Application Experience\\Microsoft Compatibility Appraiser",
    "\\Microsoft\\Windows\\Application Experience\\ProgramDataUpdater",
    "\\Microsoft\\Windows\\Application Experience\\StartupAppTask",
    "\\Microsoft\\Windows\\Autochk\\Proxy",
    "\\Microsoft\\Windows\\Customer Experience Improvement Program\\Consolidator",
    "\\Microsoft\\Windows\\Customer Experience Improvement Program\\UsbCeip",
    "\\Microsoft\\Windows\\DiskDiagnostic\\Microsoft-Windows-DiskDiagnosticDataCollector",
    "\\Microsoft\\Windows\\Feedback\\Siuf\\DmClient",
    "\\Microsoft\\Windows\\Feedback\\Siuf\\DmClientOnScenarioDownload",
    "\\Microsoft\\Windows\\Windows Error Reporting\\QueueReporting",
};
#define TELEMETRY_TASK_COUNT (sizeof(TELEMETRY_TASKS) / sizeof(TELEMETRY_TASKS[0]))

static int apply_telemetry_tasks(void) {
    int rc = 0;
    for (size_t i = 0; i < TELEMETRY_TASK_COUNT; i++) {
        rc |= task_set_enabled(TELEMETRY_TASKS[i], 0);
    }
    return rc;
}
static int revert_telemetry_tasks(void) {
    int rc = 0;
    for (size_t i = 0; i < TELEMETRY_TASK_COUNT; i++) {
        rc |= task_set_enabled(TELEMETRY_TASKS[i], 1);
    }
    return rc;
}
static int active_telemetry_tasks(void) {
    /* Querying all ten tasks means ten schtasks.exe launches on every state refresh, which
     * makes the UI visibly stall. The Compatibility Appraiser stands in for the group. */
    return task_is_enabled(TELEMETRY_TASKS[0]) == 0;
}

static int apply_location_tracking(void) {
    return reg_set_string(HKEY_LOCAL_MACHINE,
                           "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\location",
                           "Value", "Deny");
}
static int revert_location_tracking(void) {
    return reg_set_string(HKEY_LOCAL_MACHINE,
                           "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\location",
                           "Value", "Allow");
}
static int active_location_tracking(void) {
    char value[32];
    if (reg_get_string(HKEY_LOCAL_MACHINE,
                       "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\location",
                       "Value", value, sizeof(value)) != 0) return 0;
    return _stricmp(value, "Deny") == 0;
}

static int apply_error_reporting(void) {
    int rc = svc_set_start_type("WerSvc", SERVICE_DISABLED);
    svc_stop("WerSvc");
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting", "Disabled", 1);
    return rc;
}
static int revert_error_reporting(void) {
    int rc = svc_set_start_type("WerSvc", SERVICE_DEMAND_START);
    svc_start("WerSvc");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting", "Disabled");
    return rc;
}
static int active_error_reporting(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting", "Disabled", 1);
}

#define WINDOWSAI_POLICY_KEY "SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsAI"

static int apply_recall_off(void) {
    int rc = 0;
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, WINDOWSAI_POLICY_KEY, "DisableAIDataAnalysis", 1);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, WINDOWSAI_POLICY_KEY, "AllowRecallEnablement", 0);
    return rc;
}
static int revert_recall_off(void) {
    int rc = 0;
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, WINDOWSAI_POLICY_KEY, "DisableAIDataAnalysis");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, WINDOWSAI_POLICY_KEY, "AllowRecallEnablement");
    return rc;
}
static int active_recall_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, WINDOWSAI_POLICY_KEY, "DisableAIDataAnalysis", 1);
}

static int apply_copilot_off(void) {
    int rc = 0;
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsCopilot",
                        "TurnOffWindowsCopilot", 1);
    rc |= reg_set_dword(HKEY_CURRENT_USER, "Software\\Policies\\Microsoft\\Windows\\WindowsCopilot",
                        "TurnOffWindowsCopilot", 1);
    return rc;
}
static int revert_copilot_off(void) {
    int rc = 0;
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsCopilot",
                          "TurnOffWindowsCopilot");
    rc |= reg_delete_value(HKEY_CURRENT_USER, "Software\\Policies\\Microsoft\\Windows\\WindowsCopilot",
                          "TurnOffWindowsCopilot");
    return rc;
}
static int active_copilot_off(void) {
    return reg_dword_equals(HKEY_CURRENT_USER, "Software\\Policies\\Microsoft\\Windows\\WindowsCopilot",
                            "TurnOffWindowsCopilot", 1);
}

#define CDM_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager"

static const char *CDM_VALUES[] = {
    "SilentInstalledAppsEnabled",
    "SystemPaneSuggestionsEnabled",
    "PreInstalledAppsEnabled",
    "OemPreInstalledAppsEnabled",
    "SoftLandingEnabled",
    "RotatingLockScreenOverlayEnabled",
    "SubscribedContent-338387Enabled",
    "SubscribedContent-338388Enabled",
    "SubscribedContent-338389Enabled",
    "SubscribedContent-338393Enabled",
    "SubscribedContent-353694Enabled",
    "SubscribedContent-353696Enabled",
    "SubscribedContent-310093Enabled",
};
#define CDM_VALUE_COUNT (sizeof(CDM_VALUES) / sizeof(CDM_VALUES[0]))

static int apply_start_ads_off(void) {
    int rc = 0;
    for (size_t i = 0; i < CDM_VALUE_COUNT; i++) {
        rc |= reg_set_dword(HKEY_CURRENT_USER, CDM_KEY, CDM_VALUES[i], 0);
    }
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent",
                        "DisableWindowsConsumerFeatures", 1);
    return rc;
}
static int revert_start_ads_off(void) {
    int rc = 0;
    for (size_t i = 0; i < CDM_VALUE_COUNT; i++) {
        rc |= reg_delete_value(HKEY_CURRENT_USER, CDM_KEY, CDM_VALUES[i]);
    }
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent",
                           "DisableWindowsConsumerFeatures");
    return rc;
}
static int active_start_ads_off(void) {
    return reg_dword_equals(HKEY_CURRENT_USER, CDM_KEY, "SilentInstalledAppsEnabled", 0);
}

static int apply_spotlight_off(void) {
    int rc = 0;
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent",
                        "DisableWindowsSpotlightFeatures", 1);
    rc |= reg_set_dword(HKEY_CURRENT_USER, CDM_KEY, "RotatingLockScreenEnabled", 0);
    return rc;
}
static int revert_spotlight_off(void) {
    int rc = 0;
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent",
                           "DisableWindowsSpotlightFeatures");
    rc |= reg_delete_value(HKEY_CURRENT_USER, CDM_KEY, "RotatingLockScreenEnabled");
    return rc;
}
static int active_spotlight_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent",
                            "DisableWindowsSpotlightFeatures", 1);
}

#define DO_POLICY_KEY "SOFTWARE\\Policies\\Microsoft\\Windows\\DeliveryOptimization"

static int apply_delivery_opt_off(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE, DO_POLICY_KEY, "DODownloadMode", 0);
}
static int revert_delivery_opt_off(void) {
    return reg_delete_value(HKEY_LOCAL_MACHINE, DO_POLICY_KEY, "DODownloadMode");
}
static int active_delivery_opt_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, DO_POLICY_KEY, "DODownloadMode", 0);
}

#define INPUT_PERSONALIZATION_KEY "Software\\Microsoft\\InputPersonalization"

static int apply_inking_off(void) {
    int rc = 0;
    rc |= reg_set_dword(HKEY_CURRENT_USER, INPUT_PERSONALIZATION_KEY, "RestrictImplicitTextCollection", 1);
    rc |= reg_set_dword(HKEY_CURRENT_USER, INPUT_PERSONALIZATION_KEY, "RestrictImplicitInkCollection", 1);
    rc |= reg_set_dword(HKEY_CURRENT_USER, INPUT_PERSONALIZATION_KEY "\\TrainedDataStore",
                        "HarvestContacts", 0);
    rc |= reg_set_dword(HKEY_CURRENT_USER, "Software\\Microsoft\\Personalization\\Settings",
                        "AcceptedPrivacyPolicy", 0);
    return rc;
}
static int revert_inking_off(void) {
    int rc = 0;
    rc |= reg_delete_value(HKEY_CURRENT_USER, INPUT_PERSONALIZATION_KEY, "RestrictImplicitTextCollection");
    rc |= reg_delete_value(HKEY_CURRENT_USER, INPUT_PERSONALIZATION_KEY, "RestrictImplicitInkCollection");
    rc |= reg_delete_value(HKEY_CURRENT_USER, INPUT_PERSONALIZATION_KEY "\\TrainedDataStore", "HarvestContacts");
    rc |= reg_delete_value(HKEY_CURRENT_USER, "Software\\Microsoft\\Personalization\\Settings",
                           "AcceptedPrivacyPolicy");
    return rc;
}
static int active_inking_off(void) {
    return reg_dword_equals(HKEY_CURRENT_USER, INPUT_PERSONALIZATION_KEY, "RestrictImplicitTextCollection", 1);
}

#define SPEECH_PRIVACY_KEY "Software\\Microsoft\\Speech_OneCore\\Settings\\OnlineSpeechPrivacy"

static int apply_speech_off(void) {
    return reg_set_dword(HKEY_CURRENT_USER, SPEECH_PRIVACY_KEY, "HasAccepted", 0);
}
static int revert_speech_off(void) {
    return reg_set_dword(HKEY_CURRENT_USER, SPEECH_PRIVACY_KEY, "HasAccepted", 1);
}
static int active_speech_off(void) {
    return reg_dword_equals(HKEY_CURRENT_USER, SPEECH_PRIVACY_KEY, "HasAccepted", 0);
}

#define FINDMYDEVICE_KEY "SOFTWARE\\Policies\\Microsoft\\FindMyDevice"

static int apply_find_my_device_off(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE, FINDMYDEVICE_KEY, "AllowFindMyDevice", 0);
}
static int revert_find_my_device_off(void) {
    return reg_delete_value(HKEY_LOCAL_MACHINE, FINDMYDEVICE_KEY, "AllowFindMyDevice");
}
static int active_find_my_device_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, FINDMYDEVICE_KEY, "AllowFindMyDevice", 0);
}

static int apply_ceip_off(void) {
    int rc = 0;
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\SQMClient\\Windows",
                        "CEIPEnable", 0);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\SQMClient\\Windows",
                        "CEIPEnable", 0);
    return rc;
}
static int revert_ceip_off(void) {
    int rc = 0;
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\SQMClient\\Windows", "CEIPEnable");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\SQMClient\\Windows", "CEIPEnable");
    return rc;
}
static int active_ceip_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\SQMClient\\Windows",
                            "CEIPEnable", 0);
}

#define SYNC_POLICY_KEY "SOFTWARE\\Policies\\Microsoft\\Windows\\SettingSync"

static int apply_settings_sync_off(void) {
    int rc = 0;
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, SYNC_POLICY_KEY, "DisableSettingSync", 2);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, SYNC_POLICY_KEY, "DisableSettingSyncUserOverride", 1);
    return rc;
}
static int revert_settings_sync_off(void) {
    int rc = 0;
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, SYNC_POLICY_KEY, "DisableSettingSync");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, SYNC_POLICY_KEY, "DisableSettingSyncUserOverride");
    return rc;
}
static int active_settings_sync_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, SYNC_POLICY_KEY, "DisableSettingSync", 2);
}

#define REMOTE_ASSIST_KEY "SYSTEM\\CurrentControlSet\\Control\\Remote Assistance"

static int apply_remote_assist_off(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE, REMOTE_ASSIST_KEY, "fAllowToGetHelp", 0);
}
static int revert_remote_assist_off(void) {
    return reg_set_dword(HKEY_LOCAL_MACHINE, REMOTE_ASSIST_KEY, "fAllowToGetHelp", 1);
}
static int active_remote_assist_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, REMOTE_ASSIST_KEY, "fAllowToGetHelp", 0);
}

static int apply_app_diagnostics_off(void) {
    int rc = 0;
    rc |= reg_set_string(HKEY_CURRENT_USER,
                         "Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\appDiagnostics",
                         "Value", "Deny");
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy",
                        "LetAppsGetDiagnosticInfo", 2);
    return rc;
}
static int revert_app_diagnostics_off(void) {
    int rc = 0;
    rc |= reg_set_string(HKEY_CURRENT_USER,
                         "Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\appDiagnostics",
                         "Value", "Allow");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy",
                           "LetAppsGetDiagnosticInfo");
    return rc;
}
static int active_app_diagnostics_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy",
                            "LetAppsGetDiagnosticInfo", 2);
}

static int apply_handwriting_reports_off(void) {
    int rc = 0;
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE,
                        "SOFTWARE\\Policies\\Microsoft\\Windows\\HandwritingErrorReports",
                        "PreventHandwritingErrorReports", 1);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE,
                        "SOFTWARE\\Policies\\Microsoft\\Windows\\TabletPC",
                        "PreventHandwritingDataSharing", 1);
    return rc;
}
static int revert_handwriting_reports_off(void) {
    int rc = 0;
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE,
                           "SOFTWARE\\Policies\\Microsoft\\Windows\\HandwritingErrorReports",
                           "PreventHandwritingErrorReports");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE,
                           "SOFTWARE\\Policies\\Microsoft\\Windows\\TabletPC",
                           "PreventHandwritingDataSharing");
    return rc;
}
static int active_handwriting_reports_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE,
                            "SOFTWARE\\Policies\\Microsoft\\Windows\\HandwritingErrorReports",
                            "PreventHandwritingErrorReports", 1);
}

static int apply_cloud_clipboard_off(void) {
    int rc = 0;
    const char *key = "SOFTWARE\\Policies\\Microsoft\\Windows\\System";
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, key, "AllowCrossDeviceClipboard", 0);
    rc |= reg_set_dword(HKEY_LOCAL_MACHINE, key, "AllowClipboardHistory", 0);
    return rc;
}
static int revert_cloud_clipboard_off(void) {
    int rc = 0;
    const char *key = "SOFTWARE\\Policies\\Microsoft\\Windows\\System";
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, key, "AllowCrossDeviceClipboard");
    rc |= reg_delete_value(HKEY_LOCAL_MACHINE, key, "AllowClipboardHistory");
    return rc;
}
static int active_cloud_clipboard_off(void) {
    return reg_dword_equals(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\System",
                            "AllowCrossDeviceClipboard", 0);
}

static int apply_dmwappush_off(void) {
    int rc = svc_set_start_type("dmwappushservice", SERVICE_DISABLED);
    svc_stop("dmwappushservice");
    return rc;
}
static int revert_dmwappush_off(void) {
    int rc = svc_set_start_type("dmwappushservice", SERVICE_DEMAND_START);
    svc_start("dmwappushservice");
    return rc;
}
static int active_dmwappush_off(void) { return svc_is_disabled("dmwappushservice"); }

/* =======================================================================
 * TABLE
 * ===================================================================== */

Tweak g_tweaks[] = {
    /* --- Performance / gaming optimization --- */
    { "power_high_perf", "High Performance power plan",
      "Switches the active power plan to High Performance (revert = Balanced)",
      CAT_PERFORMANCE, 0, apply_power_high_perf, revert_power_high_perf, active_power_high_perf, 0 },
    { "power_ultimate", "Ultimate Performance power plan",
      "Unlocks and activates the hidden Ultimate Performance plan (revert = Balanced)",
      CAT_PERFORMANCE, 1, apply_power_ultimate, revert_power_ultimate, active_power_ultimate, 0 },
    { "boost_mode", "Aggressive processor boost mode",
      "Sets the CPU performance boost policy to Aggressive on the active power plan",
      CAT_PERFORMANCE, 1, apply_boost_mode, revert_boost_mode, active_boost_mode, 0 },
    { "usb_suspend_off", "Disable USB selective suspend",
      "Stops Windows from power-gating USB devices -- avoids mouse/keyboard input hitches",
      CAT_PERFORMANCE, 1, apply_usb_suspend_off, revert_usb_suspend_off, active_usb_suspend_off, 0 },
    { "game_mode", "Windows Game Mode",
      "Enables Windows' built-in Game Mode for foreground games",
      CAT_PERFORMANCE, 0, apply_game_mode, revert_game_mode, active_game_mode, 0 },
    { "disable_gamedvr", "Disable Game Bar / Game DVR capture",
      "Turns off background game recording overhead from Xbox Game Bar",
      CAT_PERFORMANCE, 1, apply_disable_gamedvr, revert_disable_gamedvr, active_disable_gamedvr, 0 },
    { "game_priority", "Boost game CPU/GPU scheduling priority",
      "Raises MMCSS priority for games and disables responsiveness throttling",
      CAT_PERFORMANCE, 1, apply_game_priority, revert_game_priority, active_game_priority, 0 },
    { "hags", "Hardware-accelerated GPU scheduling",
      "Enables HAGS (GPU-managed VRAM scheduling). Requires a reboot to take effect",
      CAT_PERFORMANCE, 1, apply_hags, revert_hags, active_hags, 0 },
    { "network_throttle", "Disable network throttling index",
      "Removes the QoS packet scheduler's throttle on non-DSCP network traffic (lower latency)",
      CAT_PERFORMANCE, 1, apply_network_throttle, revert_network_throttle, active_network_throttle, 0 },
    { "nagle_off", "Disable Nagle's algorithm / delayed ACK",
      "Sets TcpAckFrequency=1 + TCPNoDelay=1 per adapter -- lower latency for twitch shooters",
      CAT_PERFORMANCE, 1, apply_nagle_off, revert_nagle_off, active_nagle_off, 0 },
    { "nic_power_off", "Disable network adapter power saving",
      "Stops Windows from powering down NICs, which can cause packet loss spikes mid-match",
      CAT_PERFORMANCE, 1, apply_nic_power_off, revert_nic_power_off, active_nic_power_off, 0 },
    { "nic_interrupt_mod_off", "Disable NIC interrupt moderation",
      "Delivers network interrupts immediately instead of batching them (needs a reboot)",
      CAT_PERFORMANCE, 1, apply_nic_interrupt_moderation_off, revert_nic_interrupt_moderation_off,
      active_nic_interrupt_moderation_off, 0 },
    { "visual_effects", "Best-performance visual effects",
      "Disables animations/transparency effects that cost GPU/CPU cycles",
      CAT_PERFORMANCE, 0, apply_visual_effects, revert_visual_effects, active_visual_effects, 0 },
    { "disable_sysmain", "Disable SysMain (Superfetch) service",
      "Stops background app-prefetch caching; recommended on SSDs",
      CAT_PERFORMANCE, 1, apply_disable_sysmain, revert_disable_sysmain, active_disable_sysmain, 0 },
    { "disable_search_index", "Disable Windows Search indexing",
      "Stops the search indexer service from using disk/CPU in the background",
      CAT_PERFORMANCE, 1, apply_disable_search_index, revert_disable_search_index, active_disable_search_index, 0 },
    { "background_apps", "Disable UWP background apps",
      "Prevents Store apps from running/refreshing in the background",
      CAT_PERFORMANCE, 0, apply_background_apps, revert_background_apps, active_background_apps, 0 },
    { "startup_delay", "Remove startup app launch delay",
      "Removes Explorer's artificial delay before launching startup apps",
      CAT_PERFORMANCE, 1, apply_startup_delay, revert_startup_delay, active_startup_delay, 0 },
    { "hibernate_off", "Disable hibernation + Fast Startup",
      "Frees the hiberfil.sys disk space and avoids stale-driver-state issues after shutdown",
      CAT_PERFORMANCE, 1, apply_hibernate_off, revert_hibernate_off, active_hibernate_off, 0 },
    { "dynamic_tick_off", "Disable dynamic tick",
      "Keeps the kernel timer at a fixed rate for smoother frame pacing (needs a reboot)",
      CAT_PERFORMANCE, 1, apply_dynamic_tick_off, revert_dynamic_tick_off, active_dynamic_tick_off, 0 },
    { "driver_search_off", "Disable automatic driver search on new hardware",
      "Stops Windows Update from fetching drivers when devices are plugged in",
      CAT_PERFORMANCE, 1, apply_driver_search_off, revert_driver_search_off, active_driver_search_off, 0 },

    /* --- Telemetry / tracking / privacy --- */
    { "disable_diagtrack", "Disable Connected User Experiences and Telemetry",
      "Disables and stops the DiagTrack service (core telemetry uploader)",
      CAT_TELEMETRY, 1, apply_disable_diagtrack, revert_disable_diagtrack, active_disable_diagtrack, 0 },
    { "autologger_off", "Disable telemetry ETW autologger",
      "Stops the DiagTrack ETW trace session from restarting at boot even with the service off",
      CAT_TELEMETRY, 1, apply_autologger_off, revert_autologger_off, active_autologger_off, 0 },
    { "telemetry_level", "Minimize diagnostic data collection",
      "Sets AllowTelemetry=0 (Security level; capped at Basic on Home/Pro editions)",
      CAT_TELEMETRY, 1, apply_telemetry_level, revert_telemetry_level, active_telemetry_level, 0 },
    { "dmwappush_off", "Disable WAP Push message routing",
      "Stops dmwappushservice, a device-management channel that also forwards telemetry",
      CAT_TELEMETRY, 1, apply_dmwappush_off, revert_dmwappush_off, active_dmwappush_off, 0 },
    { "advertising_id", "Disable Advertising ID",
      "Stops apps from using your per-user advertising identifier",
      CAT_TELEMETRY, 1, apply_advertising_id, revert_advertising_id, active_advertising_id, 0 },
    { "cortana", "Disable Cortana",
      "Blocks Cortana via Windows Search policy",
      CAT_TELEMETRY, 1, apply_cortana, revert_cortana, active_cortana, 0 },
    { "bing_search_off", "Disable Bing / web results in Start search",
      "Keeps Start menu searches local instead of sending typed queries to Microsoft",
      CAT_TELEMETRY, 0, apply_bing_search_off, revert_bing_search_off, active_bing_search_off, 0 },
    { "recall_off", "Disable Windows Recall / AI data analysis",
      "Blocks the Recall snapshot feature and AI analysis of your screen activity",
      CAT_TELEMETRY, 1, apply_recall_off, revert_recall_off, active_recall_off, 0 },
    { "copilot_off", "Disable Windows Copilot",
      "Turns off the Copilot assistant and its data collection",
      CAT_TELEMETRY, 1, apply_copilot_off, revert_copilot_off, active_copilot_off, 0 },
    { "activity_history", "Disable Activity History / Timeline",
      "Stops collection and upload of your activity history to Microsoft",
      CAT_TELEMETRY, 1, apply_activity_history, revert_activity_history, active_activity_history, 0 },
    { "cloud_clipboard_off", "Disable clipboard history & cloud sync",
      "Stops clipboard contents from being retained or synced across devices",
      CAT_TELEMETRY, 1, apply_cloud_clipboard_off, revert_cloud_clipboard_off, active_cloud_clipboard_off, 0 },
    { "tailored_experiences", "Disable tailored experiences",
      "Stops Microsoft from using diagnostic data to personalize tips/ads",
      CAT_TELEMETRY, 0, apply_tailored_experiences, revert_tailored_experiences, active_tailored_experiences, 0 },
    { "start_ads_off", "Disable Start menu ads & app suggestions",
      "Turns off ContentDeliveryManager suggestions, silent app installs and promoted content",
      CAT_TELEMETRY, 1, apply_start_ads_off, revert_start_ads_off, active_start_ads_off, 0 },
    { "spotlight_off", "Disable Windows Spotlight",
      "Stops rotating lock-screen content fetched from Microsoft's servers",
      CAT_TELEMETRY, 1, apply_spotlight_off, revert_spotlight_off, active_spotlight_off, 0 },
    { "feedback_notifications", "Disable feedback notifications",
      "Stops Windows from periodically asking for feedback",
      CAT_TELEMETRY, 0, apply_feedback_notifications, revert_feedback_notifications, active_feedback_notifications, 0 },
    { "telemetry_tasks", "Disable telemetry Scheduled Tasks",
      "Disables CEIP, Compatibility Appraiser, WER queue, and related background tasks",
      CAT_TELEMETRY, 1, apply_telemetry_tasks, revert_telemetry_tasks, active_telemetry_tasks, 0 },
    { "ceip_off", "Disable Customer Experience Improvement Program",
      "Turns off the SQM/CEIP usage-reporting channel",
      CAT_TELEMETRY, 1, apply_ceip_off, revert_ceip_off, active_ceip_off, 0 },
    { "inking_off", "Disable inking & typing personalization",
      "Stops Windows from harvesting your typed/handwritten text to build a user dictionary",
      CAT_TELEMETRY, 0, apply_inking_off, revert_inking_off, active_inking_off, 0 },
    { "handwriting_reports_off", "Disable handwriting data sharing",
      "Blocks handwriting error reports and handwriting sample uploads",
      CAT_TELEMETRY, 1, apply_handwriting_reports_off, revert_handwriting_reports_off,
      active_handwriting_reports_off, 0 },
    { "speech_off", "Disable online speech recognition",
      "Stops voice data from being sent to Microsoft's cloud speech service",
      CAT_TELEMETRY, 0, apply_speech_off, revert_speech_off, active_speech_off, 0 },
    { "location_tracking", "Disable location tracking",
      "Sets the system-wide location consent to Deny",
      CAT_TELEMETRY, 1, apply_location_tracking, revert_location_tracking, active_location_tracking, 0 },
    { "find_my_device_off", "Disable Find My Device",
      "Stops periodic device-location reporting to your Microsoft account",
      CAT_TELEMETRY, 1, apply_find_my_device_off, revert_find_my_device_off, active_find_my_device_off, 0 },
    { "app_diagnostics_off", "Block app access to diagnostic info",
      "Denies apps the ability to read diagnostic data about other running apps",
      CAT_TELEMETRY, 1, apply_app_diagnostics_off, revert_app_diagnostics_off, active_app_diagnostics_off, 0 },
    { "settings_sync_off", "Disable settings sync to Microsoft account",
      "Stops Windows settings/themes/passwords from syncing to the cloud",
      CAT_TELEMETRY, 1, apply_settings_sync_off, revert_settings_sync_off, active_settings_sync_off, 0 },
    { "delivery_opt_off", "Disable Delivery Optimization P2P",
      "Stops your PC uploading Windows updates to other machines (saves upstream bandwidth)",
      CAT_TELEMETRY, 1, apply_delivery_opt_off, revert_delivery_opt_off, active_delivery_opt_off, 0 },
    { "remote_assist_off", "Disable Remote Assistance",
      "Closes the inbound Remote Assistance invitation channel",
      CAT_TELEMETRY, 1, apply_remote_assist_off, revert_remote_assist_off, active_remote_assist_off, 0 },
    { "error_reporting", "Disable Windows Error Reporting",
      "Stops WerSvc and disables crash report uploads to Microsoft",
      CAT_TELEMETRY, 1, apply_error_reporting, revert_error_reporting, active_error_reporting, 0 },
};

const int g_tweak_count = sizeof(g_tweaks) / sizeof(g_tweaks[0]);

void tweaks_refresh_state(void) {
    for (int i = 0; i < g_tweak_count; i++) {
        g_tweaks[i].enabled = g_tweaks[i].is_active() ? 1 : 0;
    }
}

int tweaks_toggle(int index) {
    if (index < 0 || index >= g_tweak_count) return -1;

    Tweak *t = &g_tweaks[index];
    int want_enabled = !t->enabled;
    int rc = t->enabled ? t->revert() : t->apply();

    /* Re-read actual state rather than assuming the call did what it claimed. */
    t->enabled = t->is_active() ? 1 : 0;

    if (rc != 0) return -1;
    return (t->enabled == want_enabled) ? 0 : -1;
}
