#ifndef COMMON_H
#define COMMON_H

/* Windows 10+ headers: per-monitor DPI APIs (GetDpiForWindow, AdjustWindowRectExForDpi) and
 * the DWM title-bar attributes the UI uses are only declared from 0x0A00 up. */
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME "GameOptimizer"
#define APP_VERSION "2.0"

int  is_running_elevated(void);
void trim_newline(char *s);

#endif
