/*
 * ignore_rules.c
 * 文件/目录忽略规则：决定一个路径是否应该跳过不索引。
 * 规则包括：常见的版本控制目录、构建输出、node_modules、二进制/媒体文件扩展名等。
 *
 * File/directory ignore rules: decide whether a path should be skipped during indexing.
 * Covers: common VCS dirs, build output, node_modules, binary/media extensions, etc.
 */
#include "ca_project.h"

#include <string.h>

/* 检查一个字符串是否等于列表中的任一项，列表以 NULL 结尾
 * Case-sensitive exact match against a NULL-terminated string list. */
static int ca_string_equals_any(const char *value, const char *const *items)
{
    size_t i;

    if (value == NULL || items == NULL) {
        return 0;
    }

    for (i = 0; items[i] != NULL; i++) {
        if (strcmp(value, items[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

/*
 * 提取文件扩展名（含 .），取最后一个 . 之后的部分。
 * 路径分隔符 / \ 会重置 dot 指针——确保 "dir.ext/file" 返回 "" 而不是 ".ext/file"。
 *
 * Extract file extension starting with '.', from the last dot.
 * Path separators reset the dot — "dir.ext/file" → "" not ".ext/file".
 */
static const char *ca_path_extension(const char *path)
{
    const char *dot = NULL;
    const char *cursor;

    if (path == NULL) {
        return "";
    }

    for (cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
            dot = NULL;
        } else if (*cursor == '.') {
            dot = cursor;
        }
    }

    return dot == NULL ? "" : dot;
}

int ca_project_should_ignore(const char *relative_path, const char *name, int is_directory)
{
    /* 需要忽略的目录名 / directory names to skip */
    static const char *const ignored_dirs[] = {
        ".git",
        "build",
        "out",
        "dist",
        "node_modules",
        "target",
        ".venv",
        "venv",
        "__pycache__",
        ".cagent",
        ".cache",
        ".idea",
        ".vscode",
        NULL
    };

    /* 需要忽略的文件扩展名（二进制/媒体/归档）/ file extensions to skip */
    static const char *const ignored_exts[] = {
        ".exe", ".dll", ".so", ".dylib",
        ".o", ".obj",
        ".class", ".jar",
        ".zip", ".7z", ".rar",
        ".png", ".jpg", ".jpeg", ".gif",
        ".mp4", ".mp3",
        NULL
    };
    const char *ext;

    (void)relative_path;

    /* 空名字直接跳过 / empty name → skip */
    if (name == NULL || name[0] == '\0') {
        return 1;
    }

    /* 跳过 . 和 .. / skip self & parent */
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 1;
    }

    /* 命中忽略目录列表 / match ignored dir list */
    if (is_directory && ca_string_equals_any(name, ignored_dirs)) {
        return 1;
    }

    /* macOS 垃圾文件 / macOS cruft */
    if (!is_directory && strcmp(name, ".DS_Store") == 0) {
        return 1;
    }

    /* 命中忽略扩展名列表 / match ignored ext list */
    ext = ca_path_extension(name);
    if (!is_directory && ca_string_equals_any(ext, ignored_exts)) {
        return 1;
    }

    return 0;
}
