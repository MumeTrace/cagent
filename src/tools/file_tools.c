/*
 * file_tools.c
 * Phase 5A read-only file tools. These tools are deliberately SAFE-only:
 * they inspect workspace metadata or text content, and never create, modify,
 * delete, or execute anything.
 */
#include "ca_file_tools.h"
#include "ca_json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define CA_FT_FIND_DATA       struct _finddata_t
#define CA_FT_FINDFIRST       _findfirst
#define CA_FT_FINDNEXT        _findnext
#define CA_FT_FINDCLOSE       _findclose
#define CA_FT_FIND_HANDLE     intptr_t
#define CA_FT_FIND_INVALID    ((intptr_t)-1)
#define CA_FT_IS_DIR(data)    (((data).attrib & _A_SUBDIR) != 0)
#define CA_FT_FILE_SIZE(data) ((uint64_t)((data).size))
#define CA_FT_FILE_NAME(data) ((data).name)
#define CA_FT_PATH_SEP        "\\"
#else
#include <dirent.h>
#include <sys/stat.h>
#define CA_FT_PATH_SEP        "/"
#endif

#define CA_FT_DEFAULT_MAX_RESULTS  20
#define CA_FT_MAX_RESULTS          100
#define CA_FT_LIST_MAX_RESULTS     200
#define CA_FT_READ_BYTES_LIMIT     6000
#define CA_FT_SEARCH_FILE_LIMIT    1048576u
#define CA_FT_LINE_CAP             4096

typedef struct ca_json_writer {
    char *buffer;
    size_t capacity;
    size_t used;
    int overflow;
} ca_json_writer_t;

typedef struct ca_file_tool_args {
    char path[CA_PROJECT_PATH_CAP];
    char query[256];
    int start_line;
    int end_line;
    int max_results;
} ca_file_tool_args_t;

static ca_status_t ca_ft_copy(char *dest, size_t dest_size, const char *src)
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

static void ca_ft_result_reset(ca_tool_result_t *result, const char *tool_name)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    if (tool_name != NULL) {
        (void)ca_ft_copy(result->tool_name, sizeof(result->tool_name), tool_name);
    }
}

static ca_status_t ca_ft_result_error(ca_tool_result_t *result,
                                      const char *tool_name,
                                      const char *code,
                                      const char *message)
{
    if (result == NULL || code == NULL || message == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    ca_ft_result_reset(result, tool_name);
    result->success = 0;
    (void)ca_ft_copy(result->error_code, sizeof(result->error_code), code);
    (void)ca_ft_copy(result->error_message, sizeof(result->error_message), message);
    return CA_ERR_TOOL_FAILED;
}

static ca_status_t ca_ft_result_success(ca_tool_result_t *result, const char *tool_name)
{
    if (result == NULL || tool_name == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    result->success = 1;
    return ca_ft_copy(result->tool_name, sizeof(result->tool_name), tool_name);
}

static void ca_json_init(ca_json_writer_t *writer, char *buffer, size_t capacity)
{
    if (writer == NULL) {
        return;
    }

    writer->buffer = buffer;
    writer->capacity = capacity;
    writer->used = 0;
    writer->overflow = 0;

    if (buffer != NULL && capacity > 0) {
        buffer[0] = '\0';
    }
}

static void ca_json_appendf(ca_json_writer_t *writer, const char *format, ...)
{
    va_list args;
    int written;

    if (writer == NULL || writer->buffer == NULL || writer->capacity == 0 || format == NULL || writer->overflow) {
        return;
    }

    if (writer->used >= writer->capacity) {
        writer->overflow = 1;
        return;
    }

    va_start(args, format);
    written = vsnprintf(writer->buffer + writer->used, writer->capacity - writer->used, format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= writer->capacity - writer->used) {
        writer->overflow = 1;
        writer->buffer[writer->capacity - 1] = '\0';
        return;
    }

    writer->used += (size_t)written;
}

static void ca_json_append_string(ca_json_writer_t *writer, const char *text)
{
    size_t escaped_len;

    if (writer == NULL || text == NULL || writer->overflow) {
        return;
    }

    ca_json_appendf(writer, "\"");
    if (writer->overflow) {
        return;
    }

    if (writer->used >= writer->capacity ||
        ca_json_escape_string(text, writer->buffer + writer->used, writer->capacity - writer->used) != CA_OK) {
        writer->overflow = 1;
        return;
    }
    escaped_len = strlen(writer->buffer + writer->used);
    writer->used += escaped_len;
    ca_json_appendf(writer, "\"");
}

static void ca_ft_args_init(ca_file_tool_args_t *args)
{
    if (args == NULL) {
        return;
    }

    memset(args, 0, sizeof(*args));
    args->max_results = CA_FT_DEFAULT_MAX_RESULTS;
}

static int ca_ft_is_absolute_path(const char *path)
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

static int ca_ft_has_parent_segment(const char *path)
{
    const char *segment = path;
    const char *cursor;

    if (path == NULL) {
        return 0;
    }

    for (cursor = path; ; cursor++) {
        if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
            size_t len = (size_t)(cursor - segment);
            if (len == 2 && segment[0] == '.' && segment[1] == '.') {
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

static ca_status_t ca_ft_join_workspace_path(const ca_tool_context_t *ctx,
                                             const char *input_path,
                                             char *out_abs_path,
                                             size_t out_abs_size,
                                             char *out_rel_path,
                                             size_t out_rel_size)
{
    const char *rel;
    int written;

    if (ctx == NULL || ctx->workspace_root == NULL || input_path == NULL ||
        out_abs_path == NULL || out_abs_size == 0 || out_rel_path == NULL || out_rel_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    if (input_path[0] == '\0') {
        return CA_ERR_INVALID_ARG;
    }

    /*
     * Phase 5A is conservative: accept workspace-relative paths only. Full
     * absolute-path normalization can come with the later sandbox hardening.
     */
    if (ca_ft_is_absolute_path(input_path) || ca_ft_has_parent_segment(input_path)) {
        return CA_ERR_PERMISSION_DENIED;
    }

    rel = input_path;
    while (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) {
        rel += 2;
    }
    if (strcmp(input_path, ".") == 0 || rel[0] == '\0') {
        rel = ".";
    }

    written = snprintf(out_rel_path, out_rel_size, "%s", rel);
    if (written < 0 || (size_t)written >= out_rel_size) {
        return CA_ERR_INVALID_ARG;
    }

    if (strcmp(rel, ".") == 0) {
        written = snprintf(out_abs_path, out_abs_size, "%s", ctx->workspace_root);
    } else {
        written = snprintf(out_abs_path, out_abs_size, "%s%s%s", ctx->workspace_root, CA_FT_PATH_SEP, rel);
    }

    if (written < 0 || (size_t)written >= out_abs_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static const char *ca_ft_basename(const char *path)
{
    const char *cursor;
    const char *base;

    if (path == NULL) {
        return "";
    }

    base = path;
    for (cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }

    return base;
}

static int ca_ft_ext_is_binary(const char *path)
{
    static const char *const binary_exts[] = {
        ".exe", ".dll", ".lib", ".obj", ".o", ".pdb", ".ilk",
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".webp",
        ".zip", ".7z", ".rar", ".gz", ".tar", ".pdf", ".mp3", ".mp4",
        NULL
    };
    const char *dot = NULL;
    const char *cursor;
    size_t i;

    if (path == NULL) {
        return 0;
    }

    for (cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
            dot = NULL;
        } else if (*cursor == '.') {
            dot = cursor;
        }
    }

    if (dot == NULL) {
        return 0;
    }

    for (i = 0; binary_exts[i] != NULL; i++) {
        if (strcmp(dot, binary_exts[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

static ca_status_t ca_ft_probe_text_file(const char *absolute_path, const char *display_path)
{
    unsigned char sample[4096];
    size_t got;
    size_t i;
    FILE *file;

    if (absolute_path == NULL || display_path == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (ca_ft_ext_is_binary(display_path)) {
        return CA_ERR_PERMISSION_DENIED;
    }

    file = fopen(absolute_path, "rb");
    if (file == NULL) {
        return CA_ERR_IO;
    }

    got = fread(sample, 1, sizeof(sample), file);
    if (ferror(file)) {
        fclose(file);
        return CA_ERR_IO;
    }
    fclose(file);

    for (i = 0; i < got; i++) {
        if (sample[i] == '\0') {
            return CA_ERR_PERMISSION_DENIED;
        }
    }

    return CA_OK;
}

static uint64_t ca_ft_file_size(FILE *file)
{
    long current;
    long end;

    if (file == NULL) {
        return 0u;
    }

    current = ftell(file);
    if (current < 0) {
        return 0u;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        return 0u;
    }
    end = ftell(file);
    (void)fseek(file, current, SEEK_SET);

    return end < 0 ? 0u : (uint64_t)end;
}

static void ca_ft_append_project_types(ca_json_writer_t *json, unsigned int flags)
{
    int printed = 0;

    ca_json_appendf(json, "[");
#define CA_FT_APPEND_TYPE(flag, name)                          \
    do {                                                       \
        if ((flags) & (flag)) {                                \
            ca_json_appendf(json, "%s", printed ? "," : ""); \
            ca_json_append_string(json, (name));               \
            printed = 1;                                       \
        }                                                      \
    } while (0)

    CA_FT_APPEND_TYPE(CA_PROJECT_TYPE_C, "C");
    CA_FT_APPEND_TYPE(CA_PROJECT_TYPE_CMAKE, "CMake");
    CA_FT_APPEND_TYPE(CA_PROJECT_TYPE_NODE, "Node");
    CA_FT_APPEND_TYPE(CA_PROJECT_TYPE_PYTHON, "Python");
    CA_FT_APPEND_TYPE(CA_PROJECT_TYPE_RUST, "Rust");
#undef CA_FT_APPEND_TYPE

    if (!printed) {
        ca_json_append_string(json, "Unknown");
    }
    ca_json_appendf(json, "]");
}

static ca_status_t ca_ft_execute_get_project_summary(const ca_tool_call_t *call,
                                                     ca_tool_result_t *result,
                                                     void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    const ca_project_index_t *index;
    ca_json_writer_t json;
    size_t i;
    size_t files = 0;
    size_t dirs = 0;

    (void)call;
    ca_ft_result_reset(result, "get_project_summary");
    if (ctx == NULL || ctx->project_index == NULL) {
        return ca_ft_result_error(result, "get_project_summary", "CONTEXT_UNAVAILABLE", "Project index is unavailable.");
    }

    index = ctx->project_index;
    for (i = 0; i < index->entry_count; i++) {
        if (index->entries[i].is_directory) {
            dirs++;
        } else {
            files++;
        }
    }

    ca_json_init(&json, result->result_json, sizeof(result->result_json));
    ca_json_appendf(&json, "{\"workspace\":");
    ca_json_append_string(&json, index->workspace_root != NULL ? index->workspace_root : "");
    ca_json_appendf(&json, ",\"project_types\":");
    ca_ft_append_project_types(&json, index->project_type_flags);
    ca_json_appendf(&json, ",\"indexed_files\":%zu,\"indexed_directories\":%zu,\"skipped_files\":%zu}",
                    files, dirs, index->skipped_count);

    if (json.overflow) {
        return ca_ft_result_error(result, "get_project_summary", "RESULT_TOO_LARGE", "Project summary exceeded result buffer.");
    }

    return ca_ft_result_success(result, "get_project_summary");
}

static ca_status_t ca_ft_execute_list_directory(const ca_tool_call_t *call,
                                                ca_tool_result_t *result,
                                                void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_file_tool_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    ca_json_writer_t json;
    int count = 0;
    int max_results;
    ca_status_t status;

    ca_ft_result_reset(result, "list_directory");
    if (call == NULL || ctx == NULL) {
        return ca_ft_result_error(result, "list_directory", "INVALID_CONTEXT", "Tool context is missing.");
    }

    ca_ft_args_init(&args);
    status = ca_json_get_string(call->arguments_json, "path", args.path, sizeof(args.path));
    if (status == CA_ERR_NOT_FOUND) {
        (void)ca_ft_copy(args.path, sizeof(args.path), ".");
    } else if (status != CA_OK) {
        return ca_ft_result_error(result, "list_directory", "INVALID_ARGUMENTS", "Invalid path argument.");
    }
    status = ca_json_get_int(call->arguments_json, "max_results", &args.max_results);
    if (status == CA_ERR_NOT_FOUND) {
        args.max_results = CA_FT_LIST_MAX_RESULTS;
        status = CA_OK;
    }
    if (status != CA_OK || args.max_results <= 0) {
        return ca_ft_result_error(result, "list_directory", "INVALID_ARGUMENTS", "Invalid max_results argument.");
    }
    max_results = args.max_results > CA_FT_LIST_MAX_RESULTS ? CA_FT_LIST_MAX_RESULTS : args.max_results;

    status = ca_ft_join_workspace_path(ctx, args.path, abs_path, sizeof(abs_path), rel_path, sizeof(rel_path));
    if (status == CA_ERR_PERMISSION_DENIED) {
        return ca_ft_result_error(result, "list_directory", "PATH_OUTSIDE_WORKSPACE", "Path is outside the workspace.");
    }
    if (status != CA_OK) {
        return ca_ft_result_error(result, "list_directory", "INVALID_PATH", "Path is invalid.");
    }

    ca_json_init(&json, result->result_json, sizeof(result->result_json));
    ca_json_appendf(&json, "{\"path\":");
    ca_json_append_string(&json, rel_path);
    ca_json_appendf(&json, ",\"entries\":[");

#ifdef _WIN32
    {
        char pattern[CA_PROJECT_PATH_CAP];
        CA_FT_FIND_DATA data;
        CA_FT_FIND_HANDLE handle;
        int written = snprintf(pattern, sizeof(pattern), "%s%s*", abs_path, CA_FT_PATH_SEP);
        int first = 1;

        if (written < 0 || (size_t)written >= sizeof(pattern)) {
            return ca_ft_result_error(result, "list_directory", "INVALID_PATH", "Directory path is too long.");
        }

        handle = CA_FT_FINDFIRST(pattern, &data);
        if (handle == CA_FT_FIND_INVALID) {
            return ca_ft_result_error(result, "list_directory", "DIRECTORY_OPEN_FAILED", "Directory cannot be opened.");
        }

        do {
            const char *name = CA_FT_FILE_NAME(data);
            int is_dir = CA_FT_IS_DIR(data);
            char child_rel[CA_PROJECT_PATH_CAP];
            int child_written;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                continue;
            }
            if (count >= max_results) {
                break;
            }

            if (strcmp(rel_path, ".") == 0) {
                child_written = snprintf(child_rel, sizeof(child_rel), "%s", name);
            } else {
                child_written = snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_path, name);
            }
            if (child_written < 0 || (size_t)child_written >= sizeof(child_rel)) {
                continue;
            }
            if (ca_project_should_ignore(child_rel, name, is_dir)) {
                continue;
            }

            ca_json_appendf(&json, "%s{\"name\":", first ? "" : ",");
            ca_json_append_string(&json, name);
            ca_json_appendf(&json, ",\"path\":");
            ca_json_append_string(&json, child_rel);
            ca_json_appendf(&json, ",\"type\":");
            ca_json_append_string(&json, is_dir ? "dir" : "file");
            if (!is_dir) {
                ca_json_appendf(&json, ",\"size\":%llu", (unsigned long long)CA_FT_FILE_SIZE(data));
            }
            ca_json_appendf(&json, "}");
            first = 0;
            count++;
        } while (CA_FT_FINDNEXT(handle, &data) == 0 && !json.overflow);

        CA_FT_FINDCLOSE(handle);
    }
#else
    {
        DIR *dir = opendir(abs_path);
        struct dirent *entry;
        int first = 1;

        if (dir == NULL) {
            return ca_ft_result_error(result, "list_directory", "DIRECTORY_OPEN_FAILED", "Directory cannot be opened.");
        }

        while ((entry = readdir(dir)) != NULL && !json.overflow) {
            char child_abs[CA_PROJECT_PATH_CAP];
            char child_rel[CA_PROJECT_PATH_CAP];
            struct stat stat_result;
            int written;
            int is_dir;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (count >= max_results) {
                break;
            }

            written = snprintf(child_abs, sizeof(child_abs), "%s/%s", abs_path, entry->d_name);
            if (written < 0 || (size_t)written >= sizeof(child_abs)) {
                continue;
            }
            if (stat(child_abs, &stat_result) != 0) {
                continue;
            }
            is_dir = S_ISDIR(stat_result.st_mode);
            if (strcmp(rel_path, ".") == 0) {
                written = snprintf(child_rel, sizeof(child_rel), "%s", entry->d_name);
            } else {
                written = snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_path, entry->d_name);
            }
            if (written < 0 || (size_t)written >= sizeof(child_rel)) {
                continue;
            }
            if (ca_project_should_ignore(child_rel, entry->d_name, is_dir)) {
                continue;
            }

            ca_json_appendf(&json, "%s{\"name\":", first ? "" : ",");
            ca_json_append_string(&json, entry->d_name);
            ca_json_appendf(&json, ",\"path\":");
            ca_json_append_string(&json, child_rel);
            ca_json_appendf(&json, ",\"type\":");
            ca_json_append_string(&json, is_dir ? "dir" : "file");
            if (!is_dir) {
                ca_json_appendf(&json, ",\"size\":%llu", (unsigned long long)stat_result.st_size);
            }
            ca_json_appendf(&json, "}");
            first = 0;
            count++;
        }

        closedir(dir);
    }
#endif

    ca_json_appendf(&json, "],\"truncated\":%s}", count >= max_results ? "true" : "false");
    if (json.overflow) {
        return ca_ft_result_error(result, "list_directory", "RESULT_TOO_LARGE", "Directory result exceeded result buffer.");
    }

    return ca_ft_result_success(result, "list_directory");
}

static ca_status_t ca_ft_execute_read_file(const ca_tool_call_t *call, ca_tool_result_t *result, void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_file_tool_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    char content[CA_FT_READ_BYTES_LIMIT + 1];
    FILE *file;
    uint64_t size;
    size_t got;
    ca_json_writer_t json;
    ca_status_t status;

    ca_ft_result_reset(result, "read_file");
    if (call == NULL || ctx == NULL) {
        return ca_ft_result_error(result, "read_file", "INVALID_CONTEXT", "Tool context is missing.");
    }

    ca_ft_args_init(&args);
    status = ca_json_get_string(call->arguments_json, "path", args.path, sizeof(args.path));
    if (status != CA_OK || args.path[0] == '\0') {
        return ca_ft_result_error(result, "read_file", "INVALID_ARGUMENTS", "read_file requires a non-empty path.");
    }

    status = ca_ft_join_workspace_path(ctx, args.path, abs_path, sizeof(abs_path), rel_path, sizeof(rel_path));
    if (status == CA_ERR_PERMISSION_DENIED) {
        return ca_ft_result_error(result, "read_file", "PATH_OUTSIDE_WORKSPACE", "Path is outside the workspace.");
    }
    if (status != CA_OK) {
        return ca_ft_result_error(result, "read_file", "INVALID_PATH", "Path is invalid.");
    }

    status = ca_ft_probe_text_file(abs_path, rel_path);
    if (status == CA_ERR_PERMISSION_DENIED) {
        return ca_ft_result_error(result, "read_file", "BINARY_FILE", "Refusing to read binary file content.");
    }
    if (status != CA_OK) {
        return ca_ft_result_error(result, "read_file", "FILE_OPEN_FAILED", "File cannot be opened.");
    }

    file = fopen(abs_path, "rb");
    if (file == NULL) {
        return ca_ft_result_error(result, "read_file", "FILE_OPEN_FAILED", "File cannot be opened.");
    }

    size = ca_ft_file_size(file);
    got = fread(content, 1, CA_FT_READ_BYTES_LIMIT, file);
    if (ferror(file)) {
        fclose(file);
        return ca_ft_result_error(result, "read_file", "FILE_READ_FAILED", "File cannot be read.");
    }
    fclose(file);
    content[got] = '\0';

    ca_json_init(&json, result->result_json, sizeof(result->result_json));
    ca_json_appendf(&json, "{\"path\":");
    ca_json_append_string(&json, rel_path);
    ca_json_appendf(&json, ",\"size\":%llu,\"content\":", (unsigned long long)size);
    ca_json_append_string(&json, content);
    ca_json_appendf(&json, ",\"truncated\":%s}", got < size ? "true" : "false");

    if (json.overflow) {
        return ca_ft_result_error(result, "read_file", "RESULT_TOO_LARGE", "File content exceeded result buffer.");
    }

    return ca_ft_result_success(result, "read_file");
}

static ca_status_t ca_ft_execute_read_file_range(const ca_tool_call_t *call,
                                                 ca_tool_result_t *result,
                                                 void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    ca_file_tool_args_t args;
    char abs_path[CA_PROJECT_PATH_CAP];
    char rel_path[CA_PROJECT_PATH_CAP];
    char line[CA_FT_LINE_CAP];
    FILE *file;
    int current_line = 0;
    int first_line = 1;
    int truncated = 0;
    ca_json_writer_t json;
    ca_status_t status;

    ca_ft_result_reset(result, "read_file_range");
    if (call == NULL || ctx == NULL) {
        return ca_ft_result_error(result, "read_file_range", "INVALID_CONTEXT", "Tool context is missing.");
    }

    ca_ft_args_init(&args);
    status = ca_json_get_string(call->arguments_json, "path", args.path, sizeof(args.path));
    if (status != CA_OK || args.path[0] == '\0') {
        return ca_ft_result_error(result, "read_file_range", "INVALID_ARGUMENTS", "read_file_range requires a non-empty path.");
    }
    if (ca_json_get_int(call->arguments_json, "start_line", &args.start_line) != CA_OK ||
        ca_json_get_int(call->arguments_json, "end_line", &args.end_line) != CA_OK ||
        args.start_line <= 0 || args.end_line <= 0 || args.end_line < args.start_line) {
        return ca_ft_result_error(result, "read_file_range", "INVALID_ARGUMENTS", "Invalid line range.");
    }

    status = ca_ft_join_workspace_path(ctx, args.path, abs_path, sizeof(abs_path), rel_path, sizeof(rel_path));
    if (status == CA_ERR_PERMISSION_DENIED) {
        return ca_ft_result_error(result, "read_file_range", "PATH_OUTSIDE_WORKSPACE", "Path is outside the workspace.");
    }
    if (status != CA_OK) {
        return ca_ft_result_error(result, "read_file_range", "INVALID_PATH", "Path is invalid.");
    }

    status = ca_ft_probe_text_file(abs_path, rel_path);
    if (status == CA_ERR_PERMISSION_DENIED) {
        return ca_ft_result_error(result, "read_file_range", "BINARY_FILE", "Refusing to read binary file content.");
    }
    if (status != CA_OK) {
        return ca_ft_result_error(result, "read_file_range", "FILE_OPEN_FAILED", "File cannot be opened.");
    }

    file = fopen(abs_path, "rb");
    if (file == NULL) {
        return ca_ft_result_error(result, "read_file_range", "FILE_OPEN_FAILED", "File cannot be opened.");
    }

    ca_json_init(&json, result->result_json, sizeof(result->result_json));
    ca_json_appendf(&json, "{\"path\":");
    ca_json_append_string(&json, rel_path);
    ca_json_appendf(&json, ",\"start_line\":%d,\"end_line\":%d,\"lines\":[", args.start_line, args.end_line);

    while (fgets(line, sizeof(line), file) != NULL) {
        current_line++;
        if (current_line < args.start_line) {
            continue;
        }
        if (current_line > args.end_line) {
            break;
        }

        ca_json_appendf(&json, "%s{\"line\":%d,\"text\":", first_line ? "" : ",", current_line);
        ca_json_append_string(&json, line);
        ca_json_appendf(&json, "}");
        first_line = 0;
        if (json.overflow) {
            truncated = 1;
            break;
        }
    }

    if (ferror(file)) {
        fclose(file);
        return ca_ft_result_error(result, "read_file_range", "FILE_READ_FAILED", "File cannot be read.");
    }
    fclose(file);

    ca_json_appendf(&json, "],\"truncated\":%s}", truncated ? "true" : "false");
    if (json.overflow) {
        return ca_ft_result_error(result, "read_file_range", "RESULT_TOO_LARGE", "Line range exceeded result buffer.");
    }

    return ca_ft_result_success(result, "read_file_range");
}

static ca_status_t ca_ft_execute_search_files(const ca_tool_call_t *call,
                                              ca_tool_result_t *result,
                                              void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    const ca_project_index_t *index;
    ca_file_tool_args_t args;
    ca_json_writer_t json;
    size_t i;
    int count = 0;
    int max_results;
    ca_status_t status;

    ca_ft_result_reset(result, "search_files");
    if (call == NULL || ctx == NULL || ctx->project_index == NULL) {
        return ca_ft_result_error(result, "search_files", "INVALID_CONTEXT", "Project index is unavailable.");
    }

    ca_ft_args_init(&args);
    status = ca_json_get_string(call->arguments_json, "query", args.query, sizeof(args.query));
    if (status != CA_OK || args.query[0] == '\0') {
        return ca_ft_result_error(result, "search_files", "INVALID_ARGUMENTS", "search_files requires a non-empty query.");
    }
    status = ca_json_get_int(call->arguments_json, "max_results", &args.max_results);
    if (status == CA_ERR_NOT_FOUND) {
        args.max_results = CA_FT_DEFAULT_MAX_RESULTS;
        status = CA_OK;
    }
    if (status != CA_OK || args.max_results <= 0) {
        return ca_ft_result_error(result, "search_files", "INVALID_ARGUMENTS", "Invalid max_results argument.");
    }
    max_results = args.max_results > CA_FT_MAX_RESULTS ? CA_FT_MAX_RESULTS : args.max_results;

    index = ctx->project_index;
    ca_json_init(&json, result->result_json, sizeof(result->result_json));
    ca_json_appendf(&json, "{\"query\":");
    ca_json_append_string(&json, args.query);
    ca_json_appendf(&json, ",\"matches\":[");

    for (i = 0; i < index->entry_count && count < max_results && !json.overflow; i++) {
        const ca_file_entry_t *entry = &index->entries[i];
        if (entry->is_directory || entry->ignored || entry->path == NULL) {
            continue;
        }
        if (strstr(entry->path, args.query) == NULL && strstr(ca_ft_basename(entry->path), args.query) == NULL) {
            continue;
        }

        ca_json_appendf(&json, "%s{\"path\":", count == 0 ? "" : ",");
        ca_json_append_string(&json, entry->path);
        ca_json_appendf(&json, ",\"kind\":");
        ca_json_append_string(&json, ca_file_kind_to_string(entry->kind));
        ca_json_appendf(&json, ",\"size\":%llu}", (unsigned long long)entry->size_bytes);
        count++;
    }

    ca_json_appendf(&json, "],\"truncated\":%s}", count >= max_results ? "true" : "false");
    if (json.overflow) {
        return ca_ft_result_error(result, "search_files", "RESULT_TOO_LARGE", "Search result exceeded result buffer.");
    }

    return ca_ft_result_success(result, "search_files");
}

static ca_status_t ca_ft_execute_search_code(const ca_tool_call_t *call,
                                             ca_tool_result_t *result,
                                             void *ctx_value)
{
    const ca_tool_context_t *ctx = (const ca_tool_context_t *)ctx_value;
    const ca_project_index_t *index;
    ca_file_tool_args_t args;
    ca_json_writer_t json;
    size_t i;
    int count = 0;
    int max_results;
    ca_status_t status;

    ca_ft_result_reset(result, "search_code");
    if (call == NULL || ctx == NULL || ctx->project_index == NULL) {
        return ca_ft_result_error(result, "search_code", "INVALID_CONTEXT", "Project index is unavailable.");
    }

    ca_ft_args_init(&args);
    status = ca_json_get_string(call->arguments_json, "query", args.query, sizeof(args.query));
    if (status != CA_OK || args.query[0] == '\0') {
        return ca_ft_result_error(result, "search_code", "INVALID_ARGUMENTS", "search_code requires a non-empty query.");
    }
    status = ca_json_get_int(call->arguments_json, "max_results", &args.max_results);
    if (status == CA_ERR_NOT_FOUND) {
        args.max_results = CA_FT_DEFAULT_MAX_RESULTS;
        status = CA_OK;
    }
    if (status != CA_OK || args.max_results <= 0) {
        return ca_ft_result_error(result, "search_code", "INVALID_ARGUMENTS", "Invalid max_results argument.");
    }
    max_results = args.max_results > CA_FT_MAX_RESULTS ? CA_FT_MAX_RESULTS : args.max_results;

    index = ctx->project_index;
    ca_json_init(&json, result->result_json, sizeof(result->result_json));
    ca_json_appendf(&json, "{\"query\":");
    ca_json_append_string(&json, args.query);
    ca_json_appendf(&json, ",\"matches\":[");

    for (i = 0; i < index->entry_count && count < max_results && !json.overflow; i++) {
        const ca_file_entry_t *entry = &index->entries[i];
        char abs_path[CA_PROJECT_PATH_CAP];
        char rel_path[CA_PROJECT_PATH_CAP];
        char line[CA_FT_LINE_CAP];
        FILE *file;
        int line_number = 0;
        int binary = 0;

        if (entry->is_directory || entry->ignored || entry->path == NULL ||
            entry->size_bytes > CA_FT_SEARCH_FILE_LIMIT || ca_ft_ext_is_binary(entry->path)) {
            continue;
        }

        status = ca_ft_join_workspace_path(ctx, entry->path, abs_path, sizeof(abs_path), rel_path, sizeof(rel_path));
        if (status != CA_OK || ca_ft_probe_text_file(abs_path, rel_path) != CA_OK) {
            continue;
        }

        file = fopen(abs_path, "rb");
        if (file == NULL) {
            continue;
        }

        while (fgets(line, sizeof(line), file) != NULL && count < max_results && !json.overflow) {
            if (memchr(line, '\0', strlen(line)) != NULL) {
                binary = 1;
                break;
            }
            line_number++;
            if (strstr(line, args.query) == NULL) {
                continue;
            }

            line[strcspn(line, "\r\n")] = '\0';
            ca_json_appendf(&json, "%s{\"path\":", count == 0 ? "" : ",");
            ca_json_append_string(&json, entry->path);
            ca_json_appendf(&json, ",\"line\":%d,\"text\":", line_number);
            ca_json_append_string(&json, line);
            ca_json_appendf(&json, "}");
            count++;
        }

        (void)binary;
        fclose(file);
    }

    ca_json_appendf(&json, "],\"truncated\":%s}", count >= max_results ? "true" : "false");
    if (json.overflow) {
        return ca_ft_result_error(result, "search_code", "RESULT_TOO_LARGE", "Search result exceeded result buffer.");
    }

    return ca_ft_result_success(result, "search_code");
}

ca_status_t ca_register_file_tools(ca_tool_registry_t *registry)
{
    static const ca_tool_def_t tools[] = {
        {
            /* get_project_summary / 获取项目摘要 */
            "get_project_summary",
            "Return a JSON summary of the current workspace project index.",
            "{\"type\":\"object\",\"properties\":{}}",
            CA_TOOL_PERMISSION_SAFE,
            ca_ft_execute_get_project_summary
        },
        {
            /* list_directory / 列出目录 */
            "list_directory",
            "List directory entries under the workspace without reading file content.",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}}}",
            CA_TOOL_PERMISSION_SAFE,
            ca_ft_execute_list_directory
        },
        {
            /* read_file / 读取文件 */
            "read_file",
            "Read a workspace text file with truncation.",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
            CA_TOOL_PERMISSION_SAFE,
            ca_ft_execute_read_file
        },
        {
            /* read_file_range / 读取文件行范围 */
            "read_file_range",
            "Read a 1-based inclusive line range from a workspace text file.",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"start_line\":{\"type\":\"integer\"},\"end_line\":{\"type\":\"integer\"}},\"required\":[\"path\",\"start_line\",\"end_line\"]}",
            CA_TOOL_PERMISSION_SAFE,
            ca_ft_execute_read_file_range
        },
        {
            /* search_files / 搜索文件路径 */
            "search_files",
            "Search indexed workspace file paths.",
            "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},\"required\":[\"query\"]}",
            CA_TOOL_PERMISSION_SAFE,
            ca_ft_execute_search_files
        },
        {
            /* search_code / 搜索代码文本 */
            "search_code",
            "Search text files in the workspace index and return matching lines.",
            "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},\"required\":[\"query\"]}",
            CA_TOOL_PERMISSION_SAFE,
            ca_ft_execute_search_code
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
