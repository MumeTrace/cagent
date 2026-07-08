/*
 * diff_tools.c
 * Phase 10A diff preview tool. It is deliberately read-only: it reads the
 * current file, compares it with new_content, and returns bounded JSON diff.
 */
#include "ca_file_tools.h"
#include "ca_json.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define CA_DT_PATH_SEP "\\"
#else
#define CA_DT_PATH_SEP "/"
#endif

#define CA_DT_MAX_FILE_BYTES   (64u * 1024u)
#define CA_DT_MAX_NEW_BYTES    (64u * 1024u)
#define CA_DT_MAX_DIFF_BYTES   (64u * 1024u)
#define CA_DT_SAMPLE_BYTES     4096u
#define CA_DT_MAX_LCS_CELLS    1000000u

typedef struct ca_diff_args {
    char path[CA_PROJECT_PATH_CAP];
    char mode[32];
    char *new_content;
} ca_diff_args_t;

typedef struct ca_diff_line {
    const char *start;
    size_t len;
} ca_diff_line_t;

typedef struct ca_diff_buffer {
    char *data;
    size_t len;
    size_t cap;
    int truncated;
} ca_diff_buffer_t;

static ca_status_t ca_dt_copy(char *dest, size_t dest_size, const char *src)
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

static void ca_dt_result_reset(ca_tool_result_t *result, const char *tool_name)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    if (tool_name != NULL) {
        (void)ca_dt_copy(result->tool_name, sizeof(result->tool_name), tool_name);
    }
}

static ca_status_t ca_dt_result_error(ca_tool_result_t *result,
                                      const char *code,
                                      const char *message)
{
    if (result == NULL || code == NULL || message == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    ca_dt_result_reset(result, "preview_file_change");
    result->success = 0;
    (void)ca_dt_copy(result->error_code, sizeof(result->error_code), code);
    (void)ca_dt_copy(result->error_message, sizeof(result->error_message), message);
    return CA_ERR_TOOL_FAILED;
}

static int ca_dt_segment_equals(const char *start, size_t len, const char *value)
{
    return value != NULL && strlen(value) == len && strncmp(start, value, len) == 0;
}

static int ca_dt_is_absolute_path(const char *path)
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

static int ca_dt_forbidden_segment(const char *path, const char **out_code, const char **out_message)
{
    const char *segment = path;
    const char *cursor;

    if (path == NULL) {
        return 0;
    }

    for (cursor = path; ; cursor++) {
        if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
            size_t len = (size_t)(cursor - segment);

            if (ca_dt_segment_equals(segment, len, "..")) {
                *out_code = "PATH_OUTSIDE_WORKSPACE";
                *out_message = "Path must not contain '..'.";
                return 1;
            }
            if (ca_dt_segment_equals(segment, len, ".git")) {
                *out_code = "PROTECTED_PATH";
                *out_message = "Refusing to read inside .git.";
                return 1;
            }
            if (ca_dt_segment_equals(segment, len, "build")) {
                *out_code = "PROTECTED_PATH";
                *out_message = "Refusing to read inside build.";
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

static ca_status_t ca_dt_join_workspace_path(const ca_tool_context_t *ctx,
                                             const char *input_path,
                                             char *out_abs,
                                             size_t out_abs_size,
                                             char *out_rel,
                                             size_t out_rel_size,
                                             const char **out_code,
                                             const char **out_message)
{
    const char *rel;
    int written;

    if (ctx == NULL || ctx->workspace_root == NULL || input_path == NULL ||
        out_abs == NULL || out_abs_size == 0 || out_rel == NULL || out_rel_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    if (input_path[0] == '\0') {
        *out_code = "INVALID_ARGUMENT";
        *out_message = "Path must not be empty.";
        return CA_ERR_INVALID_ARG;
    }
    if (ca_dt_is_absolute_path(input_path)) {
        *out_code = "PATH_OUTSIDE_WORKSPACE";
        *out_message = "Absolute paths are not allowed.";
        return CA_ERR_PERMISSION_DENIED;
    }
    if (ca_dt_forbidden_segment(input_path, out_code, out_message)) {
        return CA_ERR_PERMISSION_DENIED;
    }

    rel = input_path;
    while (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) {
        rel += 2;
    }
    if (rel[0] == '\0' || strcmp(rel, ".") == 0) {
        *out_code = "INVALID_ARGUMENT";
        *out_message = "Path must name a file.";
        return CA_ERR_INVALID_ARG;
    }
    if (ca_dt_forbidden_segment(rel, out_code, out_message)) {
        return CA_ERR_PERMISSION_DENIED;
    }

    written = snprintf(out_rel, out_rel_size, "%s", rel);
    if (written < 0 || (size_t)written >= out_rel_size) {
        *out_code = "INVALID_ARGUMENT";
        *out_message = "Path is too long.";
        return CA_ERR_INVALID_ARG;
    }
    written = snprintf(out_abs, out_abs_size, "%s%s%s", ctx->workspace_root, CA_DT_PATH_SEP, rel);
    if (written < 0 || (size_t)written >= out_abs_size) {
        *out_code = "INVALID_ARGUMENT";
        *out_message = "Path is too long.";
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static ca_status_t ca_dt_parse_args(const ca_tool_call_t *call, ca_diff_args_t *args, ca_tool_result_t *result)
{
    ca_status_t status;

    if (call == NULL || args == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(args, 0, sizeof(*args));
    status = ca_json_get_string(call->arguments_json, "path", args->path, sizeof(args->path));
    if (status == CA_ERR_NOT_FOUND) {
        return ca_dt_result_error(result, "MISSING_ARGUMENT", "preview_file_change requires path.");
    }
    if (status != CA_OK || args->path[0] == '\0') {
        return ca_dt_result_error(result, "INVALID_ARGUMENT", "path must be a non-empty string.");
    }

    args->new_content = (char *)malloc(CA_DT_MAX_NEW_BYTES + 1u);
    if (args->new_content == NULL) {
        return ca_dt_result_error(result, "OUT_OF_MEMORY", "Failed to allocate new_content buffer.");
    }
    status = ca_json_get_string(call->arguments_json, "new_content", args->new_content, CA_DT_MAX_NEW_BYTES + 1u);
    if (status == CA_ERR_NOT_FOUND) {
        free(args->new_content);
        args->new_content = NULL;
        return ca_dt_result_error(result, "MISSING_ARGUMENT", "preview_file_change requires new_content.");
    }
    if (status != CA_OK) {
        free(args->new_content);
        args->new_content = NULL;
        return ca_dt_result_error(result, "INVALID_ARGUMENT", "new_content must be a string within size limits.");
    }

    status = ca_json_get_string(call->arguments_json, "mode", args->mode, sizeof(args->mode));
    if (status == CA_ERR_NOT_FOUND) {
        (void)ca_dt_copy(args->mode, sizeof(args->mode), "replace");
    } else if (status != CA_OK || args->mode[0] == '\0') {
        free(args->new_content);
        args->new_content = NULL;
        return ca_dt_result_error(result, "INVALID_ARGUMENT", "mode must be a string.");
    }
    if (strcmp(args->mode, "replace") != 0) {
        free(args->new_content);
        args->new_content = NULL;
        return ca_dt_result_error(result, "UNSUPPORTED_MODE", "Only mode=replace is supported in this phase.");
    }

    return CA_OK;
}

static int ca_dt_content_is_text(const char *content)
{
    const unsigned char *cursor;

    if (content == NULL) {
        return 0;
    }
    for (cursor = (const unsigned char *)content; *cursor != '\0'; cursor++) {
        if (*cursor < 0x20 && *cursor != '\n' && *cursor != '\r' && *cursor != '\t') {
            return 0;
        }
    }
    return 1;
}

static ca_status_t ca_dt_read_text_file(const char *abs_path, char **out_text, size_t *out_size)
{
    FILE *file;
    long file_size;
    char *text;
    size_t got;
    size_t sample_len;
    size_t i;

    if (abs_path == NULL || out_text == NULL || out_size == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *out_text = NULL;
    *out_size = 0;
    file = fopen(abs_path, "rb");
    if (file == NULL) {
        return CA_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return CA_ERR_IO;
    }
    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return CA_ERR_IO;
    }
    if ((unsigned long)file_size > CA_DT_MAX_FILE_BYTES) {
        fclose(file);
        return CA_ERR_INVALID_ARG;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return CA_ERR_IO;
    }

    text = (char *)malloc((size_t)file_size + 1u);
    if (text == NULL) {
        fclose(file);
        return CA_ERR_NO_MEMORY;
    }
    got = fread(text, 1, (size_t)file_size, file);
    if (got != (size_t)file_size || ferror(file)) {
        free(text);
        fclose(file);
        return CA_ERR_IO;
    }
    fclose(file);
    text[got] = '\0';

    sample_len = got < CA_DT_SAMPLE_BYTES ? got : CA_DT_SAMPLE_BYTES;
    for (i = 0; i < sample_len; i++) {
        if (text[i] == '\0') {
            free(text);
            return CA_ERR_PERMISSION_DENIED;
        }
    }

    *out_text = text;
    *out_size = got;
    return CA_OK;
}

static int ca_dt_line_equal(ca_diff_line_t a, ca_diff_line_t b)
{
    return a.len == b.len && memcmp(a.start, b.start, a.len) == 0;
}

static size_t ca_dt_count_lines(const char *text)
{
    size_t count = 0;
    const char *cursor;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '\n') {
            count++;
        }
    }
    if (cursor > text && cursor[-1] != '\n') {
        count++;
    }
    return count;
}

static ca_status_t ca_dt_split_lines(const char *text, ca_diff_line_t **out_lines, size_t *out_count)
{
    ca_diff_line_t *lines;
    const char *start;
    const char *cursor;
    size_t count;
    size_t index = 0;

    if (text == NULL || out_lines == NULL || out_count == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    count = ca_dt_count_lines(text);
    *out_lines = NULL;
    *out_count = count;
    if (count == 0) {
        return CA_OK;
    }

    lines = (ca_diff_line_t *)calloc(count, sizeof(*lines));
    if (lines == NULL) {
        return CA_ERR_NO_MEMORY;
    }

    start = text;
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '\n') {
            lines[index].start = start;
            lines[index].len = (size_t)(cursor - start + 1);
            index++;
            start = cursor + 1;
        }
    }
    if (*start != '\0') {
        lines[index].start = start;
        lines[index].len = strlen(start);
        index++;
    }

    *out_lines = lines;
    *out_count = index;
    return CA_OK;
}

static void ca_dt_buffer_init(ca_diff_buffer_t *buffer, char *data, size_t cap)
{
    if (buffer == NULL) {
        return;
    }
    buffer->data = data;
    buffer->len = 0;
    buffer->cap = cap;
    buffer->truncated = 0;
    if (data != NULL && cap > 0) {
        data[0] = '\0';
    }
}

static void ca_dt_buffer_append_mem(ca_diff_buffer_t *buffer, const char *text, size_t len)
{
    size_t available;
    size_t copy_len;

    if (buffer == NULL || buffer->data == NULL || buffer->cap == 0 || text == NULL || len == 0 || buffer->truncated) {
        return;
    }
    if (buffer->len + 1u >= buffer->cap) {
        buffer->truncated = 1;
        return;
    }

    available = buffer->cap - buffer->len - 1u;
    copy_len = len < available ? len : available;
    memcpy(buffer->data + buffer->len, text, copy_len);
    buffer->len += copy_len;
    buffer->data[buffer->len] = '\0';
    if (copy_len < len) {
        buffer->truncated = 1;
    }
}

static void ca_dt_buffer_append(ca_diff_buffer_t *buffer, const char *text)
{
    if (text != NULL) {
        ca_dt_buffer_append_mem(buffer, text, strlen(text));
    }
}

static void ca_dt_buffer_append_line(ca_diff_buffer_t *buffer, char prefix, ca_diff_line_t line)
{
    ca_dt_buffer_append_mem(buffer, &prefix, 1);
    ca_dt_buffer_append_mem(buffer, line.start, line.len);
    if (line.len == 0 || line.start[line.len - 1] != '\n') {
        ca_dt_buffer_append(buffer, "\n");
    }
}

static ca_status_t ca_dt_build_lcs(const ca_diff_line_t *old_lines,
                                   size_t old_count,
                                   const ca_diff_line_t *new_lines,
                                   size_t new_count,
                                   uint16_t **out_table)
{
    uint16_t *table;
    size_t cells;
    size_t i;
    size_t j;

    if (out_table == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    *out_table = NULL;
    if (old_count > UINT16_MAX || new_count > UINT16_MAX) {
        return CA_ERR_INVALID_ARG;
    }

    cells = (old_count + 1u) * (new_count + 1u);
    if (cells > CA_DT_MAX_LCS_CELLS) {
        return CA_ERR_INVALID_ARG;
    }
    table = (uint16_t *)calloc(cells, sizeof(*table));
    if (table == NULL) {
        return CA_ERR_NO_MEMORY;
    }

#define CA_DT_LCS(i_, j_) table[(i_) * (new_count + 1u) + (j_)]
    for (i = old_count; i > 0; i--) {
        for (j = new_count; j > 0; j--) {
            size_t oi = i - 1u;
            size_t nj = j - 1u;
            if (ca_dt_line_equal(old_lines[oi], new_lines[nj])) {
                CA_DT_LCS(oi, nj) = (uint16_t)(CA_DT_LCS(oi + 1u, nj + 1u) + 1u);
            } else {
                uint16_t down = CA_DT_LCS(oi + 1u, nj);
                uint16_t right = CA_DT_LCS(oi, nj + 1u);
                CA_DT_LCS(oi, nj) = down > right ? down : right;
            }
        }
    }
#undef CA_DT_LCS

    *out_table = table;
    return CA_OK;
}

static ca_status_t ca_dt_generate_diff(const char *path,
                                       const ca_diff_line_t *old_lines,
                                       size_t old_count,
                                       const ca_diff_line_t *new_lines,
                                       size_t new_count,
                                       char *diff,
                                       size_t diff_size,
                                       int *out_truncated)
{
    uint16_t *lcs = NULL;
    ca_diff_buffer_t buffer;
    char header[CA_PROJECT_PATH_CAP * 2 + 128];
    size_t i = 0;
    size_t j = 0;
    size_t stride = new_count + 1u;
    int written;
    ca_status_t status;

    if (path == NULL || diff == NULL || diff_size == 0 || out_truncated == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    ca_dt_buffer_init(&buffer, diff, diff_size);
    status = ca_dt_build_lcs(old_lines, old_count, new_lines, new_count, &lcs);
    if (status != CA_OK) {
        return status;
    }

    /*
     * MVP uses one whole-file hunk. LCS keeps line-level additions/deletions
     * readable without pulling in a full diff library; later patch work can
     * replace this with context-window hunks.
     */
    written = snprintf(header,
                       sizeof(header),
                       "--- %s\n+++ %s\n@@ -1,%llu +1,%llu @@\n",
                       path,
                       path,
                       (unsigned long long)old_count,
                       (unsigned long long)new_count);
    if (written < 0 || (size_t)written >= sizeof(header)) {
        free(lcs);
        return CA_ERR_INVALID_ARG;
    }
    ca_dt_buffer_append(&buffer, header);

#define CA_DT_LCS_AT(i_, j_) lcs[(i_) * stride + (j_)]
    while (i < old_count || j < new_count) {
        if (i < old_count && j < new_count && ca_dt_line_equal(old_lines[i], new_lines[j])) {
            ca_dt_buffer_append_line(&buffer, ' ', old_lines[i]);
            i++;
            j++;
        } else if (j < new_count && (i == old_count || CA_DT_LCS_AT(i, j + 1u) >= CA_DT_LCS_AT(i + 1u, j))) {
            ca_dt_buffer_append_line(&buffer, '+', new_lines[j]);
            j++;
        } else if (i < old_count) {
            ca_dt_buffer_append_line(&buffer, '-', old_lines[i]);
            i++;
        }
    }
#undef CA_DT_LCS_AT

    free(lcs);
    *out_truncated = buffer.truncated;
    return CA_OK;
}

static ca_status_t ca_dt_preflight_preview(const ca_tool_call_t *call,
                                           ca_tool_result_t *result,
                                           void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_diff_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    const char *error_code = "INVALID_ARGUMENT";
    const char *error_message = "Invalid path.";
    ca_status_t status;

    ca_dt_result_reset(result, "preview_file_change");
    if (ctx == NULL || call == NULL) {
        return ca_dt_result_error(result, "INVALID_CONTEXT", "Tool context is missing.");
    }
    status = ca_dt_parse_args(call, &args, result);
    if (status != CA_OK) {
        return status;
    }
    status = ca_dt_join_workspace_path(ctx,
                                       args.path,
                                       abs_path,
                                       sizeof(abs_path),
                                       rel_path,
                                       sizeof(rel_path),
                                       &error_code,
                                       &error_message);
    free(args.new_content);
    if (status != CA_OK) {
        return ca_dt_result_error(result, error_code, error_message);
    }

    return CA_OK;
}

static ca_status_t ca_dt_execute_preview(const ca_tool_call_t *call,
                                         ca_tool_result_t *result,
                                         void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_diff_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    const char *error_code = "INVALID_ARGUMENT";
    const char *error_message = "Invalid path.";
    char *old_content = NULL;
    char *diff = NULL;
    char *escaped_path = NULL;
    char *escaped_diff = NULL;
    ca_diff_line_t *old_lines = NULL;
    ca_diff_line_t *new_lines = NULL;
    size_t old_size = 0;
    size_t old_count = 0;
    size_t new_count = 0;
    int changed;
    int truncated = 0;
    int written;
    ca_status_t status;

    ca_dt_result_reset(result, "preview_file_change");
    if (ctx == NULL || call == NULL) {
        return ca_dt_result_error(result, "INVALID_CONTEXT", "Tool context is missing.");
    }

    status = ca_dt_parse_args(call, &args, result);
    if (status != CA_OK) {
        return status;
    }
    status = ca_dt_join_workspace_path(ctx,
                                       args.path,
                                       abs_path,
                                       sizeof(abs_path),
                                       rel_path,
                                       sizeof(rel_path),
                                       &error_code,
                                       &error_message);
    if (status != CA_OK) {
        free(args.new_content);
        return ca_dt_result_error(result, error_code, error_message);
    }
    if (!ca_dt_content_is_text(args.new_content)) {
        free(args.new_content);
        return ca_dt_result_error(result, "BINARY_FILE", "new_content contains non-text control bytes.");
    }

    status = ca_dt_read_text_file(abs_path, &old_content, &old_size);
    if (status == CA_ERR_NOT_FOUND) {
        free(args.new_content);
        return ca_dt_result_error(result, "FILE_NOT_FOUND", "Target file does not exist.");
    }
    if (status == CA_ERR_INVALID_ARG) {
        free(args.new_content);
        return ca_dt_result_error(result, "FILE_TOO_LARGE", "Target file is too large for diff preview.");
    }
    if (status == CA_ERR_PERMISSION_DENIED) {
        free(args.new_content);
        return ca_dt_result_error(result, "BINARY_FILE", "Refusing to preview binary file.");
    }
    if (status == CA_ERR_NO_MEMORY) {
        free(args.new_content);
        return ca_dt_result_error(result, "OUT_OF_MEMORY", "Failed to allocate file buffer.");
    }
    if (status != CA_OK) {
        free(args.new_content);
        return ca_dt_result_error(result, "READ_FAILED", "Failed to read target file.");
    }

    changed = strcmp(old_content, args.new_content) != 0;
    status = ca_dt_split_lines(old_content, &old_lines, &old_count);
    if (status == CA_OK) {
        status = ca_dt_split_lines(args.new_content, &new_lines, &new_count);
    }
    if (status != CA_OK) {
        free(old_lines);
        free(new_lines);
        free(old_content);
        free(args.new_content);
        return ca_dt_result_error(result, "OUT_OF_MEMORY", "Failed to split file content into lines.");
    }

    diff = (char *)malloc(CA_DT_MAX_DIFF_BYTES + 1u);
    escaped_path = (char *)malloc(CA_PROJECT_PATH_CAP * 2u);
    escaped_diff = (char *)malloc((CA_DT_MAX_DIFF_BYTES * 2u) + 1u);
    if (diff == NULL || escaped_path == NULL || escaped_diff == NULL) {
        free(diff);
        free(escaped_path);
        free(escaped_diff);
        free(old_lines);
        free(new_lines);
        free(old_content);
        free(args.new_content);
        return ca_dt_result_error(result, "OUT_OF_MEMORY", "Failed to allocate diff buffers.");
    }

    diff[0] = '\0';
    if (changed) {
        status = ca_dt_generate_diff(rel_path, old_lines, old_count, new_lines, new_count, diff, CA_DT_MAX_DIFF_BYTES + 1u, &truncated);
        if (status == CA_ERR_NO_MEMORY) {
            free(diff);
            free(escaped_path);
            free(escaped_diff);
            free(old_lines);
            free(new_lines);
            free(old_content);
            free(args.new_content);
            return ca_dt_result_error(result, "OUT_OF_MEMORY", "Failed to allocate LCS table.");
        }
        if (status != CA_OK) {
            free(diff);
            free(escaped_path);
            free(escaped_diff);
            free(old_lines);
            free(new_lines);
            free(old_content);
            free(args.new_content);
            return ca_dt_result_error(result, "DIFF_TOO_LARGE", "File has too many lines for MVP diff preview.");
        }
    }

    if (ca_json_escape_string(rel_path, escaped_path, CA_PROJECT_PATH_CAP * 2u) != CA_OK ||
        ca_json_escape_string(diff, escaped_diff, (CA_DT_MAX_DIFF_BYTES * 2u) + 1u) != CA_OK) {
        free(diff);
        free(escaped_path);
        free(escaped_diff);
        free(old_lines);
        free(new_lines);
        free(old_content);
        free(args.new_content);
        return ca_dt_result_error(result, "DIFF_TOO_LARGE", "Diff output is too large to encode as JSON.");
    }

    result->success = 1;
    written = snprintf(result->result_json,
                       sizeof(result->result_json),
                       "{\"path\":\"%s\",\"mode\":\"replace\",\"changed\":%s,"
                       "\"old_size\":%llu,\"new_size\":%llu,"
                       "\"old_lines\":%llu,\"new_lines\":%llu,"
                       "\"truncated\":%s,\"diff\":\"%s\"}",
                       escaped_path,
                       changed ? "true" : "false",
                       (unsigned long long)old_size,
                       (unsigned long long)strlen(args.new_content),
                       (unsigned long long)old_count,
                       (unsigned long long)new_count,
                       truncated ? "true" : "false",
                       escaped_diff);
    free(diff);
    free(escaped_path);
    free(escaped_diff);
    free(old_lines);
    free(new_lines);
    free(old_content);
    free(args.new_content);
    if (written < 0 || (size_t)written >= sizeof(result->result_json)) {
        return ca_dt_result_error(result, "DIFF_TOO_LARGE", "Diff result JSON exceeded tool result buffer.");
    }

    return CA_OK;
}

ca_status_t ca_register_diff_tools(ca_tool_registry_t *registry)
{
    static const ca_tool_def_t preview_tool = {
        "preview_file_change",
        "Preview a unified diff for replacing a workspace text file without modifying disk.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"new_content\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\"}},\"required\":[\"path\",\"new_content\"]}",
        CA_TOOL_PERMISSION_SAFE,
        ca_dt_execute_preview,
        ca_dt_preflight_preview
    };

    if (registry == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /*
     * SAFE because this tool only reads and builds a bounded diff in memory.
     * It never opens the target in write mode and never calls write tools.
     */
    return ca_tool_registry_register(registry, &preview_tool);
}
