/* Platform-native, read-only remote directory browsing.
 * Absolute server paths, never relative to the recording output directory. */
#ifndef GUI_REMOTE_FS_H
#define GUI_REMOTE_FS_H
#include <stdbool.h>
#include <stddef.h>
#define GUI_REMOTE_FS_PAGE 12
#define GUI_REMOTE_FS_PATH 4096
#define GUI_REMOTE_FS_NAME 256
typedef struct {
    char path[GUI_REMOTE_FS_PATH];
    char parent[GUI_REMOTE_FS_PATH];
    char names[GUI_REMOTE_FS_PAGE][GUI_REMOTE_FS_NAME];
    unsigned count;
    unsigned page;
    bool has_more;
    bool computer;
    bool recovered; /* invalid/missing starting path: returned OS root instead */
} gui_remote_fs_result_t;
/* Returns false on enumeration failure (including permission denied).
 * Missing / invalid paths recover at the OS root and set recovered. */
bool gui_remote_fs_browse(const char *requested, unsigned page, gui_remote_fs_result_t *out);
#endif
