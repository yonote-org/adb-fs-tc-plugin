#pragma once
// Platform layer: on Unix the plugin ABI comes from Double Commander's SDK
// headers (sdk/). Include this file, never sdk/wfxplugin.h directly (no guard).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>

extern "C" {
#include "sdk/wfxplugin.h"
}

#define DCEXPORT __attribute__((visibility("default")))

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket ::close
typedef struct timeval TIMEVAL;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)(-1))

inline void Sleep(unsigned int ms) { usleep(ms * 1000); }
#define ZeroMemory(p, s) memset((p), 0, (s))

#ifndef countof
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define wdirtypemax 1024

#include <string>
#include <list>
#include <map>
#include <vector>
