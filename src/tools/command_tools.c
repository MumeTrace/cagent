/*
 * command_tools.c
 * Phase 11A guarded command execution. This is an application-level safety
 * check plus Permission Manager confirmation, not a complete OS sandbox.
 */
#include "ca_command_tools.h"

#include "ca_json.h"
#include "ca_payload.h"
#include "ca_process.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CA_CT_COMMAND_CAP       4096
#define CA_CT_DEFAULT_TIMEOUT   30000
#define CA_CT_MIN_TIMEOUT       1000
#define CA_CT_MAX_TIMEOUT       120000

typedef struct ca_command_args {
    char command[CA_CT_COMMAND_CAP];
    int timeout_ms;
} ca_command_args_t;

static ca_status_t ca_ct_copy(char *dest, size_t dest_size, const char *src)
{
    int written;

    if (dest == NULL || dest_size == 0 || src == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(dest, dest_size, "%s", src);
    if (written < 0 || (size_t)written >= dest_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static void ca_ct_result_reset(ca_tool_result_t *result, const char *tool_name)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    if (tool_name != NULL) {
        (void)ca_ct_copy(result->tool_name, sizeof(result->tool_name), tool_name);
    }
}

static ca_status_t ca_ct_result_error(ca_tool_result_t *result,
                                      const char *code,
                                      const char *message)
{
    if (result == NULL || code == NULL || message == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    ca_ct_result_reset(result, "execute_command");
    result->success = 0;
    (void)ca_ct_copy(result->error_code, sizeof(result->error_code), code);
    (void)ca_ct_copy(result->error_message, sizeof(result->error_message), message);
    return CA_ERR_TOOL_FAILED;
}

static int ca_ct_is_space_only(const char *text)
{
    const unsigned char *cursor;

    if (text == NULL) {
        return 1;
    }

    for (cursor = (const unsigned char *)text; *cursor != '\0'; cursor++) {
        if (!isspace(*cursor)) {
            return 0;
        }
    }

    return 1;
}

static void ca_ct_lower_copy(char *dest, size_t dest_size, const char *src)
{
    size_t i;

    if (dest == NULL || dest_size == 0) {
        return;
    }

    dest[0] = '\0';
    if (src == NULL) {
        return;
    }

    for (i = 0; i + 1 < dest_size && src[i] != '\0'; i++) {
        dest[i] = (char)tolower((unsigned char)src[i]);
    }
    dest[i] = '\0';
}

static int ca_ct_contains_dangerous_pattern(const char *command,
                                            const char **out_code,
                                            const char **out_message)
{
    static const char *const blocked_patterns[] = {
        "rm -rf",
        "del /s",
        "rmdir /s",
        "format",
        "shutdown",
        "reboot",
        "mkfs",
        "diskpart",
        "reg delete",
        "remove-item",
        "start-process",
        "powershell -encodedcommand",
        "pwsh -encodedcommand",
        "curl ",
        "wget ",
        "invoke-webrequest",
        "invoke-restmethod",
        "scp ",
        "ssh ",
        "git push",
        "git pull",
        "git fetch",
        "git reset",
        "git clean",
        "git rebase",
        "git merge",
        "git checkout",
        "git stash",
        NULL
    };
    char lower[CA_CT_COMMAND_CAP];
    size_t i;

    if (command == NULL) {
        return 0;
    }

    ca_ct_lower_copy(lower, sizeof(lower), command);

    /*
     * MVP command policy is intentionally conservative: chained commands,
     * background operators, network download commands, and redirection are
     * rejected before permission so the user is not asked to approve something
     * this runtime cannot reason about safely yet.
     */
    if (strchr(command, '&') != NULL || strstr(command, "&&") != NULL || strstr(command, "||") != NULL ||
        strchr(command, ';') != NULL || strchr(command, '|') != NULL ||
        strchr(command, '>') != NULL || strchr(command, '<') != NULL ||
        strchr(command, '\n') != NULL || strchr(command, '\r') != NULL) {
        if (out_code != NULL) {
            *out_code = "COMMAND_REJECTED";
        }
        if (out_message != NULL) {
            *out_message = "Chained, background, pipe, redirection, or multiline commands are not allowed.";
        }
        return 1;
    }

    if (strstr(lower, "cmd /k") != NULL || strstr(lower, "powershell ") != NULL ||
        strstr(lower, "pwsh ") != NULL || strstr(lower, " pause") != NULL ||
        strstr(lower, "more ") != NULL || strstr(lower, "less ") != NULL) {
        if (out_code != NULL) {
            *out_code = "COMMAND_REJECTED";
        }
        if (out_message != NULL) {
            *out_message = "Interactive shell commands are not allowed.";
        }
        return 1;
    }

    for (i = 0; blocked_patterns[i] != NULL; i++) {
        if (strstr(lower, blocked_patterns[i]) != NULL) {
            if (out_code != NULL) {
                *out_code = "COMMAND_REJECTED";
            }
            if (out_message != NULL) {
                *out_message = "Command matches a blocked dangerous pattern.";
            }
            return 1;
        }
    }

    return 0;
}

static ca_status_t ca_ct_parse_args(const ca_tool_call_t *call,
                                    ca_command_args_t *args,
                                    ca_tool_result_t *result)
{
    ca_status_t status;

    if (call == NULL || args == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(args, 0, sizeof(*args));
    args->timeout_ms = CA_CT_DEFAULT_TIMEOUT;

    status = ca_json_get_string(call->arguments_json, "command", args->command, sizeof(args->command));
    if (status == CA_ERR_NOT_FOUND) {
        return ca_ct_result_error(result, "MISSING_ARGUMENT", "execute_command requires a command field.");
    }
    if (status != CA_OK) {
        return ca_ct_result_error(result, "INVALID_ARGUMENT", "command must be a string.");
    }
    if (ca_ct_is_space_only(args->command)) {
        return ca_ct_result_error(result, "INVALID_ARGUMENT", "command must not be empty.");
    }

    status = ca_json_get_int(call->arguments_json, "timeout_ms", &args->timeout_ms);
    if (status == CA_ERR_NOT_FOUND) {
        args->timeout_ms = CA_CT_DEFAULT_TIMEOUT;
        status = CA_OK;
    }
    if (status != CA_OK) {
        return ca_ct_result_error(result, "INVALID_ARGUMENT", "timeout_ms must be an integer.");
    }
    if (args->timeout_ms < CA_CT_MIN_TIMEOUT || args->timeout_ms > CA_CT_MAX_TIMEOUT) {
        return ca_ct_result_error(result, "INVALID_ARGUMENT", "timeout_ms is outside the allowed range.");
    }

    return CA_OK;
}

static ca_status_t ca_ct_validate_args(const ca_command_args_t *args, ca_tool_result_t *result)
{
    const char *code = "COMMAND_REJECTED";
    const char *message = "Command is rejected by policy.";

    if (args == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (ca_ct_contains_dangerous_pattern(args->command, &code, &message)) {
        return ca_ct_result_error(result, code, message);
    }

    return CA_OK;
}

static ca_status_t ca_ct_preflight_execute_command(const ca_tool_call_t *call,
                                                   ca_tool_result_t *result,
                                                   void *ctx_value)
{
    ca_command_args_t args;
    ca_status_t status;

    (void)ctx_value;

    status = ca_ct_parse_args(call, &args, result);
    if (status != CA_OK) {
        return status;
    }

    return ca_ct_validate_args(&args, result);
}

static ca_status_t ca_ct_write_result_json(ca_tool_result_t *result,
                                           const ca_command_args_t *args,
                                           const ca_process_result_t *process_result)
{
    ca_payload_t json;
    ca_status_t status;
    int stdout_truncated;
    int stderr_truncated;

    if (result == NULL || args == NULL || process_result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_payload_init(&json, CA_MAX_TOOL_RESULT_INLINE, CA_MAX_TOOL_RESULT_TOTAL);
    if (status != CA_OK) {
        return ca_ct_result_error(result, "OUT_OF_MEMORY", "Failed to initialize command result payload.");
    }

    ca_ct_result_reset(result, "execute_command");
    result->success = 1;

    status = ca_payload_append_cstr(&json, "{\"command\":\"");
    if (status == CA_OK) {
        status = ca_payload_append_json_escaped(&json, args->command, strlen(args->command));
    }
    if (status == CA_OK) {
        status = ca_payload_appendf(&json,
                                    "\",\"exit_code\":%d,\"timed_out\":%s,\"stdout\":\"",
                                    process_result->exit_code,
                                    process_result->timed_out ? "true" : "false");
    }
    if (status == CA_OK) {
        status = ca_payload_append_json_escaped(&json, process_result->stdout_text, strlen(process_result->stdout_text));
    }
    stdout_truncated = process_result->stdout_truncated || ca_payload_truncated(&json);
    if (status == CA_OK) {
        status = ca_payload_append_cstr(&json, "\",\"stderr\":\"");
    }
    if (status == CA_OK) {
        status = ca_payload_append_json_escaped(&json, process_result->stderr_text, strlen(process_result->stderr_text));
    }
    stderr_truncated = process_result->stderr_truncated || ca_payload_truncated(&json);
    if (status == CA_OK) {
        status = ca_payload_appendf(&json,
                                    "\",\"stdout_truncated\":%s,\"stderr_truncated\":%s}",
                                    stdout_truncated ? "true" : "false",
                                    stderr_truncated ? "true" : "false");
    }
    if (status != CA_OK) {
        ca_payload_free(&json);
        return ca_ct_result_error(result, "OUT_OF_MEMORY", "Failed to build command result JSON.");
    }

    if (snprintf(result->result_json, sizeof(result->result_json), "%s", ca_payload_data(&json)) < 0 ||
        ca_payload_len(&json) >= sizeof(result->result_json)) {
        ca_payload_free(&json);
        status = ca_ct_result_error(result, "RESULT_TOO_LARGE", "Command result exceeded result buffer.");
        return status;
    }

    ca_payload_free(&json);
    return CA_OK;
}

static ca_status_t ca_ct_execute_command(const ca_tool_call_t *call,
                                         ca_tool_result_t *result,
                                         void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_command_args_t args;
    ca_process_result_t process_result;
    ca_status_t status;

    ca_ct_result_reset(result, "execute_command");
    if (ctx == NULL || ctx->workspace_root == NULL) {
        return ca_ct_result_error(result, "INVALID_CONTEXT", "Tool context is missing workspace_root.");
    }

    status = ca_ct_parse_args(call, &args, result);
    if (status != CA_OK) {
        return status;
    }
    status = ca_ct_validate_args(&args, result);
    if (status != CA_OK) {
        return status;
    }

#ifndef _WIN32
    return ca_ct_result_error(result, "UNSUPPORTED_PLATFORM", "execute_command is not implemented for this platform yet.");
#else
    /*
     * A non-zero process exit code is still a successful tool execution: the
     * command ran, output was captured, and the Agent should inspect stderr or
     * stdout before deciding the next step.
     */
    status = ca_process_run(args.command, ctx->workspace_root, args.timeout_ms, &process_result);
    if (status != CA_OK) {
        return ca_ct_result_error(result, "PROCESS_START_FAILED", "Failed to start or monitor the command process.");
    }

    return ca_ct_write_result_json(result, &args, &process_result);
#endif
}

ca_status_t ca_register_command_tools(ca_tool_registry_t *registry)
{
#ifdef CAGENT_ENABLE_SHELL
    static const ca_tool_def_t execute_command_tool = {
        "execute_command",
        "Execute a non-interactive command in the workspace with timeout and captured output.",
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"command\"]}",
        CA_TOOL_PERMISSION_ASK,
        ca_ct_execute_command,
        ca_ct_preflight_execute_command
    };

    if (registry == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    return ca_tool_registry_register(registry, &execute_command_tool);
#else
    (void)registry;
    return CA_OK;
#endif
}
