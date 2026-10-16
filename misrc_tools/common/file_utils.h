/*
 * MISRC Common - File Utilities
 *
 * Shared file opening and handling utilities for CLI and GUI.
 */

#ifndef MISRC_FILE_UTILS_H
#define MISRC_FILE_UTILS_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

/* Full paths and filenames have different limits. Callers must reject paths
 * that do not fit, rather than silently selecting a different file. */
#define FILE_PATH_MAX 4096

/* Windows uses UTF-8 at application boundaries; POSIX preserves native bytes. */
FILE *file_open_utf8(const char *filename, const char *mode);
bool file_path_info_utf8(const char *path, uint64_t *size, bool *is_dir);
int file_mkdir_utf8(const char *path, int mode);
bool file_disk_free_space_utf8(const char *path, uint64_t *available);
/* Returns an allocated value (including an empty value), or NULL if absent. */
char *file_getenv_utf8(const char *name);

/*-----------------------------------------------------------------------------
 * File Opening
 *-----------------------------------------------------------------------------*/

/* Open a binary input file. Windows paths are UTF-8, other platforms use
 * the native filename encoding. Returns NULL and sets errno on failure.
 */
FILE *file_open_read_utf8(const char *filename);

/* Open a file for binary writing with optional overwrite confirmation
 *
 * @param f             Pointer to FILE* to receive opened file handle
 * @param filename      Path to file, or "-" for stdout
 * @param overwrite     If true, overwrite without prompting
 * @param interactive   If true and file exists and !overwrite, prompt user
 * @return 0 on success, -1 if user declined overwrite, -2 if open failed
 *
 * If filename is "-", stdout is returned.
 * If interactive is false and file exists and !overwrite, returns -1.
 */
int file_open_write(FILE **f, const char *filename, bool overwrite, bool interactive);

/* Close a file if it's not stdout
 *
 * @param f             File handle to close
 *
 * Safe to call with NULL or stdout - does nothing in those cases.
 */
void file_close_if_not_stdout(FILE *f);

#endif /* MISRC_FILE_UTILS_H */
