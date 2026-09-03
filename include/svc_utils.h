#ifndef SVC_UTILS_H
#define SVC_UTILS_H

#include "common.h"

/* Set a service's start type: SERVICE_DISABLED, SERVICE_DEMAND_START, SERVICE_AUTO_START.
 * Returns 0 on success. */
int svc_set_start_type(const char *service_name, DWORD start_type);

/* Stop a running service. Returns 0 on success or if already stopped. */
int svc_stop(const char *service_name);

/* Start a stopped service. Returns 0 on success or if already running. */
int svc_start(const char *service_name);

/* Query current start type into *out. Returns 0 on success. */
int svc_query_start_type(const char *service_name, DWORD *out);

/* Query current run state (SERVICE_RUNNING, SERVICE_STOPPED, ...) into *out. Returns 0 on success. */
int svc_query_state(const char *service_name, DWORD *out);

/* Human readable display name for a service, or empty string if not found. */
void svc_query_display_name(const char *service_name, char *out, size_t out_size);

const char *svc_start_type_str(DWORD start_type);
const char *svc_state_str(DWORD state);

#endif
