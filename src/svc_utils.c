#include "svc_utils.h"

static SC_HANDLE open_scm(DWORD access) {
    return OpenSCManagerA(NULL, NULL, access);
}

int svc_set_start_type(const char *service_name, DWORD start_type) {
    SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT);
    if (!scm) return -1;

    SC_HANDLE svc = OpenServiceA(scm, service_name, SERVICE_CHANGE_CONFIG);
    if (!svc) {
        CloseServiceHandle(scm);
        return -1;
    }

    BOOL ok = ChangeServiceConfigA(svc, SERVICE_NO_CHANGE, start_type, SERVICE_NO_CHANGE,
                                    NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? 0 : -1;
}

int svc_stop(const char *service_name) {
    SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT);
    if (!scm) return -1;

    SC_HANDLE svc = OpenServiceA(scm, service_name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return -1;
    }

    SERVICE_STATUS status;
    if (!QueryServiceStatus(svc, &status)) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return -1;
    }

    int result = 0;
    if (status.dwCurrentState != SERVICE_STOPPED) {
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
            result = -1;
        } else {
            /* Wait briefly for the stop to take effect. */
            for (int i = 0; i < 20 && status.dwCurrentState != SERVICE_STOPPED; i++) {
                Sleep(200);
                if (!QueryServiceStatus(svc, &status)) break;
            }
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result;
}

int svc_start(const char *service_name) {
    SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT);
    if (!scm) return -1;

    SC_HANDLE svc = OpenServiceA(scm, service_name, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return -1;
    }

    SERVICE_STATUS status;
    int result = 0;
    if (QueryServiceStatus(svc, &status) && status.dwCurrentState != SERVICE_RUNNING) {
        if (!StartServiceA(svc, 0, NULL)) {
            /* ERROR_SERVICE_DISABLED etc. still counts as failure to start. */
            result = -1;
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result;
}

int svc_query_start_type(const char *service_name, DWORD *out) {
    SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT);
    if (!scm) return -1;

    SC_HANDLE svc = OpenServiceA(scm, service_name, SERVICE_QUERY_CONFIG);
    if (!svc) {
        CloseServiceHandle(scm);
        return -1;
    }

    BYTE buffer[8192];
    DWORD needed = 0;
    QUERY_SERVICE_CONFIGA *cfg = (QUERY_SERVICE_CONFIGA *)buffer;
    int result = -1;
    if (QueryServiceConfigA(svc, cfg, sizeof(buffer), &needed)) {
        *out = cfg->dwStartType;
        result = 0;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result;
}

int svc_query_state(const char *service_name, DWORD *out) {
    SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT);
    if (!scm) return -1;

    SC_HANDLE svc = OpenServiceA(scm, service_name, SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return -1;
    }

    SERVICE_STATUS status;
    int result = -1;
    if (QueryServiceStatus(svc, &status)) {
        *out = status.dwCurrentState;
        result = 0;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result;
}

void svc_query_display_name(const char *service_name, char *out, size_t out_size) {
    out[0] = '\0';
    SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT);
    if (!scm) return;

    SC_HANDLE svc = OpenServiceA(scm, service_name, SERVICE_QUERY_CONFIG);
    if (svc) {
        BYTE buffer[8192];
        DWORD needed = 0;
        QUERY_SERVICE_CONFIGA *cfg = (QUERY_SERVICE_CONFIGA *)buffer;
        if (QueryServiceConfigA(svc, cfg, sizeof(buffer), &needed) && cfg->lpDisplayName) {
            strncpy(out, cfg->lpDisplayName, out_size - 1);
            out[out_size - 1] = '\0';
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

const char *svc_start_type_str(DWORD start_type) {
    switch (start_type) {
        case SERVICE_BOOT_START:   return "Boot";
        case SERVICE_SYSTEM_START: return "System";
        case SERVICE_AUTO_START:   return "Automatic";
        case SERVICE_DEMAND_START: return "Manual";
        case SERVICE_DISABLED:     return "Disabled";
        default:                   return "Unknown";
    }
}

const char *svc_state_str(DWORD state) {
    switch (state) {
        case SERVICE_RUNNING:         return "Running";
        case SERVICE_STOPPED:         return "Stopped";
        case SERVICE_PAUSED:          return "Paused";
        case SERVICE_START_PENDING:   return "Starting";
        case SERVICE_STOP_PENDING:    return "Stopping";
        default:                      return "Unknown";
    }
}
