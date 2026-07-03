/*
 * ca_platform.h
 * Small platform setup helpers. The rest of cagent uses narrow UTF-8 strings;
 * Windows-specific console and argv conversion stays behind this boundary.
 */
#ifndef CA_PLATFORM_H
#define CA_PLATFORM_H

#include "ca_status.h"

ca_status_t ca_console_init(void);
ca_status_t ca_platform_argv_to_utf8(int argc, char **argv, int *out_argc, char ***out_argv);
void ca_platform_argv_free(int argc, char **argv);

#endif
