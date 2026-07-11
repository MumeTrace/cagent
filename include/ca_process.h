/*
 * ca_process.h
 * Small process runner abstraction used by execute_command. Keeping this
 * behind one API lets embedded builds disable shell support without touching
 * Tool Executor or Agent Loop code.
 */
#ifndef CA_PROCESS_H
#define CA_PROCESS_H

#include <stddef.h>

#include "ca_status.h"

#define CA_PROCESS_OUTPUT_CAP 32768

typedef struct ca_process_result {
    int exit_code;
    int timed_out;
    int stdout_truncated;
    int stderr_truncated;
    char stdout_text[CA_PROCESS_OUTPUT_CAP + 1];
    char stderr_text[CA_PROCESS_OUTPUT_CAP + 1];
} ca_process_result_t;

ca_status_t ca_process_run(const char *command,
                           const char *cwd,
                           int timeout_ms,
                           ca_process_result_t *result);

#endif
