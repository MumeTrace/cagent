/*
 * edit_tracking.c
 * Runtime-only read-before-edit tracking. This state deliberately lives only
 * in the current process; durable session memory is a later phase.
 */
#include "ca_edit_tracking.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <sys/stat.h>
#define CA_ETR_STAT_STRUCT struct _stat64
#define CA_ETR_STAT(path, st) _stat64((path), (st))
#define CA_ETR_PATH_SEP "\\"
#else
#include <sys/stat.h>
#define CA_ETR_STAT_STRUCT struct stat
#define CA_ETR_STAT(path, st) stat((path), (st))
#define CA_ETR_PATH_SEP "/"
#endif

#define CA_ETR_MAX_HASH_BYTES (1024u * 1024u)
#define CA_ETR_FNV_OFFSET 1469598103934665603ull
#define CA_ETR_FNV_PRIME 1099511628211ull

typedef struct ca_edit_file_state {
    uint64_t size;
    uint64_t mtime;
    uint64_t hash;
} ca_edit_file_state_t;

static int ca_etr_is_absolute_path(const char *path)
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

static int ca_etr_segment_equals(const char *start, size_t len, const char *value)
{
    return value != NULL && strlen(value) == len && strncmp(start, value, len) == 0;
}

static int ca_etr_has_parent_segment(const char *path)
{
    const char *segment = path;
    const char *cursor;

    if (path == NULL) {
        return 1;
    }

    for (cursor = path; ; cursor++) {
        if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
            size_t len = (size_t)(cursor - segment);
            if (ca_etr_segment_equals(segment, len, "..")) {
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

static ca_status_t ca_etr_normalize_path(const char *input_path, char *out_rel, size_t out_rel_size)
{
    const char *rel;
    int written;

    if (input_path == NULL || out_rel == NULL || out_rel_size == 0 || input_path[0] == '\0') {
        return CA_ERR_INVALID_ARG;
    }
    if (ca_etr_is_absolute_path(input_path) || ca_etr_has_parent_segment(input_path)) {
        return CA_ERR_PERMISSION_DENIED;
    }

    rel = input_path;
    while (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) {
        rel += 2;
    }
    if (rel[0] == '\0' || strcmp(rel, ".") == 0) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(out_rel, out_rel_size, "%s", rel);
    if (written < 0 || (size_t)written >= out_rel_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static ca_status_t ca_etr_join_workspace_path(const char *workspace_root,
                                              const char *rel_path,
                                              char *out_abs,
                                              size_t out_abs_size)
{
    int written;

    if (workspace_root == NULL || workspace_root[0] == '\0' || rel_path == NULL ||
        out_abs == NULL || out_abs_size == 0) {
        return CA_ERR_INVALID_ARG;
    }

    written = snprintf(out_abs, out_abs_size, "%s%s%s", workspace_root, CA_ETR_PATH_SEP, rel_path);
    if (written < 0 || (size_t)written >= out_abs_size) {
        return CA_ERR_INVALID_ARG;
    }

    return CA_OK;
}

static ca_status_t ca_etr_hash_file(const char *abs_path, ca_edit_file_state_t *out_state)
{
    CA_ETR_STAT_STRUCT st;
    unsigned char buffer[4096];
    FILE *file;
    size_t got;
    uint64_t hash = CA_ETR_FNV_OFFSET;

    if (abs_path == NULL || out_state == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    memset(out_state, 0, sizeof(*out_state));
    if (CA_ETR_STAT(abs_path, &st) != 0) {
        return CA_ERR_NOT_FOUND;
    }
    if ((uint64_t)st.st_size > CA_ETR_MAX_HASH_BYTES) {
        return CA_ERR_INVALID_ARG;
    }

    file = fopen(abs_path, "rb");
    if (file == NULL) {
        return CA_ERR_IO;
    }

    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        size_t i;
        for (i = 0; i < got; i++) {
            hash ^= (uint64_t)buffer[i];
            hash *= CA_ETR_FNV_PRIME;
        }
    }
    if (ferror(file)) {
        fclose(file);
        return CA_ERR_IO;
    }
    fclose(file);

    out_state->size = (uint64_t)st.st_size;
    out_state->mtime = (uint64_t)st.st_mtime;
    out_state->hash = hash;
    return CA_OK;
}

static ca_file_read_record_t *ca_etr_find_record(ca_edit_tracking_t *tracking, const char *rel_path)
{
    size_t i;

    if (tracking == NULL || rel_path == NULL) {
        return NULL;
    }

    for (i = 0; i < tracking->count; i++) {
        if (tracking->records[i].valid && strcmp(tracking->records[i].path, rel_path) == 0) {
            return &tracking->records[i];
        }
    }

    return NULL;
}

static ca_status_t ca_etr_note_state(ca_edit_tracking_t *tracking,
                                     const char *workspace_root,
                                     const char *path)
{
    char rel_path[CA_PROJECT_PATH_CAP];
    char abs_path[CA_PROJECT_PATH_CAP];
    ca_edit_file_state_t state;
    ca_file_read_record_t *record;
    ca_status_t status;
    int written;

    if (tracking == NULL || workspace_root == NULL || path == NULL) {
        return CA_ERR_INVALID_ARG;
    }

    status = ca_etr_normalize_path(path, rel_path, sizeof(rel_path));
    if (status != CA_OK) {
        return status;
    }
    status = ca_etr_join_workspace_path(workspace_root, rel_path, abs_path, sizeof(abs_path));
    if (status != CA_OK) {
        return status;
    }
    status = ca_etr_hash_file(abs_path, &state);
    if (status != CA_OK) {
        return status;
    }

    record = ca_etr_find_record(tracking, rel_path);
    if (record == NULL) {
        if (tracking->count >= CA_EDIT_TRACKING_MAX_RECORDS) {
            return CA_ERR_TOOL_FAILED;
        }
        record = &tracking->records[tracking->count];
        tracking->count++;
    }

    memset(record, 0, sizeof(*record));
    written = snprintf(record->path, sizeof(record->path), "%s", rel_path);
    if (written < 0 || (size_t)written >= sizeof(record->path)) {
        return CA_ERR_INVALID_ARG;
    }
    record->size = state.size;
    record->mtime = state.mtime;
    record->content_hash = state.hash;
    record->valid = 1;
    return CA_OK;
}

ca_status_t ca_edit_tracking_init(ca_edit_tracking_t *tracking)
{
    if (tracking == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    memset(tracking, 0, sizeof(*tracking));
    return CA_OK;
}

void ca_edit_tracking_reset(ca_edit_tracking_t *tracking)
{
    if (tracking != NULL) {
        memset(tracking, 0, sizeof(*tracking));
    }
}

ca_status_t ca_edit_tracking_note_read(ca_edit_tracking_t *tracking,
                                       const char *workspace_root,
                                       const char *path)
{
    return ca_etr_note_state(tracking, workspace_root, path);
}

ca_status_t ca_edit_tracking_note_write(ca_edit_tracking_t *tracking,
                                        const char *workspace_root,
                                        const char *path)
{
    char rel_path[CA_PROJECT_PATH_CAP];
    ca_file_read_record_t *record;
    ca_status_t status;

    /*
     * Successful writes refresh an existing read baseline, but they do not
     * create one. The hard rule is still: edit_file must be preceded by a
     * successful read_file/read_file_range in this process.
     */
    if (tracking == NULL || workspace_root == NULL || path == NULL) {
        return CA_ERR_INVALID_ARG;
    }
    status = ca_etr_normalize_path(path, rel_path, sizeof(rel_path));
    if (status != CA_OK) {
        return status;
    }
    record = ca_etr_find_record(tracking, rel_path);
    if (record == NULL) {
        return CA_OK;
    }
    return ca_etr_note_state(tracking, workspace_root, rel_path);
}

ca_status_t ca_edit_tracking_check_fresh(ca_edit_tracking_t *tracking,
                                         const char *workspace_root,
                                         const char *path)
{
    char rel_path[CA_PROJECT_PATH_CAP];
    char abs_path[CA_PROJECT_PATH_CAP];
    ca_edit_file_state_t state;
    ca_file_read_record_t *record;
    ca_status_t status;

    if (tracking == NULL || workspace_root == NULL || path == NULL) {
        return CA_ERR_NOT_FOUND;
    }

    status = ca_etr_normalize_path(path, rel_path, sizeof(rel_path));
    if (status != CA_OK) {
        return status;
    }
    record = ca_etr_find_record(tracking, rel_path);
    if (record == NULL) {
        return CA_ERR_NOT_FOUND;
    }

    status = ca_etr_join_workspace_path(workspace_root, rel_path, abs_path, sizeof(abs_path));
    if (status != CA_OK) {
        return status;
    }
    status = ca_etr_hash_file(abs_path, &state);
    if (status != CA_OK) {
        return status;
    }

    /*
     * mtime alone can be coarse on some filesystems, especially on Windows.
     * Size plus FNV-1a hash gives the edit preflight a cheap freshness check
     * without storing full file content in memory.
     */
    if (record->size != state.size ||
        record->mtime != state.mtime ||
        record->content_hash != state.hash) {
        return CA_ERR_PERMISSION_DENIED;
    }

    return CA_OK;
}
