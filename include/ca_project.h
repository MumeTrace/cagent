/*
 * ca_project.h
 * 项目索引系统：扫描工作目录、分类文件、检测项目类型（C / Node / Python / Rust）。
 * 核心结构是 ca_project_index_t，一次扫描填满它，后续拿去展示或喂给 LLM context。
 *
 * Project indexer: scans workspace, classifies files, detects project types.
 * The central struct is ca_project_index_t — scan once, then display / feed to LLM.
 */
#ifndef CA_PROJECT_H
#define CA_PROJECT_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#include "ca_status.h"

#define CA_PROJECT_PATH_CAP   4096      /* max path length                    */
#define CA_PROJECT_MAX_FILES  20000u    /* 单个项目最多索引这么多文件 / cap   */
#define CA_PROJECT_MAX_DEPTH  20u       /* 目录递归深度上限 / max recursion   */

/* 文件类型，扫描时根据扩展名 / 文件名判定
 * File kind — decided by extension / basename during scan. */
typedef enum ca_file_kind {
    CA_FILE_KIND_UNKNOWN = 0,
    CA_FILE_KIND_SOURCE,
    CA_FILE_KIND_HEADER,
    CA_FILE_KIND_CONFIG,
    CA_FILE_KIND_DOC,
    CA_FILE_KIND_TEST,
    CA_FILE_KIND_BINARY,
    CA_FILE_KIND_GENERATED
} ca_file_kind_t;

/* 项目类型用位掩码，一个项目可能同时是 C + CMake（比如本项目）
 * Bitmask — a project can be C + CMake at the same time (like this one). */
enum {
    CA_PROJECT_TYPE_C      = 1 << 0,
    CA_PROJECT_TYPE_CMAKE  = 1 << 1,
    CA_PROJECT_TYPE_NODE   = 1 << 2,
    CA_PROJECT_TYPE_PYTHON = 1 << 3,
    CA_PROJECT_TYPE_RUST   = 1 << 4
};

/* 索引中的单条记录 / One indexed file/dir entry */
typedef struct ca_file_entry {
    char *path;              /* 相对路径 / relative path  */
    uint64_t size_bytes;     /* 文件大小，目录为 0         */
    int is_directory;
    int ignored;             /* 被忽略规则命中了 / hit ignore rule */
    ca_file_kind_t kind;
} ca_file_entry_t;

/* 核心结构 — 整个项目的索引 / The whole project index */
typedef struct ca_project_index {
    char *workspace_root;            /* 工作区绝对路径 / absolute root   */
    ca_file_entry_t *entries;        /* 动态数组 / dynamic array        */
    size_t entry_count;
    size_t entry_capacity;
    size_t skipped_count;            /* 被忽略的 + 超容量的 / ignored + overflow */
    size_t max_entries;              /* 硬上限 CA_PROJECT_MAX_FILES              */
    unsigned int max_depth;          /* 硬上限 CA_PROJECT_MAX_DEPTH              */
    unsigned int project_type_flags; /* 检测到的项目类型位掩码 / detected types  */
} ca_project_index_t;

/* ---- 工作区 + 索引生命周期 / workspace + index lifecycle ---- */
ca_status_t ca_project_get_current_workspace(char *buffer, size_t buffer_size);
ca_status_t ca_project_index_init(ca_project_index_t *index, const char *workspace_root);
void ca_project_index_free(ca_project_index_t *index);
ca_status_t ca_project_index_scan(ca_project_index_t *index);

/* ---- 展示 / display ---- */
void ca_project_index_print_summary(const ca_project_index_t *index, FILE *stream);

/* ---- 内部辅助（跨编译单元引用）/ shared helpers across project/ module ---- */
int ca_project_should_ignore(const char *relative_path, const char *name, int is_directory);
ca_file_kind_t ca_project_file_kind_from_path(const char *relative_path, int is_directory);
void ca_project_detector_update(ca_project_index_t *index, const ca_file_entry_t *entry);
const char *ca_file_kind_to_string(ca_file_kind_t kind);

#endif
