#ifndef SVC_MONITOR_H
#define SVC_MONITOR_H

#include "common.h"

typedef struct {
    const char *service_name; /* SCM key name, e.g. "SysMain" */
    const char *note;         /* why it's commonly considered unneeded for gaming */
} KnownService;

extern const KnownService KNOWN_SERVICES[];
extern const int KNOWN_SERVICE_COUNT;

#endif
