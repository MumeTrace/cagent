/*
 * project_detector.c
 * 文件类型判定 + 项目类型检测：根据扩展名/文件名判断一个文件是什么类型，
 * 同时根据关键文件判断整个项目是什么技术栈（C / Node / Python / Rust）。
 *
 * File kind classification + project-type detection: classify a single file by extension,
 * and infer the project's tech stack from the presence of key files.
 */
#include "ca_project.h"

#include <string.h>

/* 提取文件名（去掉路径前缀）/ extract basename from path */
static const char *ca_basename(const char *path)
{
    const char *base = path;
    const char *cursor;

    if (path == NULL) {
        return "";
    }

    for (cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }

    return base;
}

/* 提取扩展名（含 .），基于 basename / extract extension from basename */
static const char *ca_extension(const char *path)
{
    const char *base = ca_basename(path);
    const char *dot = strrchr(base, '.');

    return dot == NULL ? "" : dot;
}

/*
 * 检查路径是否包含测试相关标记——路径里有 "test"、"tests/" 就认为是测试文件
 * Check if path contains test markers — "test", "Test", "tests/" → test file.
 */
static int ca_path_contains_test_marker(const char *path)
{
    if (path == NULL) {
        return 0;
    }

    return strstr(path, "test") != NULL || strstr(path, "Test") != NULL ||
           strstr(path, "tests/") != NULL || strstr(path, "tests\\") != NULL;
}

/*
 * 根据路径和是否目录，判定文件类型
 * 测试文件标记优先级最高——避免把 test_foo.c 归类为普通 source
 *
 * Classify a file by path. Test marker takes priority to avoid
 * mis-classifying test_foo.c as ordinary source.
 */
ca_file_kind_t ca_project_file_kind_from_path(const char *relative_path, int is_directory)
{
    const char *base;
    const char *ext;

    if (relative_path == NULL) {
        return CA_FILE_KIND_UNKNOWN;
    }

    if (is_directory) {
        return CA_FILE_KIND_UNKNOWN;
    }

    base = ca_basename(relative_path);
    ext = ca_extension(relative_path);

    /* 测试标记优先 / test marker first */
    if (ca_path_contains_test_marker(relative_path)) {
        return CA_FILE_KIND_TEST;
    }
    /* 源码文件 / source files */
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".cc") == 0 ||
        strcmp(ext, ".cpp") == 0 || strcmp(ext, ".rs") == 0 ||
        strcmp(ext, ".py") == 0 || strcmp(ext, ".js") == 0 ||
        strcmp(ext, ".ts") == 0) {
        return CA_FILE_KIND_SOURCE;
    }
    /* 头文件 / header files */
    if (strcmp(ext, ".h") == 0 || strcmp(ext, ".hpp") == 0) {
        return CA_FILE_KIND_HEADER;
    }
    /* 文档 / documentation */
    if (strcmp(ext, ".md") == 0 || strcmp(ext, ".txt") == 0) {
        return CA_FILE_KIND_DOC;
    }
    /* 配置文件 / config files */
    if (strcmp(base, "CMakeLists.txt") == 0 || strcmp(base, "Makefile") == 0 ||
        strcmp(base, "package.json") == 0 || strcmp(base, "pyproject.toml") == 0 ||
        strcmp(base, "Cargo.toml") == 0 || strcmp(ext, ".json") == 0 ||
        strcmp(ext, ".toml") == 0 || strcmp(ext, ".yaml") == 0 ||
        strcmp(ext, ".yml") == 0) {
        return CA_FILE_KIND_CONFIG;
    }

    return CA_FILE_KIND_UNKNOWN;
}

/*
 * 每遇到一个文件，检测器就检查它是不是"关键文件"，
 * 如果是就设置对应的 project_type_flags。
 * 位掩码支持多类型共存（一个项目可以同时是 C + CMake + Python）。
 *
 * For each file encountered, check if it's a "key file" and set
 * the corresponding project_type_flags. Bitmask allows coexistence
 * (e.g., C + CMake + Python in one project).
 */
void ca_project_detector_update(ca_project_index_t *index, const ca_file_entry_t *entry)
{
    const char *base;
    const char *ext;

    if (index == NULL || entry == NULL || entry->is_directory || entry->path == NULL) {
        return;
    }

    base = ca_basename(entry->path);
    ext = ca_extension(entry->path);

    /* C / CMake 项目标志 */
    if (strcmp(base, "CMakeLists.txt") == 0 || strcmp(base, "Makefile") == 0 ||
        strcmp(base, "compile_commands.json") == 0) {
        index->project_type_flags |= CA_PROJECT_TYPE_C | CA_PROJECT_TYPE_CMAKE;
    }
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0 ||
        strcmp(ext, ".cpp") == 0 || strcmp(ext, ".hpp") == 0) {
        index->project_type_flags |= CA_PROJECT_TYPE_C;
    }

    /* Node.js 项目标志 */
    if (strcmp(base, "package.json") == 0 || strcmp(base, "pnpm-lock.yaml") == 0 ||
        strcmp(base, "yarn.lock") == 0 || strcmp(base, "package-lock.json") == 0) {
        index->project_type_flags |= CA_PROJECT_TYPE_NODE;
    }

    /* Python 项目标志 */
    if (strcmp(base, "pyproject.toml") == 0 || strcmp(base, "requirements.txt") == 0 ||
        strcmp(base, "setup.py") == 0 || strcmp(ext, ".py") == 0) {
        index->project_type_flags |= CA_PROJECT_TYPE_PYTHON;
    }

    /* Rust 项目标志 */
    if (strcmp(base, "Cargo.toml") == 0 || strcmp(entry->path, "src/main.rs") == 0 ||
        strcmp(entry->path, "src/lib.rs") == 0) {
        index->project_type_flags |= CA_PROJECT_TYPE_RUST;
    }
}

const char *ca_file_kind_to_string(ca_file_kind_t kind)
{
    switch (kind) {
    case CA_FILE_KIND_SOURCE:
        return "source";
    case CA_FILE_KIND_HEADER:
        return "header";
    case CA_FILE_KIND_CONFIG:
        return "config";
    case CA_FILE_KIND_DOC:
        return "doc";
    case CA_FILE_KIND_TEST:
        return "test";
    case CA_FILE_KIND_BINARY:
        return "binary";
    case CA_FILE_KIND_GENERATED:
        return "generated";
    case CA_FILE_KIND_UNKNOWN:
    default:
        return "unknown";
    }
}
