/*
 * write_tools.c
 * Phase 9A/9B guarded write tools. Preflight rejects obviously invalid calls
 * before permission prompts; execute repeats critical checks before writing.
 */
#include "ca_file_tools.h"
#include "ca_edit_tracking.h"
#include "ca_json.h"
#include "ca_limits.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define CA_WT_PATH_SEP "\\"
#else
#define CA_WT_PATH_SEP "/"
#endif

#define CA_WT_CONTENT_CAP CA_MAX_PATCH_SIZE

typedef struct ca_write_args {
    char path[CA_PROJECT_PATH_CAP];
    char content[CA_WT_CONTENT_CAP];
    int allow_overwrite;
    int allow_create;
} ca_write_args_t;

static ca_status_t ca_wt_prepare_path(const ca_tool_context_t *ctx,
                                      const ca_write_args_t *args,
                                      const char *tool_name,
                                      ca_tool_result_t *result,
                                      char *abs_path,
                                      size_t abs_size,
                                      char *rel_path,
                                      size_t rel_size);

static ca_status_t ca_wt_copy(char *dest, size_t dest_size, const char *src)
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

static void ca_wt_result_reset(ca_tool_result_t *result, const char *tool_name)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    if (tool_name != NULL) {
        (void)ca_wt_copy(result->tool_name, sizeof(result->tool_name), tool_name);
    }
}

static ca_status_t ca_wt_result_error(ca_tool_result_t *result,
                                      const char *tool_name,
                                      const char *code,
                                      const char *message)
{
    if (result == NULL || code == NULL || message == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    ca_wt_result_reset(result, tool_name);
    result->success = 0;
    (void)ca_wt_copy(result->error_code, sizeof(result->error_code), code);
    (void)ca_wt_copy(result->error_message, sizeof(result->error_message), message);
    return CA_ERR_TOOL_FAILED;
}

static ca_status_t ca_wt_result_success(ca_tool_result_t *result,
                                        const char *tool_name,
                                        const char *path,
                                        size_t bytes_written,
                                        int created)
{
    char escaped_path[CA_PROJECT_PATH_CAP * 2];
    int written;

    if (result == NULL || tool_name == NULL || path == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    ca_wt_result_reset(result, tool_name);
    result->success = 1;
    if (ca_json_escape_string(path, escaped_path, sizeof(escaped_path)) != CA_OK) {
        return ca_wt_result_error(result, tool_name, "RESULT_TOO_LARGE", "Result path is too large.");
    }

    written = snprintf(result->result_json,
                       sizeof(result->result_json),
                       "{\"path\":\"%s\",\"bytes_written\":%llu,\"created\":%s}",
                       escaped_path,
                       (unsigned long long)bytes_written,
                       created ? "true" : "false");
    if (written < 0 || (size_t)written >= sizeof(result->result_json)) {
        return ca_wt_result_error(result, tool_name, "RESULT_TOO_LARGE", "Write result exceeded buffer.");
    }

    return CA_OK;
}

static int ca_wt_is_absolute_path(const char *path)
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

static int ca_wt_segment_equals(const char *start, size_t len, const char *value)
{
    return value != NULL && strlen(value) == len && strncmp(start, value, len) == 0;
}

static int ca_wt_has_forbidden_segment(const char *path, const char **out_code, const char **out_message)
{
    const char *segment = path;
    const char *cursor;

    if (path == NULL) {
        return 0;
    }

    for (cursor = path; ; cursor++) {
        if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
            size_t len = (size_t)(cursor - segment);

            if (ca_wt_segment_equals(segment, len, "..")) {
                if (out_code != NULL) {
                    *out_code = "PATH_OUTSIDE_WORKSPACE";
                }
                if (out_message != NULL) {
                    *out_message = "Path must not contain '..'.";
                }
                return 1;
            }
            if (ca_wt_segment_equals(segment, len, ".git")) {
                if (out_code != NULL) {
                    *out_code = "PROTECTED_PATH";
                }
                if (out_message != NULL) {
                    *out_message = "Refusing to write inside .git.";
                }
                return 1;
            }
            if (ca_wt_segment_equals(segment, len, "build")) {
                if (out_code != NULL) {
                    *out_code = "PROTECTED_PATH";
                }
                if (out_message != NULL) {
                    *out_message = "Refusing to write inside build.";
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

static int ca_wt_content_is_text(const char *content)
{
    const unsigned char *cursor;

    if (content == NULL) {
        return 0;
    }

    /*
     * JSON arguments arrive as C strings, so embedded NUL bytes cannot be
     * represented safely here. Other control bytes are rejected except common
     * text whitespace.
     */
    for (cursor = (const unsigned char *)content; *cursor != '\0'; cursor++) {
        if (*cursor < 0x20 && *cursor != '\n' && *cursor != '\r' && *cursor != '\t') {
            return 0;
        }
    }

    return 1;
}

static ca_status_t ca_wt_join_workspace_path(const ca_tool_context_t *ctx,
                                             const char *input_path,
                                             char *out_abs_path,
                                             size_t out_abs_size,
                                             char *out_rel_path,
                                             size_t out_rel_size,
                                             const char **out_error_code,
                                             const char **out_error_message)
{
    const char *rel;
    int written;

    if (ctx == NULL || ctx->workspace_root == NULL || input_path == NULL ||
        out_abs_path == NULL || out_abs_size == 0 || out_rel_path == NULL || out_rel_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    if (input_path[0] == '\0') {
        if (out_error_code != NULL) {
            *out_error_code = "INVALID_PATH";
        }
        if (out_error_message != NULL) {
            *out_error_message = "Path must not be empty.";
        }
        return CA_ERR_INVALID_ARG;
    }
    if (ca_wt_is_absolute_path(input_path)) {
        if (out_error_code != NULL) {
            *out_error_code = "PATH_OUTSIDE_WORKSPACE";
        }
        if (out_error_message != NULL) {
            *out_error_message = "Absolute paths are not allowed for write tools.";
        }
        return CA_ERR_PERMISSION_DENIED;
    }
    if (ca_wt_has_forbidden_segment(input_path, out_error_code, out_error_message)) {
        return CA_ERR_PERMISSION_DENIED;
    }

    rel = input_path;
    while (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) {
        rel += 2;
    }
    if (rel[0] == '\0' || strcmp(rel, ".") == 0) {
        if (out_error_code != NULL) {
            *out_error_code = "INVALID_PATH";
        }
        if (out_error_message != NULL) {
            *out_error_message = "Path must name a file.";
        }
        return CA_ERR_INVALID_ARG;
    }

    if (ca_wt_has_forbidden_segment(rel, out_error_code, out_error_message)) {
        return CA_ERR_PERMISSION_DENIED;
    }

    written = snprintf(out_rel_path, out_rel_size, "%s", rel);
    if (written < 0 || (size_t)written >= out_rel_size) {
        return CA_ERR_INVALID_ARG;
    }
    written = snprintf(out_abs_path, out_abs_size, "%s%s%s", ctx->workspace_root, CA_WT_PATH_SEP, rel);
    if (written < 0 || (size_t)written >= out_abs_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static int ca_wt_file_exists(const char *path)
{
    FILE *file;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

static ca_status_t ca_wt_parse_common_args(const ca_tool_call_t *call,
                                           ca_write_args_t *args,
                                           const char *tool_name,
                                           ca_tool_result_t *result)
{
    ca_status_t status;

    if (call == NULL || args == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(args, 0, sizeof(*args));
    status = ca_json_get_string(call->arguments_json, "path", args->path, sizeof(args->path));
    if (status == CA_ERR_NOT_FOUND) {
        return ca_wt_result_error(result, tool_name, "MISSING_ARGUMENT", "Tool requires a path field.");
    }
    if (status != CA_OK) {
        return ca_wt_result_error(result, tool_name, "INVALID_ARGUMENTS", "Path must be a string.");
    }
    if (args->path[0] == '\0') {
        return ca_wt_result_error(result, tool_name, "INVALID_PATH", "Path must not be empty.");
    }

    status = ca_json_get_string(call->arguments_json, "content", args->content, sizeof(args->content));
    if (status == CA_ERR_NOT_FOUND) {
        return ca_wt_result_error(result, tool_name, "MISSING_ARGUMENT", "Tool requires a content field.");
    }
    if (status != CA_OK) {
        return ca_wt_result_error(result, tool_name, "INVALID_ARGUMENTS", "Content must be a string.");
    }
    if (!ca_wt_content_is_text(args->content)) {
        return ca_wt_result_error(result, tool_name, "BINARY_CONTENT", "Refusing to write binary content.");
    }

    return CA_OK;
}

static ca_status_t ca_wt_preflight_create_file(const ca_tool_call_t *call,
                                               ca_tool_result_t *result,
                                               void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_write_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    ca_status_t status;

    if (ctx == NULL || call == NULL) {
        return ca_wt_result_error(result, "create_file", "INVALID_CONTEXT", "Tool context is missing.");
    }

    status = ca_wt_parse_common_args(call, &args, "create_file", result);
    if (status != CA_OK) {
        return status;
    }
    (void)ca_json_get_bool(call->arguments_json, "allow_overwrite", &args.allow_overwrite);

    status = ca_wt_prepare_path(ctx, &args, "create_file", result, abs_path, sizeof(abs_path), rel_path, sizeof(rel_path));
    if (status != CA_OK) {
        return status;
    }
    if (ca_wt_file_exists(abs_path) && !args.allow_overwrite) {
        return ca_wt_result_error(result, "create_file", "FILE_EXISTS", "Target file already exists.");
    }

    return CA_OK;
}

static ca_status_t ca_wt_preflight_write_file(const ca_tool_call_t *call,
                                              ca_tool_result_t *result,
                                              void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_write_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    ca_status_t status;

    if (ctx == NULL || call == NULL) {
        return ca_wt_result_error(result, "write_file", "INVALID_CONTEXT", "Tool context is missing.");
    }

    status = ca_wt_parse_common_args(call, &args, "write_file", result);
    if (status != CA_OK) {
        return status;
    }
    (void)ca_json_get_bool(call->arguments_json, "allow_create", &args.allow_create);

    status = ca_wt_prepare_path(ctx, &args, "write_file", result, abs_path, sizeof(abs_path), rel_path, sizeof(rel_path));
    if (status != CA_OK) {
        return status;
    }
    if (!ca_wt_file_exists(abs_path) && !args.allow_create) {
        return ca_wt_result_error(result, "write_file", "FILE_NOT_FOUND", "Target file does not exist.");
    }

    return CA_OK;
}

static ca_status_t ca_wt_preflight_append_file(const ca_tool_call_t *call,
                                               ca_tool_result_t *result,
                                               void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_write_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    ca_status_t status;

    if (ctx == NULL || call == NULL) {
        return ca_wt_result_error(result, "append_file", "INVALID_CONTEXT", "Tool context is missing.");
    }

    status = ca_wt_parse_common_args(call, &args, "append_file", result);
    if (status != CA_OK) {
        return status;
    }

    status = ca_wt_prepare_path(ctx, &args, "append_file", result, abs_path, sizeof(abs_path), rel_path, sizeof(rel_path));
    if (status != CA_OK) {
        return status;
    }

    return CA_OK;
}

static ca_status_t ca_wt_write_bytes(const char *abs_path, const char *content, const char *mode, size_t *out_written)
{
    FILE *file;
    size_t len;
    size_t wrote;

    if (abs_path == NULL || content == NULL || mode == NULL || out_written == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *out_written = 0;
    file = fopen(abs_path, mode);
    if (file == NULL) {
        return CA_ERR_IO;
    }

    len = strlen(content);
    wrote = fwrite(content, 1, len, file);
    if (wrote != len || ferror(file)) {
        fclose(file);
        return CA_ERR_IO;
    }
    if (fclose(file) != 0) {
        return CA_ERR_IO;
    }

    *out_written = wrote;
    return CA_OK;
}

static ca_status_t ca_wt_prepare_path(const ca_tool_context_t *ctx,
                                      const ca_write_args_t *args,
                                      const char *tool_name,
                                      ca_tool_result_t *result,
                                      char *abs_path,
                                      size_t abs_size,
                                      char *rel_path,
                                      size_t rel_size)
{
    const char *error_code = "INVALID_PATH";
    const char *error_message = "Path is invalid.";
    ca_status_t status;

    status = ca_wt_join_workspace_path(ctx,
                                       args->path,
                                       abs_path,
                                       abs_size,
                                       rel_path,
                                       rel_size,
                                       &error_code,
                                       &error_message);
    if (status != CA_OK) {
        (void)ca_wt_result_error(result, tool_name, error_code, error_message);
        return status;
    }

    return CA_OK;
}

static ca_status_t ca_wt_execute_create_file(const ca_tool_call_t *call,
                                             ca_tool_result_t *result,
                                             void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_write_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    size_t bytes_written;
    int existed;
    ca_status_t status;

    ca_wt_result_reset(result, "create_file");
    if (ctx == NULL || call == NULL) {
        return ca_wt_result_error(result, "create_file", "INVALID_CONTEXT", "Tool context is missing.");
    }
    status = ca_wt_parse_common_args(call, &args, "create_file", result);
    if (status != CA_OK) {
        return status;
    }
    (void)ca_json_get_bool(call->arguments_json, "allow_overwrite", &args.allow_overwrite);

    status = ca_wt_prepare_path(ctx, &args, "create_file", result, abs_path, sizeof(abs_path), rel_path, sizeof(rel_path));
    if (status != CA_OK) {
        return status;
    }
    existed = ca_wt_file_exists(abs_path);
    if (existed && !args.allow_overwrite) {
        return ca_wt_result_error(result, "create_file", "FILE_EXISTS", "Target file already exists.");
    }

    status = ca_wt_write_bytes(abs_path, args.content, "wb", &bytes_written);
    if (status != CA_OK) {
        return ca_wt_result_error(result, "create_file", "WRITE_FAILED", "Failed to create file.");
    }
    if (ctx->edit_tracking != NULL &&
        ca_edit_tracking_note_write(ctx->edit_tracking, ctx->workspace_root, rel_path) != CA_OK) {
        return ca_wt_result_error(result, "create_file", "TRACKING_FAILED", "Failed to update edit tracking.");
    }

    return ca_wt_result_success(result, "create_file", rel_path, bytes_written, !existed);
}

static ca_status_t ca_wt_execute_write_file(const ca_tool_call_t *call,
                                            ca_tool_result_t *result,
                                            void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_write_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    size_t bytes_written;
    int existed;
    ca_status_t status;

    ca_wt_result_reset(result, "write_file");
    if (ctx == NULL || call == NULL) {
        return ca_wt_result_error(result, "write_file", "INVALID_CONTEXT", "Tool context is missing.");
    }
    status = ca_wt_parse_common_args(call, &args, "write_file", result);
    if (status != CA_OK) {
        return status;
    }
    (void)ca_json_get_bool(call->arguments_json, "allow_create", &args.allow_create);

    status = ca_wt_prepare_path(ctx, &args, "write_file", result, abs_path, sizeof(abs_path), rel_path, sizeof(rel_path));
    if (status != CA_OK) {
        return status;
    }

    existed = ca_wt_file_exists(abs_path);
    if (!existed && !args.allow_create) {
        return ca_wt_result_error(result, "write_file", "FILE_NOT_FOUND", "Target file does not exist.");
    }

    status = ca_wt_write_bytes(abs_path, args.content, "wb", &bytes_written);
    if (status != CA_OK) {
        return ca_wt_result_error(result, "write_file", "WRITE_FAILED", "Failed to write file.");
    }
    if (ctx->edit_tracking != NULL &&
        ca_edit_tracking_note_write(ctx->edit_tracking, ctx->workspace_root, rel_path) != CA_OK) {
        return ca_wt_result_error(result, "write_file", "TRACKING_FAILED", "Failed to update edit tracking.");
    }

    return ca_wt_result_success(result, "write_file", rel_path, bytes_written, !existed);
}

static ca_status_t ca_wt_execute_append_file(const ca_tool_call_t *call,
                                             ca_tool_result_t *result,
                                             void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_write_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    size_t bytes_written;
    int existed;
    ca_status_t status;

    ca_wt_result_reset(result, "append_file");
    if (ctx == NULL || call == NULL) {
        return ca_wt_result_error(result, "append_file", "INVALID_CONTEXT", "Tool context is missing.");
    }
    status = ca_wt_parse_common_args(call, &args, "append_file", result);
    if (status != CA_OK) {
        return status;
    }

    status = ca_wt_prepare_path(ctx, &args, "append_file", result, abs_path, sizeof(abs_path), rel_path, sizeof(rel_path));
    if (status != CA_OK) {
        return status;
    }

    existed = ca_wt_file_exists(abs_path);
    status = ca_wt_write_bytes(abs_path, args.content, "ab", &bytes_written);
    if (status != CA_OK) {
        return ca_wt_result_error(result, "append_file", "WRITE_FAILED", "Failed to append file.");
    }
    if (ctx->edit_tracking != NULL &&
        ca_edit_tracking_note_write(ctx->edit_tracking, ctx->workspace_root, rel_path) != CA_OK) {
        return ca_wt_result_error(result, "append_file", "TRACKING_FAILED", "Failed to update edit tracking.");
    }

    return ca_wt_result_success(result, "append_file", rel_path, bytes_written, !existed);
}

ca_status_t ca_register_write_file_tools(ca_tool_registry_t *registry)
{
    static const ca_tool_def_t tools[] = {
        {
            "create_file",
            "Create a new workspace text file. Refuses existing files.",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"},\"allow_overwrite\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"content\"]}",
            CA_TOOL_PERMISSION_ASK,
            ca_wt_execute_create_file,
            ca_wt_preflight_create_file
        },
        {
            "write_file",
            "Replace a workspace text file, or create it only when allow_create is true.",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"},\"allow_create\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"content\"]}",
            CA_TOOL_PERMISSION_ASK,
            ca_wt_execute_write_file,
            ca_wt_preflight_write_file
        },
        {
            "append_file",
            "Append text content to a workspace file, creating it when missing.",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
            CA_TOOL_PERMISSION_ASK,
            ca_wt_execute_append_file,
            ca_wt_preflight_append_file
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
}
