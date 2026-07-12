/*
 * edit_tools.c
 * Phase 10B exact string edit tool. This is intentionally narrower than
 * apply_patch: it only performs byte-exact old_string/new_string replacement.
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
#define CA_ET_PATH_SEP "\\"
#else
#define CA_ET_PATH_SEP "/"
#endif

#define CA_ET_MAX_FILE_BYTES   CA_MAX_FILE_READ_SIZE
#define CA_ET_MAX_ARG_BYTES    CA_MAX_PATCH_SIZE
#define CA_ET_MAX_DIFF_BYTES   CA_MAX_DIFF_OUTPUT
#define CA_ET_SAMPLE_BYTES     4096u

typedef struct ca_edit_args {
    char path[CA_PROJECT_PATH_CAP];
    char *old_string;
    char *new_string;
    int replace_all;
} ca_edit_args_t;

typedef struct ca_edit_buffer {
    char *data;
    size_t len;
    size_t cap;
    int truncated;
} ca_edit_buffer_t;

static ca_status_t ca_et_copy(char *dest, size_t dest_size, const char *src)
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

static void ca_et_result_reset(ca_tool_result_t *result, const char *tool_name)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    if (tool_name != NULL) {
        (void)ca_et_copy(result->tool_name, sizeof(result->tool_name), tool_name);
    }
}

static ca_status_t ca_et_result_error(ca_tool_result_t *result,
                                      const char *code,
                                      const char *message)
{
    if (result == NULL || code == NULL || message == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    ca_et_result_reset(result, "edit_file");
    result->success = 0;
    (void)ca_et_copy(result->error_code, sizeof(result->error_code), code);
    (void)ca_et_copy(result->error_message, sizeof(result->error_message), message);
    return CA_ERR_TOOL_FAILED;
}

static int ca_et_segment_equals(const char *start, size_t len, const char *value)
{
    return value != NULL && strlen(value) == len && strncmp(start, value, len) == 0;
}

static int ca_et_is_absolute_path(const char *path)
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

static int ca_et_forbidden_segment(const char *path, const char **out_code, const char **out_message)
{
    const char *segment = path;
    const char *cursor;

    if (path == NULL) {
        return 0;
    }

    for (cursor = path; ; cursor++) {
        if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
            size_t len = (size_t)(cursor - segment);

            if (ca_et_segment_equals(segment, len, "..")) {
                *out_code = "PATH_OUTSIDE_WORKSPACE";
                *out_message = "Path must not contain '..'.";
                return 1;
            }
            if (ca_et_segment_equals(segment, len, ".git")) {
                *out_code = "PROTECTED_PATH";
                *out_message = "Refusing to edit inside .git.";
                return 1;
            }
            if (ca_et_segment_equals(segment, len, "build")) {
                *out_code = "PROTECTED_PATH";
                *out_message = "Refusing to edit inside build.";
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

static ca_status_t ca_et_join_workspace_path(const ca_tool_context_t *ctx,
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
    if (ca_et_is_absolute_path(input_path)) {
        *out_code = "PATH_OUTSIDE_WORKSPACE";
        *out_message = "Absolute paths are not allowed.";
        return CA_ERR_PERMISSION_DENIED;
    }
    if (ca_et_forbidden_segment(input_path, out_code, out_message)) {
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
    if (ca_et_forbidden_segment(rel, out_code, out_message)) {
        return CA_ERR_PERMISSION_DENIED;
    }

    written = snprintf(out_rel, out_rel_size, "%s", rel);
    if (written < 0 || (size_t)written >= out_rel_size) {
        *out_code = "INVALID_ARGUMENT";
        *out_message = "Path is too long.";
        return CA_ERR_INVALID_ARG;
    }
    written = snprintf(out_abs, out_abs_size, "%s%s%s", ctx->workspace_root, CA_ET_PATH_SEP, rel);
    if (written < 0 || (size_t)written >= out_abs_size) {
        *out_code = "INVALID_ARGUMENT";
        *out_message = "Path is too long.";
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static int ca_et_content_is_text(const char *content)
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

static ca_status_t ca_et_read_text_file(const char *abs_path, char **out_text, size_t *out_size)
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
    if ((unsigned long)file_size > CA_ET_MAX_FILE_BYTES) {
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

    sample_len = got < CA_ET_SAMPLE_BYTES ? got : CA_ET_SAMPLE_BYTES;
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

static void ca_et_args_free(ca_edit_args_t *args)
{
    if (args == NULL) {
        return;
    }
    free(args->old_string);
    free(args->new_string);
    memset(args, 0, sizeof(*args));
}

static ca_status_t ca_et_parse_args(const ca_tool_call_t *call, ca_edit_args_t *args, ca_tool_result_t *result)
{
    ca_status_t status;

    if (call == NULL || args == NULL || result == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(args, 0, sizeof(*args));
    status = ca_json_get_string(call->arguments_json, "path", args->path, sizeof(args->path));
    if (status == CA_ERR_NOT_FOUND) {
        return ca_et_result_error(result, "MISSING_ARGUMENT", "edit_file requires path.");
    }
    if (status != CA_OK || args->path[0] == '\0') {
        return ca_et_result_error(result, "INVALID_ARGUMENT", "path must be a non-empty string.");
    }

    args->old_string = (char *)malloc(CA_ET_MAX_ARG_BYTES + 1u);
    args->new_string = (char *)malloc(CA_ET_MAX_ARG_BYTES + 1u);
    if (args->old_string == NULL || args->new_string == NULL) {
        ca_et_args_free(args);
        return ca_et_result_error(result, "OUT_OF_MEMORY", "Failed to allocate edit argument buffers.");
    }

    status = ca_json_get_string(call->arguments_json, "old_string", args->old_string, CA_ET_MAX_ARG_BYTES + 1u);
    if (status == CA_ERR_NOT_FOUND) {
        ca_et_args_free(args);
        return ca_et_result_error(result, "MISSING_ARGUMENT", "edit_file requires old_string.");
    }
    if (status != CA_OK) {
        ca_et_args_free(args);
        return ca_et_result_error(result, "INVALID_ARGUMENT", "old_string must be a string within size limits.");
    }
    if (args->old_string[0] == '\0') {
        ca_et_args_free(args);
        return ca_et_result_error(result, "INVALID_ARGUMENT", "old_string must not be empty.");
    }

    status = ca_json_get_string(call->arguments_json, "new_string", args->new_string, CA_ET_MAX_ARG_BYTES + 1u);
    if (status == CA_ERR_NOT_FOUND) {
        ca_et_args_free(args);
        return ca_et_result_error(result, "MISSING_ARGUMENT", "edit_file requires new_string.");
    }
    if (status != CA_OK) {
        ca_et_args_free(args);
        return ca_et_result_error(result, "INVALID_ARGUMENT", "new_string must be a string within size limits.");
    }
    if (!ca_et_content_is_text(args->old_string) || !ca_et_content_is_text(args->new_string)) {
        ca_et_args_free(args);
        return ca_et_result_error(result, "BINARY_FILE", "edit_file only accepts text strings.");
    }

    status = ca_json_get_bool(call->arguments_json, "replace_all", &args->replace_all);
    if (status == CA_ERR_NOT_FOUND) {
        args->replace_all = 0;
    } else if (status != CA_OK) {
        ca_et_args_free(args);
        return ca_et_result_error(result, "INVALID_ARGUMENT", "replace_all must be a boolean.");
    }

    return CA_OK;
}

static size_t ca_et_count_occurrences(const char *content, const char *needle, const char **first_match)
{
    const char *cursor;
    size_t count = 0;
    size_t needle_len;

    if (first_match != NULL) {
        *first_match = NULL;
    }
    if (content == NULL || needle == NULL || needle[0] == '\0') {
        return 0;
    }

    cursor = content;
    needle_len = strlen(needle);
    while ((cursor = strstr(cursor, needle)) != NULL) {
        if (first_match != NULL && *first_match == NULL) {
            *first_match = cursor;
        }
        count++;
        cursor += needle_len;
    }

    return count;
}

static size_t ca_et_line_number_at(const char *content, const char *position)
{
    const char *cursor;
    size_t line = 1;

    if (content == NULL || position == NULL || position < content) {
        return 1;
    }

    for (cursor = content; cursor < position && *cursor != '\0'; cursor++) {
        if (*cursor == '\n') {
            line++;
        }
    }

    return line;
}

static size_t ca_et_count_text_lines(const char *text)
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

static ca_status_t ca_et_validate_match(const char *content,
                                        const ca_edit_args_t *args,
                                        size_t *out_replacements,
                                        const char **out_first_match,
                                        ca_tool_result_t *result)
{
    size_t matches;

    if (content == NULL || args == NULL || out_replacements == NULL || out_first_match == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    matches = ca_et_count_occurrences(content, args->old_string, out_first_match);
    if (matches == 0) {
        return ca_et_result_error(result, "OLD_STRING_NOT_FOUND", "old_string was not found in the target file.");
    }
    if (!args->replace_all && matches > 1) {
        /*
         * Exact replacement is safe only when the target is unambiguous.
         * replace_all=true is the explicit opt-in for repeated matches.
         */
        return ca_et_result_error(result, "OLD_STRING_NOT_UNIQUE", "old_string appears more than once; set replace_all=true.");
    }

    *out_replacements = args->replace_all ? matches : 1u;
    return CA_OK;
}

static ca_status_t ca_et_validate_read_status(ca_status_t status, ca_tool_result_t *result)
{
    if (status == CA_OK) {
        return CA_OK;
    }
    if (status == CA_ERR_NOT_FOUND) {
        return ca_et_result_error(result, "FILE_NOT_FOUND", "Target file does not exist.");
    }
    if (status == CA_ERR_INVALID_ARG) {
        return ca_et_result_error(result, "FILE_TOO_LARGE", "Target file is too large to edit.");
    }
    if (status == CA_ERR_PERMISSION_DENIED) {
        return ca_et_result_error(result, "BINARY_FILE", "Refusing to edit binary file.");
    }
    if (status == CA_ERR_NO_MEMORY) {
        return ca_et_result_error(result, "OUT_OF_MEMORY", "Failed to allocate file buffer.");
    }
    return ca_et_result_error(result, "READ_FAILED", "Failed to read target file.");
}

static ca_status_t ca_et_build_new_content(const char *old_content,
                                           const ca_edit_args_t *args,
                                           size_t replacements,
                                           char **out_new_content,
                                           size_t *out_new_size)
{
    const char *cursor;
    char *new_content;
    size_t old_len;
    size_t needle_len;
    size_t replacement_len;
    size_t new_size;
    size_t used = 0;
    size_t done = 0;

    if (old_content == NULL || args == NULL || out_new_content == NULL || out_new_size == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    *out_new_content = NULL;
    *out_new_size = 0;
    old_len = strlen(old_content);
    needle_len = strlen(args->old_string);
    replacement_len = strlen(args->new_string);
    if (replacement_len > needle_len &&
        replacements > (CA_ET_MAX_FILE_BYTES - old_len) / (replacement_len - needle_len)) {
        return CA_ERR_INVALID_ARG;
    }
    new_size = old_len - (needle_len * replacements) + (replacement_len * replacements);
    if (new_size > CA_ET_MAX_FILE_BYTES) {
        return CA_ERR_INVALID_ARG;
    }

    new_content = (char *)malloc(new_size + 1u);
    if (new_content == NULL) {
        return CA_ERR_NO_MEMORY;
    }

    cursor = old_content;
    while (done < replacements) {
        const char *match = strstr(cursor, args->old_string);
        size_t prefix_len;

        if (match == NULL) {
            free(new_content);
            return CA_ERR_INVALID_ARG;
        }

        prefix_len = (size_t)(match - cursor);
        memcpy(new_content + used, cursor, prefix_len);
        used += prefix_len;
        memcpy(new_content + used, args->new_string, replacement_len);
        used += replacement_len;
        cursor = match + needle_len;
        done++;
    }

    old_len = strlen(cursor);
    memcpy(new_content + used, cursor, old_len);
    used += old_len;
    new_content[used] = '\0';

    *out_new_content = new_content;
    *out_new_size = used;
    return CA_OK;
}

static void ca_et_buffer_init(ca_edit_buffer_t *buffer, char *data, size_t cap)
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

static void ca_et_buffer_append_mem(ca_edit_buffer_t *buffer, const char *text, size_t len)
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

static void ca_et_buffer_append(ca_edit_buffer_t *buffer, const char *text)
{
    if (text != NULL) {
        ca_et_buffer_append_mem(buffer, text, strlen(text));
    }
}

static void ca_et_buffer_append_piece_lines(ca_edit_buffer_t *buffer, char prefix, const char *text)
{
    const char *start;
    const char *cursor;

    if (buffer == NULL || text == NULL || text[0] == '\0') {
        return;
    }

    start = text;
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '\n') {
            ca_et_buffer_append_mem(buffer, &prefix, 1);
            ca_et_buffer_append_mem(buffer, start, (size_t)(cursor - start + 1));
            start = cursor + 1;
        }
    }
    if (*start != '\0') {
        ca_et_buffer_append_mem(buffer, &prefix, 1);
        ca_et_buffer_append(buffer, start);
        ca_et_buffer_append(buffer, "\n");
    }
}

static ca_status_t ca_et_generate_diff(const char *rel_path,
                                       const char *old_content,
                                       const ca_edit_args_t *args,
                                       char *diff,
                                       size_t diff_size,
                                       int *out_truncated)
{
    ca_edit_buffer_t buffer;
    const char *cursor;
    size_t needle_len;
    size_t replacements_left;
    char header[CA_PROJECT_PATH_CAP * 2 + 128];
    int written;

    if (rel_path == NULL || old_content == NULL || args == NULL || diff == NULL || out_truncated == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    ca_et_buffer_init(&buffer, diff, diff_size);
    written = snprintf(header, sizeof(header), "--- %s\n+++ %s\n", rel_path, rel_path);
    if (written < 0 || (size_t)written >= sizeof(header)) {
        return CA_ERR_INVALID_ARG;
    }
    ca_et_buffer_append(&buffer, header);

    /*
     * MVP diff is focused on the exact replacement snippets, not full patch
     * context. It is enough for human preview and can be replaced before the
     * later apply_patch phase with a shared context-aware diff engine.
     */
    cursor = old_content;
    needle_len = strlen(args->old_string);
    replacements_left = args->replace_all ? (size_t)-1 : 1u;
    while (replacements_left > 0) {
        const char *match = strstr(cursor, args->old_string);
        size_t line;
        size_t old_lines;
        size_t new_lines;

        if (match == NULL) {
            break;
        }
        line = ca_et_line_number_at(old_content, match);
        old_lines = ca_et_count_text_lines(args->old_string);
        new_lines = ca_et_count_text_lines(args->new_string);
        written = snprintf(header,
                           sizeof(header),
                           "@@ -%llu,%llu +%llu,%llu @@\n",
                           (unsigned long long)line,
                           (unsigned long long)old_lines,
                           (unsigned long long)line,
                           (unsigned long long)new_lines);
        if (written < 0 || (size_t)written >= sizeof(header)) {
            return CA_ERR_INVALID_ARG;
        }
        ca_et_buffer_append(&buffer, header);
        ca_et_buffer_append_piece_lines(&buffer, '-', args->old_string);
        ca_et_buffer_append_piece_lines(&buffer, '+', args->new_string);
        cursor = match + needle_len;
        if (replacements_left != (size_t)-1) {
            replacements_left--;
        }
    }

    *out_truncated = buffer.truncated;
    return CA_OK;
}

static ca_status_t ca_et_write_temp_then_replace(const char *abs_path, const char *content, size_t content_len)
{
    char temp_path[CA_PROJECT_PATH_CAP + 32];
    FILE *file;
    size_t wrote;
    int written;

    if (abs_path == NULL || content == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(temp_path, sizeof(temp_path), "%s.cagent_tmp", abs_path);
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
     * POSIX rename can replace an existing file. Some Windows C runtimes
     * cannot, so this MVP falls back to remove+rename. A later editing phase
     * can introduce a platform-specific atomic replace helper.
     */
    if (rename(temp_path, abs_path) != 0) {
        if (remove(abs_path) != 0 || rename(temp_path, abs_path) != 0) {
            (void)remove(temp_path);
            return CA_ERR_IO;
        }
    }

    return CA_OK;
}

static ca_status_t ca_et_preflight_edit_file(const ca_tool_call_t *call,
                                             ca_tool_result_t *result,
                                             void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_edit_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    const char *error_code = "INVALID_ARGUMENT";
    const char *error_message = "Invalid path.";
    char *old_content = NULL;
    size_t old_size = 0;
    size_t replacements = 0;
    const char *first_match = NULL;
    ca_status_t status;

    ca_et_result_reset(result, "edit_file");
    if (ctx == NULL || call == NULL) {
        return ca_et_result_error(result, "INVALID_CONTEXT", "Tool context is missing.");
    }

    status = ca_et_parse_args(call, &args, result);
    if (status != CA_OK) {
        return status;
    }
    status = ca_et_join_workspace_path(ctx,
                                       args.path,
                                       abs_path,
                                       sizeof(abs_path),
                                       rel_path,
                                       sizeof(rel_path),
                                       &error_code,
                                       &error_message);
    if (status != CA_OK) {
        ca_et_args_free(&args);
        return ca_et_result_error(result, error_code, error_message);
    }

    /*
     * read-before-edit is a runtime safety rule, not just a prompt hint.
     * It prevents stale or invented old_string edits from reaching the
     * permission prompt. apply_patch should reuse this same check later.
     */
    if (ctx->edit_tracking == NULL) {
        ca_et_args_free(&args);
        return ca_et_result_error(result, "READ_REQUIRED", "read_file or read_file_range must succeed before edit_file.");
    }
    status = ca_edit_tracking_check_fresh(ctx->edit_tracking, ctx->workspace_root, rel_path);
    if (status == CA_ERR_NOT_FOUND) {
        ca_et_args_free(&args);
        return ca_et_result_error(result, "READ_REQUIRED", "read_file or read_file_range must succeed before edit_file.");
    }
    if (status == CA_ERR_PERMISSION_DENIED) {
        ca_et_args_free(&args);
        return ca_et_result_error(result, "FILE_CHANGED_SINCE_READ", "Target file changed after it was last read.");
    }
    if (status != CA_OK) {
        ca_et_args_free(&args);
        return ca_et_result_error(result, "TRACKING_FAILED", "Failed to verify read-before-edit state.");
    }

    status = ca_et_read_text_file(abs_path, &old_content, &old_size);
    if (status != CA_OK) {
        ca_et_args_free(&args);
        return ca_et_validate_read_status(status, result);
    }
    (void)old_size;

    /*
     * Preflight catches stale or ambiguous edits before the permission prompt,
     * but execute repeats this because the file can change while the user is
     * deciding whether to allow the write.
     */
    status = ca_et_validate_match(old_content, &args, &replacements, &first_match, result);
    free(old_content);
    ca_et_args_free(&args);
    (void)replacements;
    (void)first_match;
    return status;
}

static ca_status_t ca_et_execute_edit_file(const ca_tool_call_t *call,
                                           ca_tool_result_t *result,
                                           void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_edit_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    const char *error_code = "INVALID_ARGUMENT";
    const char *error_message = "Invalid path.";
    char *old_content = NULL;
    char *new_content = NULL;
    char *diff = NULL;
    ca_payload_t json;
    size_t old_size = 0;
    size_t new_size = 0;
    size_t replacements = 0;
    const char *first_match = NULL;
    int changed;
    int diff_truncated = 0;
    ca_status_t status;

    ca_et_result_reset(result, "edit_file");
    if (ctx == NULL || call == NULL) {
        return ca_et_result_error(result, "INVALID_CONTEXT", "Tool context is missing.");
    }

    status = ca_et_parse_args(call, &args, result);
    if (status != CA_OK) {
        return status;
    }
    status = ca_et_join_workspace_path(ctx,
                                       args.path,
                                       abs_path,
                                       sizeof(abs_path),
                                       rel_path,
                                       sizeof(rel_path),
                                       &error_code,
                                       &error_message);
    if (status != CA_OK) {
        ca_et_args_free(&args);
        return ca_et_result_error(result, error_code, error_message);
    }

    if (ctx->edit_tracking == NULL) {
        ca_et_args_free(&args);
        return ca_et_result_error(result, "READ_REQUIRED", "read_file or read_file_range must succeed before edit_file.");
    }
    status = ca_edit_tracking_check_fresh(ctx->edit_tracking, ctx->workspace_root, rel_path);
    if (status == CA_ERR_NOT_FOUND) {
        ca_et_args_free(&args);
        return ca_et_result_error(result, "READ_REQUIRED", "read_file or read_file_range must succeed before edit_file.");
    }
    if (status == CA_ERR_PERMISSION_DENIED) {
        ca_et_args_free(&args);
        return ca_et_result_error(result, "FILE_CHANGED_SINCE_READ", "Target file changed after it was last read.");
    }
    if (status != CA_OK) {
        ca_et_args_free(&args);
        return ca_et_result_error(result, "TRACKING_FAILED", "Failed to verify read-before-edit state.");
    }

    status = ca_et_read_text_file(abs_path, &old_content, &old_size);
    if (status != CA_OK) {
        ca_et_args_free(&args);
        return ca_et_validate_read_status(status, result);
    }
    status = ca_et_validate_match(old_content, &args, &replacements, &first_match, result);
    (void)first_match;
    if (status != CA_OK) {
        free(old_content);
        ca_et_args_free(&args);
        return status;
    }

    changed = strcmp(args.old_string, args.new_string) != 0;
    if (!changed) {
        replacements = 0;
        new_size = old_size;
        new_content = old_content;
        old_content = NULL;
    } else {
        status = ca_et_build_new_content(old_content, &args, replacements, &new_content, &new_size);
        if (status == CA_ERR_NO_MEMORY) {
            free(old_content);
            ca_et_args_free(&args);
            return ca_et_result_error(result, "OUT_OF_MEMORY", "Failed to allocate edited content.");
        }
        if (status != CA_OK) {
            free(old_content);
            ca_et_args_free(&args);
            return ca_et_result_error(result, "FILE_TOO_LARGE", "Edited file would exceed size limits.");
        }
    }

    diff = (char *)malloc(CA_ET_MAX_DIFF_BYTES + 1u);
    if (diff == NULL) {
        free(diff);
        free(old_content);
        free(new_content);
        ca_et_args_free(&args);
        return ca_et_result_error(result, "OUT_OF_MEMORY", "Failed to allocate edit result buffers.");
    }

    diff[0] = '\0';
    if (changed) {
        status = ca_et_generate_diff(rel_path, old_content != NULL ? old_content : new_content, &args, diff, CA_ET_MAX_DIFF_BYTES + 1u, &diff_truncated);
        if (status != CA_OK) {
            free(diff);
            free(old_content);
            free(new_content);
            ca_et_args_free(&args);
            return ca_et_result_error(result, "DIFF_TOO_LARGE", "Diff output is too large.");
        }
        status = ca_et_write_temp_then_replace(abs_path, new_content, new_size);
        if (status != CA_OK) {
            free(diff);
            free(old_content);
            free(new_content);
            ca_et_args_free(&args);
            return ca_et_result_error(result, "WRITE_FAILED", "Failed to write edited file.");
        }
        if (ca_edit_tracking_note_write(ctx->edit_tracking, ctx->workspace_root, rel_path) != CA_OK) {
            free(diff);
            free(old_content);
            free(new_content);
            ca_et_args_free(&args);
            return ca_et_result_error(result, "TRACKING_FAILED", "Failed to update edit tracking after write.");
        }
    }

    status = ca_payload_init(&json, CA_MAX_TOOL_RESULT_INLINE, CA_MAX_TOOL_RESULT_TOTAL);
    if (status != CA_OK) {
        free(diff);
        free(old_content);
        free(new_content);
        ca_et_args_free(&args);
        return ca_et_result_error(result, "OUT_OF_MEMORY", "Failed to initialize edit result payload.");
    }

    result->success = 1;
    status = ca_payload_append_cstr(&json, "{\"path\":\"");
    if (status == CA_OK) {
        status = ca_payload_append_json_escaped(&json, rel_path, strlen(rel_path));
    }
    if (status == CA_OK) {
        status = ca_payload_appendf(&json,
                                    "\",\"replacements\":%llu,\"bytes_written\":%llu,"
                                    "\"old_size\":%llu,\"new_size\":%llu,\"changed\":%s,"
                                    "\"diff\":\"",
                                    (unsigned long long)replacements,
                                    (unsigned long long)(changed ? new_size : 0u),
                                    (unsigned long long)old_size,
                                    (unsigned long long)new_size,
                                    changed ? "true" : "false");
    }
    if (status == CA_OK) {
        status = ca_payload_append_json_escaped(&json, diff, strlen(diff));
    }
    if (ca_payload_truncated(&json)) {
        diff_truncated = 1;
    }
    if (status == CA_OK) {
        status = ca_payload_appendf(&json, "\",\"diff_truncated\":%s}", diff_truncated ? "true" : "false");
    }
    if (status == CA_OK && ca_payload_len(&json) < sizeof(result->result_json)) {
        (void)snprintf(result->result_json, sizeof(result->result_json), "%s", ca_payload_data(&json));
    }
    free(diff);
    ca_payload_free(&json);
    free(old_content);
    free(new_content);
    ca_et_args_free(&args);
    if (status != CA_OK) {
        return ca_et_result_error(result, "OUT_OF_MEMORY", "Failed to build edit result JSON.");
    }
    if (result->result_json[0] == '\0') {
        return ca_et_result_error(result, "DIFF_TOO_LARGE", "Edit result JSON exceeded tool result buffer.");
    }

    return CA_OK;
}

ca_status_t ca_register_edit_tools(ca_tool_registry_t *registry)
{
    static const ca_tool_def_t edit_tool = {
        /* edit_file / exact string file edit */
        "edit_file",
        "Replace an exact string in a workspace text file.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        CA_TOOL_PERMISSION_ASK,
        ca_et_execute_edit_file,
        ca_et_preflight_edit_file
    };

    if (registry == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    return ca_tool_registry_register(registry, &edit_tool);
}
