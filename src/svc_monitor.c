#include "svc_monitor.h"

const KnownService KNOWN_SERVICES[] = {
    { "SysMain",           "Superfetch/prefetch caching -- usually safe to disable on SSDs" },
    { "WSearch",           "Windows Search indexer -- background disk/CPU use" },
    { "DiagTrack",         "Connected User Experiences and Telemetry -- sends usage data" },
    { "dmwappushservice",  "WAP Push message routing -- rarely used outside mobile/enterprise" },
    { "MapsBroker",        "Downloaded Maps Manager -- only needed for offline Maps app" },
    { "RetailDemo",        "Retail Demo Service -- only used on store display units" },
    { "WerSvc",            "Windows Error Reporting -- uploads crash reports to Microsoft" },
    { "RemoteRegistry",    "Allows remote registry access -- disabled by default, safe off" },
    { "Fax",               "Fax service -- unneeded unless you use fax hardware" },
    { "WMPNetworkSvc",     "Windows Media Player Network Sharing -- only for media libraries" },
    { "PhoneSvc",          "Phone Service -- only needed for cellular/phone link features" },
    { "TabletInputService","Touch keyboard/handwriting panel -- unneeded without a touchscreen" },
    { "WalletService",     "Wallet Service -- rarely used, mostly legacy" },
    { "XblAuthManager",    "Xbox Live Auth Manager -- only needed for Xbox/Game Pass sign-in" },
    { "XblGameSave",       "Xbox Live Game Save -- only needed for Xbox cloud saves" },
    { "XboxNetApiSvc",     "Xbox Live Networking -- only needed for Xbox multiplayer features" },
};

const int KNOWN_SERVICE_COUNT = sizeof(KNOWN_SERVICES) / sizeof(KNOWN_SERVICES[0]);
