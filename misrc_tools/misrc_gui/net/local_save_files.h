#ifndef MISRC_LOCAL_SAVE_FILES_H
#define MISRC_LOCAL_SAVE_FILES_H
#include "local_save_protocol.h"
#include <stdio.h>
#include <stdbool.h>
#ifndef MLS_PATH_MAX
#define MLS_PATH_MAX 4096
#endif

typedef struct {
    FILE *fp;
    /* Cursor tracks sequential writes; only header rewrites need fseek. */
    uint64_t next_offset;
    char incomplete_path[MLS_PATH_MAX];
    char final_path[MLS_PATH_MAX];
    bool bad;
    bool was_opened;
    bool was_closed;
} mls_file_t;

typedef struct {
    char directory[MLS_PATH_MAX];
    bool overwrite;
    bool had_error;
    mls_file_t files[MLS_MAX_FILES];
} mls_receiver_t;

/* Each session is tied to an explicitly selected CLIENT-side directory.
 * No server-provided path is ever interpreted as a directory. */
int mls_receiver_init(mls_receiver_t *receiver,const char *directory,bool overwrite);
/* Apply one parsed frame and its exact length payload. Ack ONLY on success.
 * On fatal failure error is sticky, and incomplete files retain .partial suffix. */
int mls_receiver_apply(mls_receiver_t *receiver,const mls_frame_t *frame,const void *payload);
/* Abort closes all incomplete files WITHOUT renaming them to final filenames. */
void mls_receiver_abort(mls_receiver_t *receiver);
#endif
