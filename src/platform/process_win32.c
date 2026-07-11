#include "ca_process.h"

#ifdef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static void ca_process_result_reset(ca_process_result_t *result)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->exit_code = -1;
}

static void ca_process_close_handle(HANDLE *handle)
{
    if (handle != NULL && *handle != NULL && *handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
        *handle = NULL;
    }
}

static void ca_process_append_output(char *dest,
                                     size_t dest_size,
                                     size_t *used,
                                     int *truncated,
                                     const char *src,
                                     size_t src_len)
{
    size_t available;
    size_t to_copy;

    if (dest == NULL || dest_size == 0 || used == NULL || truncated == NULL || src == NULL || src_len == 0) {
        return;
    }

    if (*used >= dest_size - 1) {
        *truncated = 1;
        return;
    }

    available = dest_size - 1 - *used;
    to_copy = src_len < available ? src_len : available;
    memcpy(dest + *used, src, to_copy);
    *used += to_copy;
    dest[*used] = '\0';
    if (to_copy < src_len) {
        *truncated = 1;
    }
}

static void ca_process_drain_pipe(HANDLE pipe,
                                  char *dest,
                                  size_t dest_size,
                                  size_t *used,
                                  int *truncated)
{
    char buffer[4096];
    DWORD available = 0;
    DWORD read_bytes = 0;

    if (pipe == NULL || pipe == INVALID_HANDLE_VALUE) {
        return;
    }

    while (PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL) && available > 0) {
        DWORD want = available < sizeof(buffer) ? available : (DWORD)sizeof(buffer);
        if (!ReadFile(pipe, buffer, want, &read_bytes, NULL) || read_bytes == 0) {
            break;
        }
        ca_process_append_output(dest, dest_size, used, truncated, buffer, (size_t)read_bytes);
    }
}

ca_status_t ca_process_run(const char *command,
                           const char *cwd,
                           int timeout_ms,
                           ca_process_result_t *result)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE stdout_read = NULL;
    HANDLE stdout_write = NULL;
    HANDLE stderr_read = NULL;
    HANDLE stderr_write = NULL;
    HANDLE stdin_read = NULL;
    HANDLE stdin_write = NULL;
    char *command_line = NULL;
    size_t stdout_used = 0;
    size_t stderr_used = 0;
    ULONGLONG started;
    ca_status_t status = CA_OK;

    if (command == NULL || command[0] == '\0' || cwd == NULL || result == NULL || timeout_ms <= 0) {
        return CA_ERR_INVALID_ARG;
    }

    ca_process_result_reset(result);
    command_line = (char *)malloc(strlen(command) + 1);
    if (command_line == NULL) {
        return CA_ERR_NO_MEMORY;
    }
    memcpy(command_line, command, strlen(command) + 1);

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0) ||
        !CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        status = CA_ERR_IO;
        goto cleanup;
    }
    (void)SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    (void)SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    (void)SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    si.hStdInput = stdin_read;

    /*
     * execute_command is deliberately non-interactive. The child's stdin pipe
     * is closed from the parent immediately after start, so commands waiting
     * for input see EOF instead of borrowing the user's terminal.
     */
    if (!CreateProcessA(NULL,
                        command_line,
                        NULL,
                        NULL,
                        TRUE,
                        CREATE_NO_WINDOW,
                        NULL,
                        cwd,
                        &si,
                        &pi)) {
        status = CA_ERR_IO;
        goto cleanup;
    }

    ca_process_close_handle(&stdout_write);
    ca_process_close_handle(&stderr_write);
    ca_process_close_handle(&stdin_read);
    ca_process_close_handle(&stdin_write);

    started = GetTickCount64();
    for (;;) {
        DWORD wait_status;

        ca_process_drain_pipe(stdout_read,
                              result->stdout_text,
                              sizeof(result->stdout_text),
                              &stdout_used,
                              &result->stdout_truncated);
        ca_process_drain_pipe(stderr_read,
                              result->stderr_text,
                              sizeof(result->stderr_text),
                              &stderr_used,
                              &result->stderr_truncated);

        wait_status = WaitForSingleObject(pi.hProcess, 50);
        if (wait_status == WAIT_OBJECT_0) {
            DWORD exit_code = 0;
            ca_process_drain_pipe(stdout_read,
                                  result->stdout_text,
                                  sizeof(result->stdout_text),
                                  &stdout_used,
                                  &result->stdout_truncated);
            ca_process_drain_pipe(stderr_read,
                                  result->stderr_text,
                                  sizeof(result->stderr_text),
                                  &stderr_used,
                                  &result->stderr_truncated);
            if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
                result->exit_code = (int)exit_code;
            }
            break;
        }

        if (wait_status == WAIT_FAILED) {
            status = CA_ERR_IO;
            break;
        }

        if (GetTickCount64() - started >= (ULONGLONG)timeout_ms) {
            /*
             * Timeout is a command outcome, not a tool crash. The tool layer
             * returns success=true with timed_out=true so the Agent can inspect
             * partial output and decide what to do next.
             */
            result->timed_out = 1;
            result->exit_code = -1;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            ca_process_drain_pipe(stdout_read,
                                  result->stdout_text,
                                  sizeof(result->stdout_text),
                                  &stdout_used,
                                  &result->stdout_truncated);
            ca_process_drain_pipe(stderr_read,
                                  result->stderr_text,
                                  sizeof(result->stderr_text),
                                  &stderr_used,
                                  &result->stderr_truncated);
            break;
        }
    }

cleanup:
    ca_process_close_handle(&stdout_read);
    ca_process_close_handle(&stdout_write);
    ca_process_close_handle(&stderr_read);
    ca_process_close_handle(&stderr_write);
    ca_process_close_handle(&stdin_read);
    ca_process_close_handle(&stdin_write);
    if (pi.hThread != NULL) {
        CloseHandle(pi.hThread);
    }
    if (pi.hProcess != NULL) {
        CloseHandle(pi.hProcess);
    }
    free(command_line);
    return status;
}

#endif
