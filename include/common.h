#ifndef COMMON_H
#define COMMON_H

#define _WIN32_WINNT 0x0602
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME "GameOptimizer"
#define APP_VERSION "1.1"

int  is_running_elevated(void);
void trim_newline(char *s);

#endif
