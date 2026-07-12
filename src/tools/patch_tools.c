/*
 * patch_tools.c
 * Phase 10D strict unified diff apply tool. It intentionally supports only
 * existing workspace text files and exact context matching.
 */
#include "ca_file_tools.h"
#include "ca_edit_tracking.h"
#include "ca_json.h"
#include "ca_limits.h"
#include "ca_payload.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define CA_PT_PATH_SEP "\\"
#else
#define CA_PT_PATH_SEP "/"
#endif

#define CA_PT_MAX_PATCH_BYTES  CA_MAX_PATCH_SIZE
#define CA_PT_MAX_FILE_BYTES   CA_MAX_FILE_READ_SIZE
#define CA_PT_MAX_DIFF_BYTES   CA_MAX_DIFF_OUTPUT
#define CA_PT_MAX_FILES        8u
#define CA_PT_MAX_HUNKS        64u
#define CA_PT_MAX_OPS          4096u

typedef struct ca_patch_line {
    const char *start;
    size_t len;
} ca_patch_line_t;

typedef struct ca_patch_op {
    char kind;
    ca_patch_line_t text;
} ca_patch_op_t;

typedef struct ca_patch_hunk_t {
    int old_start;
    int old_count;
    int new_start;
    int new_count;
    size_t op_start;
    size_t op_count;
} ca_patch_hunk_t;

typedef struct ca_patch_file {
    char path[CA_PROJECT_PATH_CAP];
    char abs_path[CA_PROJECT_PATH_CAP];
    ca_patch_hunk_t hunks[CA_PT_MAX_HUNKS];
    size_t hunk_count;
    ca_patch_op_t ops[CA_PT_MAX_OPS];
    size_t op_count;
    char *old_content;
    char *new_content;
    size_t old_size;
    size_t new_size;
} ca_patch_file_t;

typedef struct ca_patch_plan {
    ca_patch_file_t files[CA_PT_MAX_FILES];
    size_t file_count;
    size_t total_hunks;
    size_t total_bytes_written;
} ca_patch_plan_t;

typedef struct ca_patch_args {
    char *patch;
    int require_read;
} ca_patch_args_t;

typedef struct ca_patch_buffer_t {
    char *data;
    size_t len;
    size_t cap;
    int truncated;
} ca_patch_buffer_t;

static ca_status_t ca_pt_copy(char *dest, size_t dest_size, const char *src)
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

static void ca_pt_result_reset(ca_tool_result_t *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        (void)ca_pt_copy(result->tool_name, sizeof(result->tool_name), "apply_patch");
    }
}

static ca_status_t ca_pt_result_error(ca_tool_result_t *result, const char *code, const char *message)
{
    if (result == NULL || code == NULL || message == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    ca_pt_result_reset(result);
    result->success = 0;
    (void)ca_pt_copy(result->error_code, sizeof(result->error_code), code);
    (void)ca_pt_copy(result->error_message, sizeof(result->error_message), message);
    return CA_ERR_TOOL_FAILED;
}

static int ca_pt_starts_with(const char *text, size_t len, const char *prefix)
{
    size_t prefix_len;

    if (text == NULL || prefix == NULL) {
        return 0;
    }
    prefix_len = strlen(prefix);
    return len >= prefix_len && memcmp(text, prefix, prefix_len) == 0;
}

static int ca_pt_line_is_empty(ca_patch_line_t line)
{
    return line.len == 0 || (line.len == 1 && line.start[0] == '\n') ||
           (line.len == 2 && line.start[0] == '\r' && line.start[1] == '\n');
}

static void ca_pt_line_content(ca_patch_line_t line, const char **out_start, size_t *out_len)
{
    size_t len = line.len;

    if (out_start == NULL || out_len == NULL) {
        return;
    }
    while (len > 0 && (line.start[len - 1] == '\n' || line.start[len - 1] == '\r')) {
        len--;
    }
    *out_start = line.start;
    *out_len = len;
}

static ca_status_t ca_pt_parse_args(const ca_tool_call_t *call, ca_patch_args_t *args, ca_tool_result_t *result)
{
    ca_status_t status;

    if (call == NULL || args == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    memset(args, 0, sizeof(*args));
    args->require_read = 1;
    args->patch = (char *)malloc(CA_PT_MAX_PATCH_BYTES + 1u);
    if (args->patch == NULL) {
        return ca_pt_result_error(result, "OUT_OF_MEMORY", "Failed to allocate patch buffer.");
    }

    status = ca_json_get_string(call->arguments_json, "patch", args->patch, CA_PT_MAX_PATCH_BYTES + 1u);
    if (status == CA_ERR_NOT_FOUND) {
        free(args->patch);
        args->patch = NULL;
        return ca_pt_result_error(result, "MISSING_ARGUMENT", "apply_patch requires patch.");
    }
    if (status != CA_OK || args->patch[0] == '\0') {
        free(args->patch);
        args->patch = NULL;
        return ca_pt_result_error(result, "INVALID_ARGUMENT", "patch must be a non-empty string within size limits.");
    }
    args->require_read = 1;
    if (strstr(call->arguments_json, "\"require_read\"") != NULL) {
        status = ca_json_get_bool(call->arguments_json, "require_read", &args->require_read);
        if (status != CA_OK) {
            free(args->patch);
            args->patch = NULL;
            return ca_pt_result_error(result, "INVALID_ARGUMENT", "require_read must be a boolean.");
        }
    }
    return CA_OK;
}

static int ca_pt_is_absolute_path(const char *path)
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

static int ca_pt_segment_equals(const char *start, size_t len, const char *value)
{
    return value != NULL && strlen(value) == len && strncmp(start, value, len) == 0;
}

static int ca_pt_forbidden_segment(const char *path, const char **out_code, const char **out_message)
{
    const char *segment = path;
    const char *cursor;

    if (path == NULL) {
        return 0;
    }
    for (cursor = path; ; cursor++) {
        if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
            size_t len = (size_t)(cursor - segment);
            if (ca_pt_segment_equals(segment, len, "..")) {
                *out_code = "PATH_OUTSIDE_WORKSPACE";
                *out_message = "Path must not contain '..'.";
                return 1;
            }
            if (ca_pt_segment_equals(segment, len, ".git")) {
                *out_code = "PROTECTED_PATH";
                *out_message = "Refusing to patch inside .git.";
                return 1;
            }
            if (ca_pt_segment_equals(segment, len, "build")) {
                *out_code = "PROTECTED_PATH";
                *out_message = "Refusing to patch inside build.";
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

static ca_status_t ca_pt_normalize_patch_path(const char *raw_path,
                                              char *out_rel,
                                              size_t out_rel_size,
                                              const char **out_code,
                                              const char **out_message)
{
    const char *path = raw_path;
    int written;

    if (raw_path == NULL || out_rel == NULL || out_rel_size == 0) {
        return CA_ERR_INVALID_ARG;
    }
    while (*path != '\0' && isspace((unsigned char)*path)) {
        path++;
    }
    if (strncmp(path, "a/", 2) == 0 || strncmp(path, "b/", 2) == 0) {
        path += 2;
    }
    if (strcmp(path, "/dev/null") == 0) {
        *out_code = "UNSUPPORTED_PATCH";
        *out_message = "Creating or deleting files is not supported in this phase.";
        return CA_ERR_INVALID_ARG;
    }
    if (path[0] == '\0') {
        *out_code = "INVALID_ARGUMENT";
        *out_message = "Patch path must not be empty.";
        return CA_ERR_INVALID_ARG;
    }
    if (ca_pt_is_absolute_path(path)) {
        *out_code = "PATH_OUTSIDE_WORKSPACE";
        *out_message = "Absolute paths are not allowed.";
        return CA_ERR_PERMISSION_DENIED;
    }
    if (ca_pt_forbidden_segment(path, out_code, out_message)) {
        return CA_ERR_PERMISSION_DENIED;
    }
    while (path[0] == '.' && (path[1] == '/' || path[1] == '\\')) {
        path += 2;
    }
    if (path[0] == '\0' || strcmp(path, ".") == 0) {
        *out_code = "INVALID_ARGUMENT";
        *out_message = "Patch path must name a file.";
        return CA_ERR_INVALID_ARG;
    }
    if (ca_pt_forbidden_segment(path, out_code, out_message)) {
        return CA_ERR_PERMISSION_DENIED;
    }
    written = snprintf(out_rel, out_rel_size, "%s", path);
    if (written < 0 || (size_t)written >= out_rel_size) {
        *out_code = "INVALID_ARGUMENT";
        *out_message = "Patch path is too long.";
        return CA_ERR_INVALID_ARG;
    }
    return CA_OK;
}

static ca_status_t ca_pt_join_workspace_path(const ca_tool_context_t *ctx,
                                             const char *rel_path,
                                             char *out_abs,
                                             size_t out_abs_size)
{
    int written;

    if (ctx == NULL || ctx->workspace_root == NULL || rel_path == NULL ||
        out_abs == NULL || out_abs_size == 0) {
        return CA_ERR_INVALID_ARG;
    }
    written = snprintf(out_abs, out_abs_size, "%s%s%s", ctx->workspace_root, CA_PT_PATH_SEP, rel_path);
    if (written < 0 || (size_t)written >= out_abs_size) {
        return CA_ERR_INVALID_ARG;
    }
    return CA_OK;
}

static ca_status_t ca_pt_read_text_file(const char *abs_path, char **out_text, size_t *out_size)
{
    FILE *file;
    long file_size;
    char *text;
    size_t got;
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
    if ((unsigned long)file_size > CA_PT_MAX_FILE_BYTES) {
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
    for (i = 0; i < got; i++) {
        if (text[i] == '\0') {
            free(text);
            return CA_ERR_PERMISSION_DENIED;
        }
    }
    *out_text = text;
    *out_size = got;
    return CA_OK;
}

static size_t ca_pt_count_lines(const char *text)
{
    const char *cursor;
    size_t count = 0;

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

static ca_status_t ca_pt_split_lines(const char *text, ca_patch_line_t **out_lines, size_t *out_count)
{
    ca_patch_line_t *lines;
    const char *start;
    const char *cursor;
    size_t count;
    size_t index = 0;

    if (text == NULL || out_lines == NULL || out_count == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    count = ca_pt_count_lines(text);
    *out_lines = NULL;
    *out_count = count;
    if (count == 0) {
        return CA_OK;
    }
    lines = (ca_patch_line_t *)calloc(count, sizeof(*lines));
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

static int ca_pt_line_equal(ca_patch_line_t a, ca_patch_line_t b)
{
    return a.len == b.len && memcmp(a.start, b.start, a.len) == 0;
}

static void ca_pt_buffer_init(ca_patch_buffer_t *buffer, char *data, size_t cap)
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

static void ca_pt_buffer_append_mem(ca_patch_buffer_t *buffer, const char *text, size_t len)
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

static void ca_pt_buffer_append(ca_patch_buffer_t *buffer, const char *text)
{
    if (text != NULL) {
        ca_pt_buffer_append_mem(buffer, text, strlen(text));
    }
}

static ca_status_t ca_pt_append_line_to_content(ca_patch_buffer_t *buffer, ca_patch_line_t line)
{
    if (buffer == NULL || buffer->truncated) {
        return CA_ERR_INVALID_ARG;
    }
    ca_pt_buffer_append_mem(buffer, line.start, line.len);
    return buffer->truncated ? CA_ERR_INVALID_ARG : CA_OK;
}

static int ca_pt_parse_hunk_header(ca_patch_line_t line,
                                   int *old_start,
                                   int *old_count,
                                   int *new_start,
                                   int *new_count)
{
    const char *cursor = line.start;
    const char *end = line.start + line.len;
    char *parse_end;
    long value;

    if (!ca_pt_starts_with(line.start, line.len, "@@ -")) {
        return 0;
    }
    cursor += 4;
    value = strtol(cursor, &parse_end, 10);
    if (parse_end == cursor || value < 0) {
        return 0;
    }
    *old_start = (int)value;
    *old_count = 1;
    cursor = parse_end;
    if (cursor < end && *cursor == ',') {
        cursor++;
        value = strtol(cursor, &parse_end, 10);
        if (parse_end == cursor || value < 0) {
            return 0;
        }
        *old_count = (int)value;
        cursor = parse_end;
    }
    if (cursor >= end || *cursor != ' ') {
        return 0;
    }
    cursor++;
    if (cursor >= end || *cursor != '+') {
        return 0;
    }
    cursor++;
    value = strtol(cursor, &parse_end, 10);
    if (parse_end == cursor || value < 0) {
        return 0;
    }
    *new_start = (int)value;
    *new_count = 1;
    cursor = parse_end;
    if (cursor < end && *cursor == ',') {
        cursor++;
        value = strtol(cursor, &parse_end, 10);
        if (parse_end == cursor || value < 0) {
            return 0;
        }
        *new_count = (int)value;
        cursor = parse_end;
    }
    while (cursor < end && *cursor == ' ') {
        cursor++;
    }
    return (end - cursor) >= 2 && cursor[0] == '@' && cursor[1] == '@';
}

static ca_status_t ca_pt_parse_path_line(ca_patch_line_t line, const char *prefix, char *out_raw, size_t out_raw_size)
{
    const char *start;
    const char *end;
    size_t len;
    int written;

    if (!ca_pt_starts_with(line.start, line.len, prefix) || out_raw == NULL || out_raw_size == 0) {
        return CA_ERR_INVALID_ARG;
    }
    start = line.start + strlen(prefix);
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    end = line.start + line.len;
    while (end > start && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    len = (size_t)(end - start);
    if (len == 0 || len >= out_raw_size) {
        return CA_ERR_INVALID_ARG;
    }
    written = snprintf(out_raw, out_raw_size, "%.*s", (int)len, start);
    return written < 0 || (size_t)written >= out_raw_size ? CA_ERR_INVALID_ARG : CA_OK;
}

static ca_status_t ca_pt_parse_patch_lines(char *patch_text,
                                           ca_patch_line_t **out_lines,
                                           size_t *out_count)
{
    return ca_pt_split_lines(patch_text, out_lines, out_count);
}

static ca_status_t ca_pt_plan_add_file(ca_patch_plan_t *plan,
                                       const ca_tool_context_t *ctx,
                                       const char *path,
                                       ca_patch_file_t **out_file,
                                       ca_tool_result_t *result)
{
    ca_patch_file_t *file;

    if (plan == NULL || ctx == NULL || path == NULL || out_file == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    if (plan->file_count >= CA_PT_MAX_FILES) {
        return ca_pt_result_error(result, "TOO_MANY_PATCH_FILES", "Patch touches too many files.");
    }
    file = &plan->files[plan->file_count];
    memset(file, 0, sizeof(*file));
    if (ca_pt_copy(file->path, sizeof(file->path), path) != CA_OK ||
        ca_pt_join_workspace_path(ctx, path, file->abs_path, sizeof(file->abs_path)) != CA_OK) {
        return ca_pt_result_error(result, "INVALID_ARGUMENT", "Patch path is too long.");
    }
    plan->file_count++;
    *out_file = file;
    return CA_OK;
}

static ca_status_t ca_pt_parse_plan(char *patch_text,
                                    const ca_tool_context_t *ctx,
                                    ca_patch_plan_t *plan,
                                    ca_tool_result_t *result)
{
    ca_patch_line_t *lines = NULL;
    size_t line_count = 0;
    size_t i = 0;
    ca_patch_file_t *current_file = NULL;
    ca_patch_hunk_t *current_hunk = NULL;
    ca_status_t status;

    if (patch_text == NULL || ctx == NULL || plan == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    memset(plan, 0, sizeof(*plan));
    status = ca_pt_parse_patch_lines(patch_text, &lines, &line_count);
    if (status != CA_OK) {
        return ca_pt_result_error(result, "OUT_OF_MEMORY", "Failed to split patch into lines.");
    }

    while (i < line_count) {
        ca_patch_line_t line = lines[i];
        if (ca_pt_line_is_empty(line)) {
            i++;
            continue;
        }
        if (ca_pt_starts_with(line.start, line.len, "diff --git ") ||
            ca_pt_starts_with(line.start, line.len, "index ")) {
            i++;
            continue;
        }
        if (ca_pt_starts_with(line.start, line.len, "Binary files ") ||
            ca_pt_starts_with(line.start, line.len, "rename ") ||
            ca_pt_starts_with(line.start, line.len, "new file mode ") ||
            ca_pt_starts_with(line.start, line.len, "deleted file mode ") ||
            ca_pt_starts_with(line.start, line.len, "old mode ") ||
            ca_pt_starts_with(line.start, line.len, "new mode ")) {
            free(lines);
            return ca_pt_result_error(result, "UNSUPPORTED_PATCH", "Delete, rename, mode, or binary patches are not supported.");
        }
        if (ca_pt_starts_with(line.start, line.len, "--- ")) {
            char old_raw[CA_PROJECT_PATH_CAP];
            char new_raw[CA_PROJECT_PATH_CAP];
            char old_path[CA_PROJECT_PATH_CAP];
            char new_path[CA_PROJECT_PATH_CAP];
            const char *error_code = "INVALID_ARGUMENT";
            const char *error_message = "Invalid patch path.";

            if (i + 1 >= line_count || !ca_pt_starts_with(lines[i + 1].start, lines[i + 1].len, "+++ ")) {
                free(lines);
                return ca_pt_result_error(result, "PATCH_PARSE_ERROR", "Expected +++ path after --- path.");
            }
            if (ca_pt_parse_path_line(line, "---", old_raw, sizeof(old_raw)) != CA_OK ||
                ca_pt_parse_path_line(lines[i + 1], "+++", new_raw, sizeof(new_raw)) != CA_OK) {
                free(lines);
                return ca_pt_result_error(result, "PATCH_PARSE_ERROR", "Invalid file header path.");
            }
            status = ca_pt_normalize_patch_path(old_raw, old_path, sizeof(old_path), &error_code, &error_message);
            if (status != CA_OK) {
                free(lines);
                return ca_pt_result_error(result, error_code, error_message);
            }
            status = ca_pt_normalize_patch_path(new_raw, new_path, sizeof(new_path), &error_code, &error_message);
            if (status != CA_OK) {
                free(lines);
                return ca_pt_result_error(result, error_code, error_message);
            }
            if (strcmp(old_path, new_path) != 0) {
                free(lines);
                return ca_pt_result_error(result, "UNSUPPORTED_PATCH", "Rename patches are not supported.");
            }
            status = ca_pt_plan_add_file(plan, ctx, old_path, &current_file, result);
            if (status != CA_OK) {
                free(lines);
                return status;
            }
            current_hunk = NULL;
            i += 2;
            continue;
        }
        if (ca_pt_starts_with(line.start, line.len, "@@ ")) {
            if (current_file == NULL) {
                free(lines);
                return ca_pt_result_error(result, "PATCH_PARSE_ERROR", "Hunk appears before file header.");
            }
            if (current_file->hunk_count >= CA_PT_MAX_HUNKS) {
                free(lines);
                return ca_pt_result_error(result, "TOO_MANY_HUNKS", "Patch has too many hunks.");
            }
            current_hunk = &current_file->hunks[current_file->hunk_count++];
            memset(current_hunk, 0, sizeof(*current_hunk));
            current_hunk->op_start = current_file->op_count;
            if (!ca_pt_parse_hunk_header(line,
                                         &current_hunk->old_start,
                                         &current_hunk->old_count,
                                         &current_hunk->new_start,
                                         &current_hunk->new_count)) {
                free(lines);
                return ca_pt_result_error(result, "PATCH_PARSE_ERROR", "Invalid hunk header.");
            }
            plan->total_hunks++;
            i++;
            continue;
        }
        if (line.start[0] == ' ' || line.start[0] == '-' || line.start[0] == '+') {
            if (current_file == NULL || current_hunk == NULL) {
                free(lines);
                return ca_pt_result_error(result, "PATCH_PARSE_ERROR", "Patch line appears outside a hunk.");
            }
            if (current_file->op_count >= CA_PT_MAX_OPS) {
                free(lines);
                return ca_pt_result_error(result, "TOO_MANY_HUNKS", "Patch has too many hunk lines.");
            }
            current_file->ops[current_file->op_count].kind = line.start[0];
            current_file->ops[current_file->op_count].text.start = line.start + 1;
            current_file->ops[current_file->op_count].text.len = line.len - 1u;
            current_file->op_count++;
            current_hunk->op_count++;
            i++;
            continue;
        }
        if (ca_pt_starts_with(line.start, line.len, "\\ No newline at end of file")) {
            i++;
            continue;
        }
        free(lines);
        return ca_pt_result_error(result, "PATCH_PARSE_ERROR", "Unsupported patch line.");
    }

    free(lines);
    if (plan->file_count == 0 || plan->total_hunks == 0) {
        return ca_pt_result_error(result, "PATCH_PARSE_ERROR", "Patch contains no file hunks.");
    }
    return CA_OK;
}

static ca_status_t ca_pt_validate_read_status(ca_status_t status, ca_tool_result_t *result)
{
    if (status == CA_OK) {
        return CA_OK;
    }
    if (status == CA_ERR_NOT_FOUND) {
        return ca_pt_result_error(result, "FILE_NOT_FOUND", "Target file does not exist.");
    }
    if (status == CA_ERR_INVALID_ARG) {
        return ca_pt_result_error(result, "FILE_TOO_LARGE", "Target file is too large for patch.");
    }
    if (status == CA_ERR_PERMISSION_DENIED) {
        return ca_pt_result_error(result, "BINARY_FILE", "Refusing to patch binary file.");
    }
    if (status == CA_ERR_NO_MEMORY) {
        return ca_pt_result_error(result, "OUT_OF_MEMORY", "Failed to allocate file buffer.");
    }
    return ca_pt_result_error(result, "READ_FAILED", "Failed to read target file.");
}

static ca_status_t ca_pt_check_tracking(const ca_tool_context_t *ctx,
                                        const ca_patch_file_t *file,
                                        ca_tool_result_t *result)
{
    ca_status_t status;

    if (ctx == NULL || file == NULL || ctx->edit_tracking == NULL) {
        return ca_pt_result_error(result, "READ_REQUIRED", "read_file or read_file_range must succeed before apply_patch.");
    }
    status = ca_edit_tracking_check_fresh(ctx->edit_tracking, ctx->workspace_root, file->path);
    if (status == CA_ERR_NOT_FOUND) {
        return ca_pt_result_error(result, "READ_REQUIRED", "read_file or read_file_range must succeed before apply_patch.");
    }
    if (status == CA_ERR_PERMISSION_DENIED) {
        return ca_pt_result_error(result, "FILE_CHANGED_SINCE_READ", "Target file changed after it was last read.");
    }
    if (status != CA_OK) {
        return ca_pt_result_error(result, "TRACKING_FAILED", "Failed to verify read-before-edit state.");
    }
    return CA_OK;
}

static ca_status_t ca_pt_apply_file_hunks(ca_patch_file_t *file, ca_tool_result_t *result)
{
    ca_patch_line_t *old_lines = NULL;
    size_t old_line_count = 0;
    size_t original_index = 0;
    size_t hunk_index;
    char *new_data;
    ca_patch_buffer_t buffer;
    ca_status_t status;

    if (file == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    status = ca_pt_split_lines(file->old_content, &old_lines, &old_line_count);
    if (status != CA_OK) {
        return ca_pt_result_error(result, "OUT_OF_MEMORY", "Failed to split target file lines.");
    }
    new_data = (char *)malloc(CA_PT_MAX_FILE_BYTES + 1u);
    if (new_data == NULL) {
        free(old_lines);
        return ca_pt_result_error(result, "OUT_OF_MEMORY", "Failed to allocate patched file buffer.");
    }
    ca_pt_buffer_init(&buffer, new_data, CA_PT_MAX_FILE_BYTES + 1u);

    for (hunk_index = 0; hunk_index < file->hunk_count; hunk_index++) {
        ca_patch_hunk_t *hunk = &file->hunks[hunk_index];
        size_t target = hunk->old_start > 0 ? (size_t)(hunk->old_start - 1) : 0;
        size_t op_index;

        if (target < original_index || target > old_line_count) {
            free(old_lines);
            free(new_data);
            return ca_pt_result_error(result, "PATCH_CONTEXT_MISMATCH", "Hunk location does not match target file.");
        }
        while (original_index < target) {
            if (ca_pt_append_line_to_content(&buffer, old_lines[original_index]) != CA_OK) {
                free(old_lines);
                free(new_data);
                return ca_pt_result_error(result, "FILE_TOO_LARGE", "Patched file would exceed size limit.");
            }
            original_index++;
        }
        for (op_index = 0; op_index < hunk->op_count; op_index++) {
            ca_patch_op_t *op = &file->ops[hunk->op_start + op_index];
            if (op->kind == ' ' || op->kind == '-') {
                if (original_index >= old_line_count ||
                    !ca_pt_line_equal(old_lines[original_index], op->text)) {
                    free(old_lines);
                    free(new_data);
                    return ca_pt_result_error(result, "PATCH_CONTEXT_MISMATCH", "Patch context does not match target file.");
                }
                if (op->kind == ' ') {
                    if (ca_pt_append_line_to_content(&buffer, old_lines[original_index]) != CA_OK) {
                        free(old_lines);
                        free(new_data);
                        return ca_pt_result_error(result, "FILE_TOO_LARGE", "Patched file would exceed size limit.");
                    }
                }
                original_index++;
            } else if (op->kind == '+') {
                if (ca_pt_append_line_to_content(&buffer, op->text) != CA_OK) {
                    free(old_lines);
                    free(new_data);
                    return ca_pt_result_error(result, "FILE_TOO_LARGE", "Patched file would exceed size limit.");
                }
            }
        }
    }
    while (original_index < old_line_count) {
        if (ca_pt_append_line_to_content(&buffer, old_lines[original_index]) != CA_OK) {
            free(old_lines);
            free(new_data);
            return ca_pt_result_error(result, "FILE_TOO_LARGE", "Patched file would exceed size limit.");
        }
        original_index++;
    }

    free(old_lines);
    file->new_content = new_data;
    file->new_size = buffer.len;
    return CA_OK;
}

static void ca_pt_plan_free(ca_patch_plan_t *plan)
{
    size_t i;

    if (plan == NULL) {
        return;
    }
    for (i = 0; i < plan->file_count; i++) {
        free(plan->files[i].old_content);
        free(plan->files[i].new_content);
        plan->files[i].old_content = NULL;
        plan->files[i].new_content = NULL;
    }
}

static ca_status_t ca_pt_load_and_simulate(const ca_tool_context_t *ctx,
                                           ca_patch_plan_t *plan,
                                           int require_read,
                                           ca_tool_result_t *result)
{
    size_t i;

    if (ctx == NULL || plan == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    for (i = 0; i < plan->file_count; i++) {
        ca_patch_file_t *file = &plan->files[i];
        ca_status_t status;

        if (require_read) {
            status = ca_pt_check_tracking(ctx, file, result);
            if (status != CA_OK) {
                return status;
            }
        }
        status = ca_pt_read_text_file(file->abs_path, &file->old_content, &file->old_size);
        if (status != CA_OK) {
            return ca_pt_validate_read_status(status, result);
        }
        status = ca_pt_apply_file_hunks(file, result);
        if (status != CA_OK) {
            return status;
        }
    }
    return CA_OK;
}

static ca_status_t ca_pt_write_temp_then_replace(const char *abs_path, const char *content, size_t content_len)
{
    char temp_path[CA_PROJECT_PATH_CAP + 32];
    FILE *file;
    size_t wrote;
    int written;

    if (abs_path == NULL || content == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    written = snprintf(temp_path, sizeof(temp_path), "%s.cagent_patch_tmp", abs_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        return CA_ERR_INVALID_ARG;
    }
    file = fopen(temp_path, "wb");
    if (file == NULL) {
        return CA_ERR_IO;
    }
    wrote = fwrite(content, 1, content_len, file);
    if (wrote != content_len || ferror(file)) {
        fclose(file);
        (void)remove(temp_path);
        return CA_ERR_IO;
    }
    if (fclose(file) != 0) {
        (void)remove(temp_path);
        return CA_ERR_IO;
    }
    /*
     * Same MVP strategy as edit_file: write a temp file first, then replace.
     * Windows may need remove+rename, which is not perfectly atomic.
     */
    if (rename(temp_path, abs_path) != 0) {
        if (remove(abs_path) != 0 || rename(temp_path, abs_path) != 0) {
            (void)remove(temp_path);
            return CA_ERR_IO;
        }
    }
    return CA_OK;
}

static ca_status_t ca_pt_write_all(const ca_tool_context_t *ctx, ca_patch_plan_t *plan, ca_tool_result_t *result)
{
    size_t i;

    if (ctx == NULL || plan == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    for (i = 0; i < plan->file_count; i++) {
        ca_patch_file_t *file = &plan->files[i];
        ca_status_t status = ca_pt_write_temp_then_replace(file->abs_path, file->new_content, file->new_size);
        if (status != CA_OK) {
            return ca_pt_result_error(result, "WRITE_FAILED", "Failed to write patched file.");
        }
        plan->total_bytes_written += file->new_size;
        if (ctx->edit_tracking != NULL &&
            ca_edit_tracking_note_write(ctx->edit_tracking, ctx->workspace_root, file->path) != CA_OK) {
            return ca_pt_result_error(result, "TRACKING_FAILED", "Failed to update edit tracking after patch.");
        }
    }
    return CA_OK;
}

static ca_status_t ca_pt_build_result(ca_patch_plan_t *plan,
                                      const char *patch_text,
                                      ca_tool_result_t *result)
{
    ca_payload_t json;
    size_t i;
    int diff_truncated = 0;
    ca_status_t status;

    if (plan == NULL || patch_text == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    status = ca_payload_init(&json, CA_MAX_TOOL_RESULT_INLINE, CA_MAX_TOOL_RESULT_TOTAL);
    if (status != CA_OK) {
        return ca_pt_result_error(result, "OUT_OF_MEMORY", "Failed to initialize patch result payload.");
    }

    ca_pt_result_reset(result);
    result->success = 1;
    status = ca_payload_appendf(&json,
                                "{\"files_changed\":%llu,\"hunks_applied\":%llu,\"bytes_written\":%llu,\"files\":[",
                                (unsigned long long)plan->file_count,
                                (unsigned long long)plan->total_hunks,
                                (unsigned long long)plan->total_bytes_written);
    for (i = 0; i < plan->file_count; i++) {
        if (status == CA_OK) {
            status = ca_payload_append_cstr(&json, i == 0 ? "" : ",");
        }
        if (status == CA_OK) {
            status = ca_payload_append_cstr(&json, "{\"path\":\"");
        }
        if (status == CA_OK) {
            status = ca_payload_append_json_escaped(&json, plan->files[i].path, strlen(plan->files[i].path));
        }
        if (status == CA_OK) {
            status = ca_payload_appendf(&json,
                                        "\",\"hunks_applied\":%llu,\"old_size\":%llu,\"new_size\":%llu}",
                                        (unsigned long long)plan->files[i].hunk_count,
                                        (unsigned long long)plan->files[i].old_size,
                                        (unsigned long long)plan->files[i].new_size);
        }
    }
    if (status == CA_OK) {
        status = ca_payload_append_cstr(&json, "],\"diff\":\"");
    }
    if (status == CA_OK) {
        size_t patch_len = strlen(patch_text);
        size_t encoded_len = patch_len > CA_PT_MAX_DIFF_BYTES ? CA_PT_MAX_DIFF_BYTES : patch_len;
        if (patch_len > encoded_len) {
            diff_truncated = 1;
        }
        status = ca_payload_append_json_escaped(&json, patch_text, encoded_len);
    }
    if (ca_payload_truncated(&json)) {
        diff_truncated = 1;
    }
    if (status == CA_OK) {
        status = ca_payload_appendf(&json, "\",\"diff_truncated\":%s}", diff_truncated ? "true" : "false");
    }
    if (status != CA_OK) {
        ca_payload_free(&json);
        return ca_pt_result_error(result, "OUT_OF_MEMORY", "Failed to build patch result JSON.");
    }
    if (snprintf(result->result_json, sizeof(result->result_json), "%s", ca_payload_data(&json)) < 0 ||
        ca_payload_len(&json) >= sizeof(result->result_json)) {
        ca_payload_free(&json);
        return ca_pt_result_error(result, "PATCH_TOO_LARGE", "Patch result JSON exceeded tool result buffer.");
    }
    ca_payload_free(&json);
    return CA_OK;
}

static ca_status_t ca_pt_prepare_plan(const ca_tool_call_t *call,
                                      ca_tool_result_t *result,
                                      void *ctx_value,
                                      ca_patch_args_t *args,
                                      ca_patch_plan_t *plan)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_status_t status;

    ca_pt_result_reset(result);
    if (ctx == NULL || call == NULL || args == NULL || plan == NULL) {
        return ca_pt_result_error(result, "INVALID_CONTEXT", "Tool context is missing.");
    }
    status = ca_pt_parse_args(call, args, result);
    if (status != CA_OK) {
        return status;
    }
    /*
     * Strict unified diff keeps the C MVP predictable: preflight parses and
     * simulates every hunk in memory, and execute repeats this before writing.
     */
    status = ca_pt_parse_plan(args->patch, ctx, plan, result);
    if (status != CA_OK) {
        return status;
    }
    status = ca_pt_load_and_simulate(ctx, plan, args->require_read, result);
    return status;
}

static ca_status_t ca_pt_preflight_apply_patch(const ca_tool_call_t *call,
                                               ca_tool_result_t *result,
                                               void *ctx_value)
{
    ca_patch_args_t args;
    ca_patch_plan_t *plan;
    ca_status_t status;

    plan = (ca_patch_plan_t *)calloc(1, sizeof(*plan));
    if (plan == NULL) {
        return ca_pt_result_error(result, "OUT_OF_MEMORY", "Failed to allocate patch plan.");
    }
    memset(&args, 0, sizeof(args));
    status = ca_pt_prepare_plan(call, result, ctx_value, &args, plan);
    ca_pt_plan_free(plan);
    free(plan);
    free(args.patch);
    return status;
}

static ca_status_t ca_pt_execute_apply_patch(const ca_tool_call_t *call,
                                             ca_tool_result_t *result,
                                             void *ctx_value)
{
    ca_patch_args_t args;
    ca_patch_plan_t *plan;
    ca_status_t status;

    plan = (ca_patch_plan_t *)calloc(1, sizeof(*plan));
    if (plan == NULL) {
        return ca_pt_result_error(result, "OUT_OF_MEMORY", "Failed to allocate patch plan.");
    }
    memset(&args, 0, sizeof(args));
    status = ca_pt_prepare_plan(call, result, ctx_value, &args, plan);
    if (status == CA_OK) {
        status = ca_pt_write_all((const ca_tool_context_t *)ctx_value, plan, result);
    }
    if (status == CA_OK) {
        status = ca_pt_build_result(plan, args.patch, result);
    }
    ca_pt_plan_free(plan);
    free(plan);
    free(args.patch);
    return status;
}

ca_status_t ca_register_patch_tools(ca_tool_registry_t *registry)
{
    static const ca_tool_def_t patch_tool = {
        "apply_patch",
        "Apply a strict unified diff patch to workspace text files.",
        "{\"type\":\"object\",\"properties\":{\"patch\":{\"type\":\"string\"},\"require_read\":{\"type\":\"boolean\"}},\"required\":[\"patch\"]}",
        CA_TOOL_PERMISSION_ASK,
        ca_pt_execute_apply_patch,
        ca_pt_preflight_apply_patch
    };

    if (registry == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    return ca_tool_registry_register(registry, &patch_tool);
}
