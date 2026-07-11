#include "ca_process.h"

#ifndef _WIN32

#include <string.h>

ca_status_t ca_process_run(const char *command,
                           const char *cwd,
                           int timeout_ms,
                           ca_process_result_t *result)
{
    (void)command;
    (void)cwd;
    (void)timeout_ms;

    if (result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /*
     * Phase 11A is Windows-first because the current development target is
     * PowerShell. POSIX can later use fork/exec with pipes and poll/select
     * behind this same API without changing execute_command.
     */
    memset(result, 0, sizeof(*result));
    result->exit_code = -1;
    return CA_ERR_TOOL_FAILED;
}

#endif
