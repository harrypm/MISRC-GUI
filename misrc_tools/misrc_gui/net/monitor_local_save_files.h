#ifndef MISRC_MONITOR_LOCAL_SAVE_FILES_H
#define MISRC_MONITOR_LOCAL_SAVE_FILES_H
#include "monitor_local_save_protocol.h"
#include <stdio.h>
#include <stdbool.h>
#ifndef MLS_PATH_MAX
#define MLS_PATH_MAX 4096
#endif

typedef struct {
    FILE *fp;
    /* Cursor tracks sequential writes; only header rewrites need fseek. */
    uint64_t next_offset;
#if defined(_WIN32)
    uintptr_t stage_handle;
    char incomplete_path[MLS_PATH_MAX];
#else
    uint64_t stage_device;
    uint64_t stage_object;
    int stage_fd;
#endif
    char name[256];
    char final_path[MLS_PATH_MAX];
    bool bad;
    bool was_opened;
    bool was_closed;
} mls_file_t;

typedef struct {
    char directory[MLS_PATH_MAX];
    char physical_directory[MLS_PATH_MAX];
    char published_path[MLS_PATH_MAX];
    char recovery_path[MLS_PATH_MAX];
#if defined(_WIN32)
    uintptr_t directory_handle;
#else
    int directory_fd;
    int work_fd;
    int payload_fd;
    char group_name[80];
    char work_name[96];
#endif
    bool overwrite;
    bool had_error;
    mls_file_t files[MLS_MAX_FILES];
} mls_receiver_t;

/* Each session is tied to an explicitly selected CLIENT-side directory.
 * No server-provided path is ever interpreted as a directory. */
int mls_receiver_init(mls_receiver_t *receiver,const char *directory,bool overwrite);
/* Apply one parsed frame and its exact length payload. Ack ONLY on success.
 * On fatal failure error is sticky and unfinished files are retained for recovery. */
int mls_receiver_apply(mls_receiver_t *receiver,const mls_frame_t *frame,const void *payload);
/* Release the retained handles after sending a successful COMMIT acknowledgement. */
void mls_receiver_finish_commit(mls_receiver_t *receiver);
/* Abort closes all incomplete files WITHOUT publishing their final filenames. */
void mls_receiver_abort(mls_receiver_t *receiver);
#endif
