/*
 * project_indexer.c
 * 项目索引核心：从工作区根目录递归扫描所有文件，构建 ca_project_index_t。
 * 包含平台差异层——Windows 用 _findfirst/_findnext，Unix 用 opendir/readdir。
 * 扫描结果用于展示摘要或喂给 LLM 上下文。
 *
 * Core indexer: recursively walks workspace root, builds ca_project_index_t.
 * Platform layer: Windows → _findfirst/_findnext, Unix → opendir/readdir.
 * Result feeds the summary display and (later) the LLM context window.
 */
#include "ca_project.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#define CA_GETCWD      _getcwd
#define CA_STAT_STRUCT struct _stat
#define CA_STAT        _stat
#define CA_ISDIR(mode) (((mode) & _S_IFDIR) != 0)
#define CA_PATH_SEP    "\\"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define CA_GETCWD      getcwd
#define CA_STAT_STRUCT struct stat
#define CA_STAT        stat
#define CA_ISDIR(mode)  S_ISDIR(mode)
#define CA_PATH_SEP    "/"
#endif

/* ---- 内部工具 / internal helpers ---- */

/* strdup 的本地实现，避免依赖 POSIX / local strdup, avoids POSIX dependency */
static char *ca_project_strdup(const char *text)
{
    char *copy;
    size_t len;

    if (text == NULL) {
        return NULL;
    }

    len = strlen(text);
    copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, len + 1);
    return copy;
}

/* 拼接两个路径，中间自动加分隔符 / join two path components with separator */
static ca_status_t ca_project_join_path(char *buffer,
                                        size_t buffer_size,
                                        const char *left,
                                        const char *right)
{
    int written;

    if (buffer == NULL || buffer_size == 0 || left == NULL || right == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (left[0] == '\0') {
        written = snprintf(buffer, buffer_size, "%s", right);
    } else {
        written = snprintf(buffer, buffer_size, "%s%s%s", left, CA_PATH_SEP, right);
    }

    if (written < 0 || (size_t)written >= buffer_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

/*
 * Index-relative paths intentionally use '/' on every platform. Keeping one
 * display/logical form makes matching and summaries stable across shells.
 */
static ca_status_t ca_project_make_relative(char *buffer,
                                            size_t buffer_size,
                                            const char *parent,
                                            const char *name)
{
    int written;

    if (buffer == NULL || buffer_size == 0 || parent == NULL || name == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    if (parent[0] == '\0') {
        written = snprintf(buffer, buffer_size, "%s", name);
    } else {
        written = snprintf(buffer, buffer_size, "%s/%s", parent, name);
    }

    if (written < 0 || (size_t)written >= buffer_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

/*
 * 往索引里添加一条记录——动态扩容，超过上限的记入 skipped_count
 * Add an entry; grow array as needed; entries beyond max_entries are counted as skipped.
 */
static ca_status_t ca_project_add_entry(ca_project_index_t *index,
                                        const char *relative_path,
                                        uint64_t size_bytes,
                                        int is_directory,
                                        int ignored,
                                        ca_file_kind_t kind)
{
    ca_file_entry_t *new_entries;
    ca_file_entry_t *entry;
    size_t new_capacity;

    if (index == NULL || relative_path == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    /* 已达上限 → 跳过，只记数 / at cap → skip but count */
    if (index->entry_count >= index->max_entries) {
        index->skipped_count++;
        return CA_OK;
    }

    /* 动态扩容 / grow if full */
    if (index->entry_count == index->entry_capacity) {
        new_capacity = index->entry_capacity == 0 ? 128 : index->entry_capacity * 2;
        if (new_capacity > index->max_entries) {
            new_capacity = index->max_entries;
        }

        new_entries = (ca_file_entry_t *)realloc(index->entries, new_capacity * sizeof(*new_entries));
        if (new_entries == NULL) {
            return CA_ERR_NO_MEMORY;
        }

        index->entries = new_entries;
        index->entry_capacity = new_capacity;
    }

    entry = &index->entries[index->entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->path = ca_project_strdup(relative_path);
    if (entry->path == NULL) {
        return CA_ERR_NO_MEMORY;
    }
    entry->size_bytes = size_bytes;
    entry->is_directory = is_directory;
    entry->ignored = ignored;
    entry->kind = kind;

    index->entry_count++;
    /* 顺便更新项目类型检测 / feed the project-type detector */
    ca_project_detector_update(index, entry);
    return CA_OK;
}

/* 前向声明 / forward declaration */
static ca_status_t ca_project_scan_dir(ca_project_index_t *index,
                                       const char *absolute_dir,
                                       const char *relative_dir,
                                       unsigned int depth);

/*
 * 递归目录扫描 — Windows 版本（_findfirst / _findnext）
 * Recursive directory scanner — Windows variant.
 */
#ifdef _WIN32
static ca_status_t ca_project_scan_dir(ca_project_index_t *index,
                                       const char *absolute_dir,
                                       const char *relative_dir,
                                       unsigned int depth)
{
    char pattern[CA_PROJECT_PATH_CAP];
    char absolute_child[CA_PROJECT_PATH_CAP];
    char relative_child[CA_PROJECT_PATH_CAP];
    intptr_t handle;
    struct _finddata_t data;
    ca_status_t status;

    /* 超深度 → 跳过 / exceeded max depth */
    if (depth > index->max_depth) {
        index->skipped_count++;
        return CA_OK;
    }

    status = ca_project_join_path(pattern, sizeof(pattern), absolute_dir, "*");
    if (status != CA_OK) {
        return status;
    }

    handle = _findfirst(pattern, &data);
    if (handle == -1) {
        CA_STAT_STRUCT stat_result;
        int find_errno = errno;

        if (find_errno == ENOENT && CA_STAT(absolute_dir, &stat_result) == 0 && CA_ISDIR(stat_result.st_mode)) {
            return CA_OK;
        }

        index->skipped_count++;
        fprintf(stderr, "Warning: cannot open directory '%s': %s\n", absolute_dir, strerror(find_errno));
        return CA_OK;
    }

    do {
        int is_dir = (data.attrib & _A_SUBDIR) != 0;
        int ignored;

        if (strcmp(data.name, ".") == 0 || strcmp(data.name, "..") == 0) {
            continue;
        }

        status = ca_project_join_path(absolute_child, sizeof(absolute_child), absolute_dir, data.name);
        if (status != CA_OK) {
            _findclose(handle);
            return status;
        }
        status = ca_project_make_relative(relative_child, sizeof(relative_child), relative_dir, data.name);
        if (status != CA_OK) {
            _findclose(handle);
            return status;
        }

        ignored = ca_project_should_ignore(relative_child, data.name, is_dir);
        if (ignored) {
            index->skipped_count++;
            continue;
        }

        status = ca_project_add_entry(index,
                                      relative_child,
                                      is_dir ? 0u : (uint64_t)data.size,
                                      is_dir,
                                      0,
                                      ca_project_file_kind_from_path(relative_child, is_dir));
        if (status != CA_OK) {
            _findclose(handle);
            return status;
        }

        /* 递归进入子目录 / recurse into subdirectory */
        if (is_dir) {
            status = ca_project_scan_dir(index, absolute_child, relative_child, depth + 1);
            if (status != CA_OK) {
                _findclose(handle);
                return status;
            }
        }
    } while (_findnext(handle, &data) == 0);

    _findclose(handle);
    return CA_OK;
}

/*
 * 递归目录扫描 — Unix 版本（opendir / readdir）
 * Recursive directory scanner — Unix variant.
 */
#else
static ca_status_t ca_project_scan_dir(ca_project_index_t *index,
                                       const char *absolute_dir,
                                       const char *relative_dir,
                                       unsigned int depth)
{
    DIR *dir;
    struct dirent *entry;
    ca_status_t status;

    if (depth > index->max_depth) {
        index->skipped_count++;
        return CA_OK;
    }

    dir = opendir(absolute_dir);
    if (dir == NULL) {
        index->skipped_count++;
        fprintf(stderr, "Warning: cannot open directory '%s': %s\n", absolute_dir, strerror(errno));
        return CA_OK;
    }

    while ((entry = readdir(dir)) != NULL) {
        char absolute_child[CA_PROJECT_PATH_CAP];
        char relative_child[CA_PROJECT_PATH_CAP];
        CA_STAT_STRUCT stat_result;
        int is_dir;
        int ignored;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        status = ca_project_join_path(absolute_child, sizeof(absolute_child), absolute_dir, entry->d_name);
        if (status != CA_OK) {
            closedir(dir);
            return status;
        }
        status = ca_project_make_relative(relative_child, sizeof(relative_child), relative_dir, entry->d_name);
        if (status != CA_OK) {
            closedir(dir);
            return status;
        }

        /* stat 失败 → 跳过，记数 / can't stat → skip and count */
        if (CA_STAT(absolute_child, &stat_result) != 0) {
            index->skipped_count++;
            continue;
        }

        is_dir = CA_ISDIR(stat_result.st_mode);
        ignored = ca_project_should_ignore(relative_child, entry->d_name, is_dir);
        if (ignored) {
            index->skipped_count++;
            continue;
        }

        status = ca_project_add_entry(index,
                                      relative_child,
                                      is_dir ? 0u : (uint64_t)stat_result.st_size,
                                      is_dir,
                                      0,
                                      ca_project_file_kind_from_path(relative_child, is_dir));
        if (status != CA_OK) {
            closedir(dir);
            return status;
        }

        if (is_dir) {
            status = ca_project_scan_dir(index, absolute_child, relative_child, depth + 1);
            if (status != CA_OK) {
                closedir(dir);
                return status;
            }
        }
    }

    closedir(dir);
    return CA_OK;
}
#endif

/* ---- 公共 API / public API ---- */

ca_status_t ca_project_get_current_workspace(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    if (CA_GETCWD(buffer, buffer_size) == NULL) {
        return CA_ERR_IO;
    }

    buffer[buffer_size - 1] = '\0';
    return CA_OK;
}

ca_status_t ca_project_index_init(ca_project_index_t *index, const char *workspace_root)
{
    if (index == NULL || workspace_root == NULL || workspace_root[0] == '\0') {
        return CA_ERR_INVALID_ARG;
    }

    memset(index, 0, sizeof(*index));
    index->workspace_root = ca_project_strdup(workspace_root);
    if (index->workspace_root == NULL) {
        return CA_ERR_NO_MEMORY;
    }

    index->max_entries = CA_PROJECT_MAX_FILES;
    index->max_depth = CA_PROJECT_MAX_DEPTH;
    return CA_OK;
}

void ca_project_index_free(ca_project_index_t *index)
{
    size_t i;

    if (index == NULL) {
        return;
    }

    for (i = 0; i < index->entry_count; i++) {
        free(index->entries[i].path);
    }
    free(index->entries);
    free(index->workspace_root);

    /* 清零防止 use-after-free / zero out to catch use-after-free */
    memset(index, 0, sizeof(*index));
}

/* 开始扫描——从工作区根目录递归 / kick off recursive scan from workspace root */
ca_status_t ca_project_index_scan(ca_project_index_t *index)
{
    if (index == NULL || index->workspace_root == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    return ca_project_scan_dir(index, index->workspace_root, "", 0);
}

/* ---- 摘要展示 / summary display ---- */

/* 打印检测到的项目类型 / print detected project types */
static void ca_project_print_types(unsigned int flags, FILE *stream)
{
    int printed = 0;

    if (flags & CA_PROJECT_TYPE_C) {
        fprintf(stream, "C");
        printed = 1;
    }
    if (flags & CA_PROJECT_TYPE_CMAKE) {
        fprintf(stream, "%sCMake", printed ? ", " : "");
        printed = 1;
    }
    if (flags & CA_PROJECT_TYPE_NODE) {
        fprintf(stream, "%sNode", printed ? ", " : "");
        printed = 1;
    }
    if (flags & CA_PROJECT_TYPE_PYTHON) {
        fprintf(stream, "%sPython", printed ? ", " : "");
        printed = 1;
    }
    if (flags & CA_PROJECT_TYPE_RUST) {
        fprintf(stream, "%sRust", printed ? ", " : "");
        printed = 1;
    }
    if (!printed) {
        fprintf(stream, "Unknown");
    }
}

/* 判断一个条目是否需要高亮展示——这些是标志性文件，用户通常最关心
 * Check if an entry is "important" — landmark files users care about. */
static int ca_project_is_important(const ca_file_entry_t *entry)
{
    if (entry == NULL || entry->is_directory || entry->path == NULL) {
        return 0;
    }

    return strcmp(entry->path, "CMakeLists.txt") == 0 ||
           strcmp(entry->path, "Makefile") == 0 ||
           strcmp(entry->path, "package.json") == 0 ||
           strcmp(entry->path, "pyproject.toml") == 0 ||
           strcmp(entry->path, "Cargo.toml") == 0 ||
           strcmp(entry->path, "README.md") == 0 ||
           strcmp(entry->path, "src/main.c") == 0 ||
           strcmp(entry->path, "include/ca_config.h") == 0 ||
           strcmp(entry->path, "include/ca_project.h") == 0;
}

void ca_project_index_print_summary(const ca_project_index_t *index, FILE *stream)
{
    size_t i;
    size_t printed = 0;
    size_t file_count = 0;

    if (stream == NULL) {
        return;
    }

    if (index == NULL) {
        fprintf(stream, "Project index is unavailable.\n");
        return;
    }

    for (i = 0; i < index->entry_count; i++) {
        if (!index->entries[i].is_directory) {
            file_count++;
        }
    }

    fprintf(stream, "Workspace: %s\n", index->workspace_root != NULL ? index->workspace_root : "<unknown>");
    fprintf(stream, "Project types: ");
    ca_project_print_types(index->project_type_flags, stream);
    fprintf(stream, "\n");
    fprintf(stream, "Indexed files: %zu\n", file_count);
    fprintf(stream, "Skipped files: %zu\n", index->skipped_count);
    fprintf(stream, "\nImportant files:\n");

    /* 最多展示 12 个重要文件 / show up to 12 important files */
    for (i = 0; i < index->entry_count && printed < 12; i++) {
        if (ca_project_is_important(&index->entries[i])) {
            fprintf(stream, "%s\n", index->entries[i].path);
            printed++;
        }
    }

    if (printed == 0) {
        fprintf(stream, "<none detected>\n");
    }
}
