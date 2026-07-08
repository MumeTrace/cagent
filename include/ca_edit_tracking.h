/*
 * ca_edit_tracking.h
 * In-process read-before-edit tracking for exact edit tools.
 */
#ifndef CA_EDIT_TRACKING_H
#define CA_EDIT_TRACKING_H

#include <stdint.h>
#include <stddef.h>

#include "ca_project.h"
#include "ca_status.h"

#define CA_EDIT_TRACKING_MAX_RECORDS 128

typedef struct ca_file_read_record {
    char path[CA_PROJECT_PATH_CAP];
    uint64_t size;
    uint64_t mtime;
    uint64_t content_hash;
    int valid;
} ca_file_read_record_t;

typedef struct ca_edit_tracking {
    ca_file_read_record_t records[CA_EDIT_TRACKING_MAX_RECORDS];
    size_t count;
} ca_edit_tracking_t;

ca_status_t ca_edit_tracking_init(ca_edit_tracking_t *tracking);
void ca_edit_tracking_reset(ca_edit_tracking_t *tracking);

ca_status_t ca_edit_tracking_note_read(ca_edit_tracking_t *tracking,
                                       const char *workspace_root,
                                       const char *path);

ca_status_t ca_edit_tracking_check_fresh(ca_edit_tracking_t *tracking,
                                         const char *workspace_root,
                                         const char *path);

ca_status_t ca_edit_tracking_note_write(ca_edit_tracking_t *tracking,
                                        const char *workspace_root,
                                        const char *path);

#endif
