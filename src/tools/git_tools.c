/*
 * git_tools.c
 * Local Git tools with fixed subcommands. Read-only tools are SAFE; tools that
 * mutate the index/history/working tree are ASK or DANGEROUS and still go
 * through Tool Executor and Permission Manager.
 */
#include "ca_git_tools.h"

#include "ca_json.h"
#include "ca_process.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CA_GT_TIMEOUT_MS       30000
#define CA_GT_MAX_PATHS        8
#define CA_GT_COMMAND_CAP      32768
#define CA_GT_REF_CAP          128
#define CA_GT_BRANCH_CAP       128
#define CA_GT_MESSAGE_CAP      201
#define CA_GT_KEY_CAP          64
#define CA_GT_PATHS_JSON_CAP   (CA_GT_MAX_PATHS * CA_PROJECT_PATH_CAP * 2u + 64u)

typedef struct ca_git_paths {
    char items[CA_GT_MAX_PATHS][CA_PROJECT_PATH_CAP];
    size_t count;
} ca_git_paths_t;

typedef struct ca_git_diff_args {
    char path[CA_PROJECT_PATH_CAP];
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

static const char *ca_gt_skip_ws(const char *cursor)
{
    while (cursor != NULL && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static ca_status_t ca_gt_decode_json_string(const char **cursor_io, char *out, size_t out_size)
{
    const char *cursor;
    size_t used = 0;

    if (cursor_io == NULL || *cursor_io == NULL || out == NULL || out_size == 0 || **cursor_io != '"') {
        return CA_ERR_INVALID_ARG;
    }

    cursor = *cursor_io + 1;
    out[0] = '\0';
    while (*cursor != '\0') {
        unsigned char ch = (unsigned char)*cursor;

        if (ch == '"') {
            out[used] = '\0';
            *cursor_io = cursor + 1;
            return CA_OK;
        }

        if (ch == '\\') {
            cursor++;
            ch = (unsigned char)*cursor;
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            default:
                return CA_ERR_JSON;
            }
        }

        if (used + 1 >= out_size) {
            return CA_ERR_INVALID_ARG;
        }
        out[used++] = (char)ch;
        cursor++;
    }

    return CA_ERR_JSON;
}

static ca_status_t ca_gt_find_value(const char *json, const char *key, const char **out_value)
{
    const char *cursor;
    char parsed_key[CA_GT_KEY_CAP];

    if (json == NULL || key == NULL || out_value == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    cursor = json;
    while (*cursor != '\0') {
        const char *after_string;

        if (*cursor != '"') {
            cursor++;
            continue;
        }

        after_string = cursor;
        if (ca_gt_decode_json_string(&after_string, parsed_key, sizeof(parsed_key)) != CA_OK) {
            return CA_ERR_JSON;
        }
        after_string = ca_gt_skip_ws(after_string);
        if (*after_string == ':' && strcmp(parsed_key, key) == 0) {
            *out_value = ca_gt_skip_ws(after_string + 1);
            return CA_OK;
        }
        cursor = after_string;
    }

    return CA_ERR_NOT_FOUND;
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
                    *out_message = "Refusing to access .git internals.";
                }
                return 1;
            }
            if (ca_gt_segment_equals(segment, len, "build")) {
                if (out_code != NULL) {
                    *out_code = "PROTECTED_PATH";
                }
                if (out_message != NULL) {
                    *out_message = "Refusing to access build artifacts.";
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
                                        int allow_empty,
                                        char *out,
                                        size_t out_size,
                                        const char **out_code,
                                        const char **out_message)
{
    const char *rel;
    char *cursor;
    int written;

    if (input == NULL || out == NULL || out_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    out[0] = '\0';
    if (input[0] == '\0') {
        if (allow_empty) {
            return CA_OK;
        }
        if (out_code != NULL) {
            *out_code = "MISSING_ARGUMENT";
        }
        if (out_message != NULL) {
            *out_message = "Path list must not contain empty paths.";
        }
        return CA_ERR_INVALID_ARG;
    }
    if (ca_gt_is_absolute_path(input)) {
        if (out_code != NULL) {
            *out_code = "PATH_OUTSIDE_WORKSPACE";
        }
        if (out_message != NULL) {
            *out_message = "Absolute paths are not allowed.";
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
        if (allow_empty) {
            return CA_OK;
        }
        if (out_code != NULL) {
            *out_code = "INVALID_ARGUMENT";
        }
        if (out_message != NULL) {
            *out_message = "Whole-repository pathspecs are not allowed.";
        }
        return CA_ERR_INVALID_ARG;
    }
    if (ca_gt_has_forbidden_segment(rel, out_code, out_message)) {
        return CA_ERR_PERMISSION_DENIED;
    }

    for (cursor = (char *)rel; *cursor != '\0'; cursor++) {
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
            *cursor = '/';
        }
    }

    return CA_OK;
}

static int ca_gt_ref_is_valid(const char *ref)
{
    const unsigned char *cursor;

    if (ref == NULL || ref[0] == '\0' || strlen(ref) >= CA_GT_REF_CAP) {
        return 0;
    }
    if (ref[0] == '-') {
        return 0;
    }
    for (cursor = (const unsigned char *)ref; *cursor != '\0'; cursor++) {
        if (*cursor <= 0x20 || *cursor == '"' || *cursor == '\'' || *cursor == ';' ||
            *cursor == '|' || *cursor == '>' || *cursor == '<' || *cursor == '&' ||
            *cursor == '\\') {
            return 0;
        }
    }
    return 1;
}

static int ca_gt_branch_is_valid(const char *name)
{
    const unsigned char *cursor;

    if (name == NULL || name[0] == '\0' || strlen(name) >= CA_GT_BRANCH_CAP || name[0] == '-') {
        return 0;
    }
    if (strstr(name, "..") != NULL || strstr(name, "@{") != NULL) {
        return 0;
    }
    for (cursor = (const unsigned char *)name; *cursor != '\0'; cursor++) {
        if (*cursor <= 0x20 || *cursor == '"' || *cursor == '\'' || *cursor == ';' ||
            *cursor == '|' || *cursor == '>' || *cursor == '<' || *cursor == '&' ||
            *cursor == '\\') {
            return 0;
        }
    }
    return 1;
}

static int ca_gt_message_is_valid(const char *message)
{
    const unsigned char *cursor;

    if (message == NULL || message[0] == '\0' || strlen(message) >= CA_GT_MESSAGE_CAP) {
        return 0;
    }
    for (cursor = (const unsigned char *)message; *cursor != '\0'; cursor++) {
        if (*cursor < 0x20 || *cursor == '"') {
            return 0;
        }
    }
    return 1;
}

static ca_status_t ca_gt_parse_paths(const ca_tool_call_t *call,
                                     const char *tool_name,
                                     ca_git_paths_t *paths,
                                     ca_tool_result_t *result)
{
    const char *value;
    const char *cursor;
    ca_status_t status;

    if (call == NULL || tool_name == NULL || paths == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(paths, 0, sizeof(*paths));
    status = ca_gt_find_value(call->arguments_json, "paths", &value);
    if (status == CA_ERR_NOT_FOUND) {
        return ca_gt_result_error(result, tool_name, "MISSING_ARGUMENT", "Tool requires a non-empty paths array.");
    }
    if (status != CA_OK) {
        return ca_gt_result_error(result, tool_name, "INVALID_ARGUMENT", "Could not parse paths array.");
    }

    cursor = ca_gt_skip_ws(value);
    if (*cursor != '[') {
        return ca_gt_result_error(result, tool_name, "INVALID_ARGUMENT", "paths must be an array of strings.");
    }
    cursor = ca_gt_skip_ws(cursor + 1);
    if (*cursor == ']') {
        return ca_gt_result_error(result, tool_name, "MISSING_ARGUMENT", "paths array must not be empty.");
    }

    while (*cursor != '\0') {
        char raw_path[CA_PROJECT_PATH_CAP];
        const char *code = "INVALID_ARGUMENT";
        const char *message = "Invalid path.";

        if (paths->count >= CA_GT_MAX_PATHS) {
            return ca_gt_result_error(result, tool_name, "INVALID_ARGUMENT", "Too many paths.");
        }
        if (*cursor != '"') {
            return ca_gt_result_error(result, tool_name, "INVALID_ARGUMENT", "paths must contain strings only.");
        }
        status = ca_gt_decode_json_string(&cursor, raw_path, sizeof(raw_path));
        if (status != CA_OK) {
            return ca_gt_result_error(result, tool_name, "INVALID_ARGUMENT", "Could not decode path string.");
        }
        status = ca_gt_normalize_path(raw_path,
                                      0,
                                      paths->items[paths->count],
                                      sizeof(paths->items[paths->count]),
                                      &code,
                                      &message);
        if (status != CA_OK) {
            return ca_gt_result_error(result, tool_name, code, message);
        }
        paths->count++;

        cursor = ca_gt_skip_ws(cursor);
        if (*cursor == ',') {
            cursor = ca_gt_skip_ws(cursor + 1);
            continue;
        }
        if (*cursor == ']') {
            cursor++;
            break;
        }
        return ca_gt_result_error(result, tool_name, "INVALID_ARGUMENT", "Malformed paths array.");
    }

    cursor = ca_gt_skip_ws(cursor);
    if (*cursor != ',' && *cursor != '}' && *cursor != '\0') {
        return ca_gt_result_error(result, tool_name, "INVALID_ARGUMENT", "Malformed paths array.");
    }

    return paths->count > 0 ? CA_OK : ca_gt_result_error(result, tool_name, "MISSING_ARGUMENT", "paths array must not be empty.");
}

static ca_status_t ca_gt_append_quoted(char *command, size_t command_size, size_t *used, const char *value)
{
    int written;

    if (command == NULL || command_size == 0 || used == NULL || value == NULL || *used >= command_size) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(command + *used, command_size - *used, " \"%s\"", value);
    if (written < 0 || (size_t)written >= command_size - *used) {
        return CA_ERR_INVALID_ARG;
    }

    *used += (size_t)written;
    return CA_OK;
}

static ca_status_t ca_gt_build_paths_command(const char *prefix,
                                             const ca_git_paths_t *paths,
                                             char *command,
                                             size_t command_size)
{
    size_t used;
    size_t i;
    int written;

    if (prefix == NULL || paths == NULL || command == NULL || command_size == 0 || paths->count == 0) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(command, command_size, "%s --", prefix);
    if (written < 0 || (size_t)written >= command_size) {
        return CA_ERR_INVALID_ARG;
    }
    used = (size_t)written;

    for (i = 0; i < paths->count; i++) {
        if (ca_gt_append_quoted(command, command_size, &used, paths->items[i]) != CA_OK) {
            return CA_ERR_INVALID_ARG;
        }
    }

    return CA_OK;
}

static ca_status_t ca_gt_paths_json(const ca_git_paths_t *paths, char *out, size_t out_size)
{
    size_t used = 0;
    size_t i;
    int written;

    if (paths == NULL || out == NULL || out_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    out[0] = '\0';
    written = snprintf(out, out_size, "[");
    if (written < 0 || (size_t)written >= out_size) {
        return CA_ERR_INVALID_ARG;
    }
    used = (size_t)written;

    for (i = 0; i < paths->count; i++) {
        char escaped[CA_PROJECT_PATH_CAP * 2u];

        if (ca_json_escape_string(paths->items[i], escaped, sizeof(escaped)) != CA_OK) {
            return CA_ERR_INVALID_ARG;
        }
        written = snprintf(out + used, out_size - used, "%s\"%s\"", i == 0 ? "" : ",", escaped);
        if (written < 0 || (size_t)written >= out_size - used) {
            return CA_ERR_INVALID_ARG;
        }
        used += (size_t)written;
    }

    written = snprintf(out + used, out_size - used, "]");
    if (written < 0 || (size_t)written >= out_size - used) {
        return CA_ERR_INVALID_ARG;
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

static ca_status_t ca_gt_write_stdout_json(ca_tool_result_t *result,
                                           const char *tool_name,
                                           const char *stdout_key,
                                           const ca_process_result_t *process_result)
{
    return ca_gt_write_process_json(result, tool_name, stdout_key, process_result->stdout_text, process_result);
}

static ca_status_t ca_gt_write_paths_result(ca_tool_result_t *result,
                                            const char *tool_name,
                                            const ca_git_paths_t *paths,
                                            const ca_process_result_t *process_result)
{
    char paths_json[CA_GT_PATHS_JSON_CAP];
    char *escaped_stderr;
    size_t output_escape_size;
    int written;
    ca_status_t status = CA_OK;

    if (result == NULL || tool_name == NULL || paths == NULL || process_result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (ca_gt_paths_json(paths, paths_json, sizeof(paths_json)) != CA_OK) {
        return ca_gt_result_error(result, tool_name, "RESULT_TOO_LARGE", "Git paths result exceeded buffer.");
    }

    output_escape_size = (size_t)CA_PROCESS_OUTPUT_CAP * 6u + 1u;
    escaped_stderr = (char *)malloc(output_escape_size);
    if (escaped_stderr == NULL) {
        return ca_gt_result_error(result, tool_name, "OUT_OF_MEMORY", "Failed to allocate Git result buffer.");
    }
    if (ca_json_escape_string(process_result->stderr_text, escaped_stderr, output_escape_size) != CA_OK) {
        free(escaped_stderr);
        return ca_gt_result_error(result, tool_name, "RESULT_TOO_LARGE", "Git stderr could not be encoded as JSON.");
    }

    ca_gt_result_reset(result, tool_name);
    result->success = 1;
    written = snprintf(result->result_json,
                       sizeof(result->result_json),
                       "{\"exit_code\":%d,\"paths\":%s,\"stderr\":\"%s\",\"truncated\":%s}",
                       process_result->exit_code,
                       paths_json,
                       escaped_stderr,
                       (process_result->stdout_truncated || process_result->stderr_truncated) ? "true" : "false");
    if (written < 0 || (size_t)written >= sizeof(result->result_json)) {
        status = ca_gt_result_error(result, tool_name, "RESULT_TOO_LARGE", "Git result exceeded result buffer.");
    }

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
     * Git exit_code != 0 still means the command ran. For example, outside a
     * repository Git returns 128 and stderr explains the condition.
     */
    return ca_gt_write_stdout_json(result, "git_status", "status", &process_result);
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
    char rel_path[CA_PROJECT_PATH_CAP];
    const char *code = "INVALID_ARGUMENT";
    const char *message = "Invalid git_diff path.";
    ca_status_t status;

    (void)ctx_value;
    ca_gt_result_reset(result, "git_diff");
    status = ca_gt_parse_diff_args(call, &args, result);
    if (status != CA_OK) {
        return status;
    }

    status = ca_gt_normalize_path(args.path, 1, rel_path, sizeof(rel_path), &code, &message);
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
    char rel_path[CA_PROJECT_PATH_CAP];
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

    status = ca_gt_normalize_path(args.path, 1, rel_path, sizeof(rel_path), &code, &message);
    if (status != CA_OK) {
        return ca_gt_result_error(result, "git_diff", code, message);
    }

    if (rel_path[0] == '\0') {
        written = snprintf(command, sizeof(command), "%s", args.staged ? "git diff --staged" : "git diff");
    } else {
        written = snprintf(command, sizeof(command), "%s -- \"%s\"", args.staged ? "git diff --staged" : "git diff", rel_path);
    }
    if (written < 0 || (size_t)written >= sizeof(command)) {
        return ca_gt_result_error(result, "git_diff", "INVALID_ARGUMENT", "git_diff path is too long.");
    }

    status = ca_gt_run_git_command(ctx, command, "git_diff", &process_result, result);
    if (status != CA_OK) {
        return status;
    }

    return ca_gt_write_stdout_json(result, "git_diff", "diff", &process_result);
}

static ca_status_t ca_gt_execute_log(const ca_tool_call_t *call,
                                     ca_tool_result_t *result,
                                     void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_process_result_t process_result;
    int max_count = 10;
    int oneline = 1;
    char command[128];
    ca_status_t status;

    ca_gt_result_reset(result, "git_log");
    if (call != NULL) {
        (void)ca_json_get_int(call->arguments_json, "max_count", &max_count);
        (void)ca_json_get_bool(call->arguments_json, "oneline", &oneline);
    }
    if (max_count <= 0) {
        max_count = 10;
    }
    if (max_count > 50) {
        max_count = 50;
    }

    if (snprintf(command, sizeof(command), "git log %s -n %d", oneline ? "--oneline" : "--stat --patch", max_count) >= (int)sizeof(command)) {
        return ca_gt_result_error(result, "git_log", "INVALID_ARGUMENT", "git_log command is too long.");
    }
    status = ca_gt_run_git_command(ctx, command, "git_log", &process_result, result);
    if (status != CA_OK) {
        return status;
    }
    return ca_gt_write_stdout_json(result, "git_log", "log", &process_result);
}

static ca_status_t ca_gt_parse_ref_path(const ca_tool_call_t *call,
                                        char *ref,
                                        size_t ref_size,
                                        char *path,
                                        size_t path_size,
                                        ca_tool_result_t *result)
{
    ca_status_t status;

    if (call == NULL || ref == NULL || path == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_json_get_string(call->arguments_json, "ref", ref, ref_size);
    if (status == CA_ERR_NOT_FOUND) {
        (void)ca_gt_copy(ref, ref_size, "HEAD");
        status = CA_OK;
    }
    if (status != CA_OK || !ca_gt_ref_is_valid(ref)) {
        return ca_gt_result_error(result, "git_show", "GIT_REF_INVALID", "Git ref is invalid.");
    }

    status = ca_json_get_string(call->arguments_json, "path", path, path_size);
    if (status == CA_ERR_NOT_FOUND) {
        path[0] = '\0';
        status = CA_OK;
    }
    if (status != CA_OK) {
        return ca_gt_result_error(result, "git_show", "INVALID_ARGUMENT", "path must be a string.");
    }

    return CA_OK;
}

static ca_status_t ca_gt_preflight_show(const ca_tool_call_t *call,
                                        ca_tool_result_t *result,
                                        void *ctx_value)
{
    char ref[CA_GT_REF_CAP];
    char raw_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    const char *code = "INVALID_ARGUMENT";
    const char *message = "Invalid git_show path.";
    ca_status_t status;

    (void)ctx_value;
    ca_gt_result_reset(result, "git_show");
    status = ca_gt_parse_ref_path(call, ref, sizeof(ref), raw_path, sizeof(raw_path), result);
    if (status != CA_OK) {
        return status;
    }
    status = ca_gt_normalize_path(raw_path, 1, rel_path, sizeof(rel_path), &code, &message);
    if (status != CA_OK) {
        return ca_gt_result_error(result, "git_show", code, message);
    }
    return CA_OK;
}

static ca_status_t ca_gt_execute_show(const ca_tool_call_t *call,
                                      ca_tool_result_t *result,
                                      void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_process_result_t process_result;
    char ref[CA_GT_REF_CAP];
    char raw_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    char command[CA_GT_COMMAND_CAP];
    const char *code = "INVALID_ARGUMENT";
    const char *message = "Invalid git_show path.";
    int written;
    ca_status_t status;

    ca_gt_result_reset(result, "git_show");
    status = ca_gt_parse_ref_path(call, ref, sizeof(ref), raw_path, sizeof(raw_path), result);
    if (status != CA_OK) {
        return status;
    }
    status = ca_gt_normalize_path(raw_path, 1, rel_path, sizeof(rel_path), &code, &message);
    if (status != CA_OK) {
        return ca_gt_result_error(result, "git_show", code, message);
    }

    if (rel_path[0] == '\0') {
        written = snprintf(command, sizeof(command), "git show --stat --patch \"%s\"", ref);
    } else {
        written = snprintf(command, sizeof(command), "git show \"%s\" -- \"%s\"", ref, rel_path);
    }
    if (written < 0 || (size_t)written >= sizeof(command)) {
        return ca_gt_result_error(result, "git_show", "INVALID_ARGUMENT", "git_show command is too long.");
    }
    status = ca_gt_run_git_command(ctx, command, "git_show", &process_result, result);
    if (status != CA_OK) {
        return status;
    }
    return ca_gt_write_stdout_json(result, "git_show", "content", &process_result);
}

static ca_status_t ca_gt_execute_branch(const ca_tool_call_t *call,
                                        ca_tool_result_t *result,
                                        void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_process_result_t process_result;
    int all = 0;
    ca_status_t status;

    ca_gt_result_reset(result, "git_branch");
    if (call != NULL) {
        (void)ca_json_get_bool(call->arguments_json, "all", &all);
    }
    status = ca_gt_run_git_command(ctx, all ? "git branch --all" : "git branch --list", "git_branch", &process_result, result);
    if (status != CA_OK) {
        return status;
    }
    return ca_gt_write_stdout_json(result, "git_branch", "branches", &process_result);
}

static ca_status_t ca_gt_preflight_paths_tool(const ca_tool_call_t *call,
                                              ca_tool_result_t *result,
                                              void *ctx_value,
                                              const char *tool_name)
{
    ca_git_paths_t paths;

    (void)ctx_value;
    ca_gt_result_reset(result, tool_name);
    return ca_gt_parse_paths(call, tool_name, &paths, result);
}

static ca_status_t ca_gt_preflight_add(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx_value)
{
    /*
     * git_add intentionally requires explicit file paths. "git add ." and
     * "git add -A" are not represented by this schema, preventing accidental
     * staging of unrelated user work.
     */
    return ca_gt_preflight_paths_tool(call, result, ctx_value, "git_add");
}

static ca_status_t ca_gt_preflight_unstage(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx_value)
{
    return ca_gt_preflight_paths_tool(call, result, ctx_value, "git_unstage");
}

static ca_status_t ca_gt_preflight_restore(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx_value)
{
    return ca_gt_preflight_paths_tool(call, result, ctx_value, "git_restore_file");
}

static ca_status_t ca_gt_execute_paths_command(const ca_tool_call_t *call,
                                               ca_tool_result_t *result,
                                               void *ctx_value,
                                               const char *tool_name,
                                               const char *command_prefix)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_git_paths_t paths;
    ca_process_result_t process_result;
    char command[CA_GT_COMMAND_CAP];
    ca_status_t status;

    ca_gt_result_reset(result, tool_name);
    status = ca_gt_parse_paths(call, tool_name, &paths, result);
    if (status != CA_OK) {
        return status;
    }
    status = ca_gt_build_paths_command(command_prefix, &paths, command, sizeof(command));
    if (status != CA_OK) {
        return ca_gt_result_error(result, tool_name, "INVALID_ARGUMENT", "Git path command is too long.");
    }
    status = ca_gt_run_git_command(ctx, command, tool_name, &process_result, result);
    if (status != CA_OK) {
        return status;
    }
    return ca_gt_write_paths_result(result, tool_name, &paths, &process_result);
}

static ca_status_t ca_gt_execute_add(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx_value)
{
    return ca_gt_execute_paths_command(call, result, ctx_value, "git_add", "git add");
}

static ca_status_t ca_gt_execute_unstage(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx_value)
{
    return ca_gt_execute_paths_command(call, result, ctx_value, "git_unstage", "git restore --staged");
}

static ca_status_t ca_gt_execute_restore(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx_value)
{
    return ca_gt_execute_paths_command(call, result, ctx_value, "git_restore_file", "git restore");
}

static ca_status_t ca_gt_parse_message(const ca_tool_call_t *call, char *message, size_t message_size, ca_tool_result_t *result)
{
    ca_status_t status;

    if (call == NULL || message == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    status = ca_json_get_string(call->arguments_json, "message", message, message_size);
    if (status == CA_ERR_NOT_FOUND) {
        return ca_gt_result_error(result, "git_commit", "MISSING_ARGUMENT", "git_commit requires a message.");
    }
    if (status != CA_OK || !ca_gt_message_is_valid(message)) {
        return ca_gt_result_error(result, "git_commit", "INVALID_ARGUMENT", "Commit message is invalid.");
    }
    return CA_OK;
}

static ca_status_t ca_gt_preflight_commit(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx_value)
{
    char message[CA_GT_MESSAGE_CAP];

    (void)ctx_value;
    ca_gt_result_reset(result, "git_commit");
    return ca_gt_parse_message(call, message, sizeof(message), result);
}

static ca_status_t ca_gt_execute_commit(const ca_tool_call_t *call,
                                        ca_tool_result_t *result,
                                        void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_process_result_t process_result;
    char message[CA_GT_MESSAGE_CAP];
    char command[CA_GT_COMMAND_CAP];
    char *escaped_stdout;
    char *escaped_stderr;
    size_t output_escape_size;
    int written;
    ca_status_t status;

    ca_gt_result_reset(result, "git_commit");
    status = ca_gt_parse_message(call, message, sizeof(message), result);
    if (status != CA_OK) {
        return status;
    }
    written = snprintf(command, sizeof(command), "git commit -m \"%s\"", message);
    if (written < 0 || (size_t)written >= sizeof(command)) {
        return ca_gt_result_error(result, "git_commit", "INVALID_ARGUMENT", "git_commit command is too long.");
    }
    status = ca_gt_run_git_command(ctx, command, "git_commit", &process_result, result);
    if (status != CA_OK) {
        return status;
    }

    output_escape_size = (size_t)CA_PROCESS_OUTPUT_CAP * 6u + 1u;
    escaped_stdout = (char *)malloc(output_escape_size);
    escaped_stderr = (char *)malloc(output_escape_size);
    if (escaped_stdout == NULL || escaped_stderr == NULL) {
        free(escaped_stdout);
        free(escaped_stderr);
        return ca_gt_result_error(result, "git_commit", "OUT_OF_MEMORY", "Failed to allocate commit result buffer.");
    }
    if (ca_json_escape_string(process_result.stdout_text, escaped_stdout, output_escape_size) != CA_OK ||
        ca_json_escape_string(process_result.stderr_text, escaped_stderr, output_escape_size) != CA_OK) {
        free(escaped_stdout);
        free(escaped_stderr);
        return ca_gt_result_error(result, "git_commit", "RESULT_TOO_LARGE", "Commit output could not be encoded as JSON.");
    }

    ca_gt_result_reset(result, "git_commit");
    result->success = 1;
    written = snprintf(result->result_json,
                       sizeof(result->result_json),
                       "{\"exit_code\":%d,\"stdout\":\"%s\",\"stderr\":\"%s\",\"truncated\":%s}",
                       process_result.exit_code,
                       escaped_stdout,
                       escaped_stderr,
                       (process_result.stdout_truncated || process_result.stderr_truncated) ? "true" : "false");
    free(escaped_stdout);
    free(escaped_stderr);
    if (written < 0 || (size_t)written >= sizeof(result->result_json)) {
        return ca_gt_result_error(result, "git_commit", "RESULT_TOO_LARGE", "Commit result exceeded buffer.");
    }
    return CA_OK;
}

static ca_status_t ca_gt_parse_branch_name(const ca_tool_call_t *call, char *name, size_t name_size, ca_tool_result_t *result)
{
    ca_status_t status;

    if (call == NULL || name == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    status = ca_json_get_string(call->arguments_json, "name", name, name_size);
    if (status == CA_ERR_NOT_FOUND) {
        return ca_gt_result_error(result, "git_create_branch", "MISSING_ARGUMENT", "git_create_branch requires a name.");
    }
    if (status != CA_OK || !ca_gt_branch_is_valid(name)) {
        return ca_gt_result_error(result, "git_create_branch", "GIT_BRANCH_INVALID", "Git branch name is invalid.");
    }
    return CA_OK;
}

static ca_status_t ca_gt_preflight_create_branch(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx_value)
{
    char name[CA_GT_BRANCH_CAP];

    (void)ctx_value;
    ca_gt_result_reset(result, "git_create_branch");
    return ca_gt_parse_branch_name(call, name, sizeof(name), result);
}

static ca_status_t ca_gt_execute_create_branch(const ca_tool_call_t *call,
                                               ca_tool_result_t *result,
                                               void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_process_result_t process_result;
    char name[CA_GT_BRANCH_CAP];
    char command[CA_GT_COMMAND_CAP];
    char escaped_branch[CA_GT_BRANCH_CAP * 2u];
    char *escaped_stderr;
    size_t output_escape_size;
    int written;
    ca_status_t status;

    ca_gt_result_reset(result, "git_create_branch");
    status = ca_gt_parse_branch_name(call, name, sizeof(name), result);
    if (status != CA_OK) {
        return status;
    }
    written = snprintf(command, sizeof(command), "git branch \"%s\"", name);
    if (written < 0 || (size_t)written >= sizeof(command)) {
        return ca_gt_result_error(result, "git_create_branch", "INVALID_ARGUMENT", "git_create_branch command is too long.");
    }
    status = ca_gt_run_git_command(ctx, command, "git_create_branch", &process_result, result);
    if (status != CA_OK) {
        return status;
    }

    output_escape_size = (size_t)CA_PROCESS_OUTPUT_CAP * 6u + 1u;
    escaped_stderr = (char *)malloc(output_escape_size);
    if (escaped_stderr == NULL) {
        return ca_gt_result_error(result, "git_create_branch", "OUT_OF_MEMORY", "Failed to allocate branch result buffer.");
    }
    if (ca_json_escape_string(name, escaped_branch, sizeof(escaped_branch)) != CA_OK ||
        ca_json_escape_string(process_result.stderr_text, escaped_stderr, output_escape_size) != CA_OK) {
        free(escaped_stderr);
        return ca_gt_result_error(result, "git_create_branch", "RESULT_TOO_LARGE", "Branch result could not be encoded as JSON.");
    }

    ca_gt_result_reset(result, "git_create_branch");
    result->success = 1;
    written = snprintf(result->result_json,
                       sizeof(result->result_json),
                       "{\"exit_code\":%d,\"branch\":\"%s\",\"stderr\":\"%s\",\"truncated\":%s}",
                       process_result.exit_code,
                       escaped_branch,
                       escaped_stderr,
                       (process_result.stdout_truncated || process_result.stderr_truncated) ? "true" : "false");
    free(escaped_stderr);
    if (written < 0 || (size_t)written >= sizeof(result->result_json)) {
        return ca_gt_result_error(result, "git_create_branch", "RESULT_TOO_LARGE", "Branch result exceeded buffer.");
    }
    return CA_OK;
}

ca_status_t ca_register_git_tools(ca_tool_registry_t *registry)
{
#ifdef CAGENT_ENABLE_GIT
    static const ca_tool_def_t tools[] = {
        {
            "git_status",
            "Show short Git working tree status.",
            "{\"type\":\"object\",\"properties\":{\"porcelain\":{\"type\":\"boolean\"}}}",
            CA_TOOL_PERMISSION_SAFE,
            ca_gt_execute_status,
            NULL
        },
        {
            "git_diff",
            "Show Git diff for the workspace or one file.",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"staged\":{\"type\":\"boolean\"}}}",
            CA_TOOL_PERMISSION_SAFE,
            ca_gt_execute_diff,
            ca_gt_preflight_diff
        },
        {
            "git_log",
            "Show recent local Git history.",
            "{\"type\":\"object\",\"properties\":{\"max_count\":{\"type\":\"integer\"},\"oneline\":{\"type\":\"boolean\"}}}",
            CA_TOOL_PERMISSION_SAFE,
            ca_gt_execute_log,
            NULL
        },
        {
            "git_show",
            "Show a commit or one path at a Git ref.",
            "{\"type\":\"object\",\"properties\":{\"ref\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}}}",
            CA_TOOL_PERMISSION_SAFE,
            ca_gt_execute_show,
            ca_gt_preflight_show
        },
        {
            "git_branch",
            "List local Git branches.",
            "{\"type\":\"object\",\"properties\":{\"all\":{\"type\":\"boolean\"}}}",
            CA_TOOL_PERMISSION_SAFE,
            ca_gt_execute_branch,
            NULL
        },
        {
            "git_add",
            "Stage explicit workspace paths in the Git index.",
            "{\"type\":\"object\",\"properties\":{\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"paths\"]}",
            CA_TOOL_PERMISSION_ASK,
            ca_gt_execute_add,
            ca_gt_preflight_add
        },
        {
            "git_unstage",
            "Unstage explicit paths without discarding working tree changes.",
            "{\"type\":\"object\",\"properties\":{\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"paths\"]}",
            CA_TOOL_PERMISSION_ASK,
            ca_gt_execute_unstage,
            ca_gt_preflight_unstage
        },
        {
            "git_commit",
            "Create a local Git commit from staged changes.",
            "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"}},\"required\":[\"message\"]}",
            CA_TOOL_PERMISSION_ASK,
            ca_gt_execute_commit,
            ca_gt_preflight_commit
        },
        {
            "git_create_branch",
            "Create a local branch without checking it out.",
            "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}",
            CA_TOOL_PERMISSION_ASK,
            ca_gt_execute_create_branch,
            ca_gt_preflight_create_branch
        },
        {
            "git_restore_file",
            "Discard working tree changes for explicit files.",
            "{\"type\":\"object\",\"properties\":{\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"paths\"]}",
            CA_TOOL_PERMISSION_DANGEROUS,
            ca_gt_execute_restore,
            ca_gt_preflight_restore
        }
    };
    size_t i;
    ca_status_t status;

    if (registry == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    for (i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        status = ca_tool_registry_register(registry, &tools[i]);
        if (status != CA_OK) {
            return status;
        }
    }
    return CA_OK;
#else
    (void)registry;
    return CA_OK;
#endif
}
