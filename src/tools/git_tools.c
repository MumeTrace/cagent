/*
 * git_tools.c
 * Phase 11B read-only Git tools. These tools use fixed Git subcommands rather
 * than accepting arbitrary shell strings, so they can remain SAFE while still
 * going through Tool Executor and result JSON handling.
 */
#include "ca_git_tools.h"

#include "ca_json.h"
#include "ca_process.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CA_GT_PATH_CAP      CA_PROJECT_PATH_CAP
#define CA_GT_TIMEOUT_MS    30000
#define CA_GT_COMMAND_CAP   (CA_PROJECT_PATH_CAP + 128)

typedef struct ca_git_diff_args {
    char path[CA_GT_PATH_CAP];
    int staged;
} ca_git_diff_args_t;

static ca_status_t ca_gt_copy(char *dest, size_t dest_size, const char *src)
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

static void ca_gt_result_reset(ca_tool_result_t *result, const char *tool_name)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    if (tool_name != NULL) {
        (void)ca_gt_copy(result->tool_name, sizeof(result->tool_name), tool_name);
    }
}

static ca_status_t ca_gt_result_error(ca_tool_result_t *result,
                                      const char *tool_name,
                                      const char *code,
                                      const char *message)
{
    if (result == NULL || code == NULL || message == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    ca_gt_result_reset(result, tool_name);
    result->success = 0;
    (void)ca_gt_copy(result->error_code, sizeof(result->error_code), code);
    (void)ca_gt_copy(result->error_message, sizeof(result->error_message), message);
    return CA_ERR_TOOL_FAILED;
}

static int ca_gt_is_absolute_path(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return 1;
    }
    if (isalpha((unsigned char)path[0]) && path[1] == ':') {
        return 1;
    }
    return 0;
}

static int ca_gt_segment_equals(const char *start, size_t len, const char *value)
{
    return value != NULL && strlen(value) == len && strncmp(start, value, len) == 0;
}

static int ca_gt_has_forbidden_segment(const char *path, const char **out_code, const char **out_message)
{
    const char *segment = path;
    const char *cursor;

    if (path == NULL) {
        return 0;
    }

    for (cursor = path; ; cursor++) {
        if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
            size_t len = (size_t)(cursor - segment);

            if (ca_gt_segment_equals(segment, len, "..")) {
                if (out_code != NULL) {
                    *out_code = "PATH_OUTSIDE_WORKSPACE";
                }
                if (out_message != NULL) {
                    *out_message = "Path must not contain '..'.";
                }
                return 1;
            }
            if (ca_gt_segment_equals(segment, len, ".git")) {
                if (out_code != NULL) {
                    *out_code = "PROTECTED_PATH";
                }
                if (out_message != NULL) {
                    *out_message = "Refusing to inspect .git internals.";
                }
                return 1;
            }
            if (ca_gt_segment_equals(segment, len, "build")) {
                if (out_code != NULL) {
                    *out_code = "PROTECTED_PATH";
                }
                if (out_message != NULL) {
                    *out_message = "Refusing to inspect build artifacts.";
                }
                return 1;
            }

            if (*cursor == '\0') {
                break;
            }
            segment = cursor + 1;
        }
    }

    return 0;
}

static ca_status_t ca_gt_normalize_path(const char *input,
                                        char *out,
                                        size_t out_size,
                                        const char **out_code,
                                        const char **out_message)
{
    const char *rel;
    const char *cursor;
    int written;

    if (input == NULL || out == NULL || out_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    out[0] = '\0';
    if (input[0] == '\0') {
        return CA_OK;
    }
    if (ca_gt_is_absolute_path(input)) {
        if (out_code != NULL) {
            *out_code = "PATH_OUTSIDE_WORKSPACE";
        }
        if (out_message != NULL) {
            *out_message = "Absolute paths are not allowed for git_diff.";
        }
        return CA_ERR_PERMISSION_DENIED;
    }
    if (ca_gt_has_forbidden_segment(input, out_code, out_message)) {
        return CA_ERR_PERMISSION_DENIED;
    }

    rel = input;
    while (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) {
        rel += 2;
    }
    if (rel[0] == '\0' || strcmp(rel, ".") == 0) {
        out[0] = '\0';
        return CA_OK;
    }
    if (ca_gt_has_forbidden_segment(rel, out_code, out_message)) {
        return CA_ERR_PERMISSION_DENIED;
    }

    /*
     * The command is a fixed Git invocation, but path still crosses a command
     * line boundary. Reject quotes/control bytes and normalize separators so
     * the path remains a plain workspace-relative Git pathspec.
     */
    for (cursor = rel; *cursor != '\0'; cursor++) {
        unsigned char ch = (unsigned char)*cursor;
        if (ch < 0x20 || *cursor == '"') {
            if (out_code != NULL) {
                *out_code = "INVALID_ARGUMENT";
            }
            if (out_message != NULL) {
                *out_message = "Path contains unsupported characters.";
            }
            return CA_ERR_INVALID_ARG;
        }
    }

    written = snprintf(out, out_size, "%s", rel);
    if (written < 0 || (size_t)written >= out_size) {
        return CA_ERR_INVALID_ARG;
    }
    for (cursor = out; *cursor != '\0'; cursor++) {
        if (*cursor == '\\') {
            *((char *)cursor) = '/';
        }
    }

    return CA_OK;
}

static ca_status_t ca_gt_write_process_json(ca_tool_result_t *result,
                                            const char *tool_name,
                                            const char *primary_key,
                                            const char *primary_value,
                                            const ca_process_result_t *process_result)
{
    char *escaped_primary;
    char *escaped_stderr;
    size_t output_escape_size;
    int written;
    ca_status_t status = CA_OK;

    if (result == NULL || tool_name == NULL || primary_key == NULL || primary_value == NULL || process_result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    output_escape_size = (size_t)CA_PROCESS_OUTPUT_CAP * 6u + 1u;
    escaped_primary = (char *)malloc(output_escape_size);
    escaped_stderr = (char *)malloc(output_escape_size);
    if (escaped_primary == NULL || escaped_stderr == NULL) {
        status = ca_gt_result_error(result, tool_name, "OUT_OF_MEMORY", "Failed to allocate Git result buffers.");
        goto cleanup;
    }

    if (ca_json_escape_string(primary_value, escaped_primary, output_escape_size) != CA_OK ||
        ca_json_escape_string(process_result->stderr_text, escaped_stderr, output_escape_size) != CA_OK) {
        status = ca_gt_result_error(result, tool_name, "RESULT_TOO_LARGE", "Git output could not be encoded as JSON.");
        goto cleanup;
    }

    ca_gt_result_reset(result, tool_name);
    result->success = 1;
    written = snprintf(result->result_json,
                       sizeof(result->result_json),
                       "{\"exit_code\":%d,\"%s\":\"%s\",\"stderr\":\"%s\",\"truncated\":%s}",
                       process_result->exit_code,
                       primary_key,
                       escaped_primary,
                       escaped_stderr,
                       (process_result->stdout_truncated || process_result->stderr_truncated) ? "true" : "false");
    if (written < 0 || (size_t)written >= sizeof(result->result_json)) {
        status = ca_gt_result_error(result, tool_name, "RESULT_TOO_LARGE", "Git result exceeded result buffer.");
        goto cleanup;
    }

cleanup:
    free(escaped_primary);
    free(escaped_stderr);
    return status;
}

static ca_status_t ca_gt_run_git_command(const ca_tool_context_t *ctx,
                                         const char *command,
                                         const char *tool_name,
                                         ca_process_result_t *process_result,
                                         ca_tool_result_t *result)
{
    ca_status_t status;

    if (ctx == NULL || ctx->workspace_root == NULL || command == NULL || process_result == NULL) {
        return ca_gt_result_error(result, tool_name, "INVALID_CONTEXT", "Tool context is missing workspace_root.");
    }

#ifndef _WIN32
    return ca_gt_result_error(result, tool_name, "UNSUPPORTED_PLATFORM", "Git process execution is not implemented for this platform yet.");
#else
    status = ca_process_run(command, ctx->workspace_root, CA_GT_TIMEOUT_MS, process_result);
    if (status != CA_OK) {
        return ca_gt_result_error(result, tool_name, "PROCESS_START_FAILED", "Failed to start or monitor git.");
    }
    return CA_OK;
#endif
}

static ca_status_t ca_gt_execute_status(const ca_tool_call_t *call,
                                        ca_tool_result_t *result,
                                        void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_process_result_t process_result;
    int porcelain = 1;
    const char *command;
    ca_status_t status;

    ca_gt_result_reset(result, "git_status");
    if (call != NULL) {
        (void)ca_json_get_bool(call->arguments_json, "porcelain", &porcelain);
    }
    command = porcelain ? "git status --short" : "git status";

    status = ca_gt_run_git_command(ctx, command, "git_status", &process_result, result);
    if (status != CA_OK) {
        return status;
    }

    /*
     * Git exit_code != 0 still means the read-only command ran. For example,
     * outside a repository Git returns 128 and stderr explains the condition.
     */
    return ca_gt_write_process_json(result, "git_status", "status", process_result.stdout_text, &process_result);
}

static ca_status_t ca_gt_parse_diff_args(const ca_tool_call_t *call,
                                         ca_git_diff_args_t *args,
                                         ca_tool_result_t *result)
{
    ca_status_t status;

    if (call == NULL || args == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(args, 0, sizeof(*args));
    status = ca_json_get_string(call->arguments_json, "path", args->path, sizeof(args->path));
    if (status == CA_ERR_NOT_FOUND) {
        args->path[0] = '\0';
        status = CA_OK;
    }
    if (status != CA_OK) {
        return ca_gt_result_error(result, "git_diff", "INVALID_ARGUMENT", "path must be a string.");
    }

    status = ca_json_get_bool(call->arguments_json, "staged", &args->staged);
    if (status == CA_ERR_NOT_FOUND) {
        args->staged = 0;
        status = CA_OK;
    }
    if (status != CA_OK) {
        return ca_gt_result_error(result, "git_diff", "INVALID_ARGUMENT", "staged must be a boolean.");
    }

    return CA_OK;
}

static ca_status_t ca_gt_preflight_diff(const ca_tool_call_t *call,
                                        ca_tool_result_t *result,
                                        void *ctx_value)
{
    ca_git_diff_args_t args;
    char rel_path[CA_GT_PATH_CAP];
    const char *code = "INVALID_ARGUMENT";
    const char *message = "Invalid git_diff path.";
    ca_status_t status;

    (void)ctx_value;

    ca_gt_result_reset(result, "git_diff");
    status = ca_gt_parse_diff_args(call, &args, result);
    if (status != CA_OK) {
        return status;
    }

    status = ca_gt_normalize_path(args.path, rel_path, sizeof(rel_path), &code, &message);
    if (status != CA_OK) {
        return ca_gt_result_error(result, "git_diff", code, message);
    }

    return CA_OK;
}

static ca_status_t ca_gt_execute_diff(const ca_tool_call_t *call,
                                      ca_tool_result_t *result,
                                      void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_git_diff_args_t args;
    ca_process_result_t process_result;
    char rel_path[CA_GT_PATH_CAP];
    char command[CA_GT_COMMAND_CAP];
    const char *code = "INVALID_ARGUMENT";
    const char *message = "Invalid git_diff path.";
    int written;
    ca_status_t status;

    ca_gt_result_reset(result, "git_diff");
    status = ca_gt_parse_diff_args(call, &args, result);
    if (status != CA_OK) {
        return status;
    }

    status = ca_gt_normalize_path(args.path, rel_path, sizeof(rel_path), &code, &message);
    if (status != CA_OK) {
        return ca_gt_result_error(result, "git_diff", code, message);
    }

    if (rel_path[0] == '\0') {
        written = snprintf(command, sizeof(command), "%s", args.staged ? "git diff --staged" : "git diff");
    } else {
        written = snprintf(command,
                           sizeof(command),
                           "%s -- \"%s\"",
                           args.staged ? "git diff --staged" : "git diff",
                           rel_path);
    }
    if (written < 0 || (size_t)written >= sizeof(command)) {
        return ca_gt_result_error(result, "git_diff", "INVALID_ARGUMENT", "git_diff path is too long.");
    }

    status = ca_gt_run_git_command(ctx, command, "git_diff", &process_result, result);
    if (status != CA_OK) {
        return status;
    }

    return ca_gt_write_process_json(result, "git_diff", "diff", process_result.stdout_text, &process_result);
}

ca_status_t ca_register_git_tools(ca_tool_registry_t *registry)
{
#ifdef CAGENT_ENABLE_GIT
    static const ca_tool_def_t git_status_tool = {
        "git_status",
        "Show short Git working tree status.",
        "{\"type\":\"object\",\"properties\":{\"porcelain\":{\"type\":\"boolean\"}}}",
        CA_TOOL_PERMISSION_SAFE,
        ca_gt_execute_status,
        NULL
    };
    static const ca_tool_def_t git_diff_tool = {
        "git_diff",
        "Show Git diff for the workspace or one file.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"staged\":{\"type\":\"boolean\"}}}",
        CA_TOOL_PERMISSION_SAFE,
        ca_gt_execute_diff,
        ca_gt_preflight_diff
    };
    ca_status_t status;

    if (registry == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_tool_registry_register(registry, &git_status_tool);
    if (status != CA_OK) {
        return status;
    }

    return ca_tool_registry_register(registry, &git_diff_tool);
#else
    (void)registry;
    return CA_OK;
#endif
}
