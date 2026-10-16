/*
 * MISRC Common - File Utilities Implementation
 */

#include "file_utils.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#include <direct.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/statvfs.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
static wchar_t *file_wide_path(const char *filename) {
    if (!filename || !filename[0]) {
        errno = EINVAL;
        return NULL;
    }
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, NULL, 0);
    if (count <= 0) {
        errno = EILSEQ;
        return NULL;
    }
    if ((size_t)count > SIZE_MAX / sizeof(wchar_t)) {
        errno = ENOMEM;
        return NULL;
    }
    wchar_t *wide = malloc((size_t)count * sizeof(*wide));
    if (!wide) {
        errno = ENOMEM;
        return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, wide, count)) {
        free(wide);
        errno = EILSEQ;
        return NULL;
    }
    return wide;
}
#endif

FILE *file_open_utf8(const char *filename, const char *mode)
{
    if (!filename || !filename[0] || !mode || !mode[0]) {
        errno = EINVAL;
        return NULL;
    }
#if defined(_WIN32) || defined(_WIN64)
    wchar_t *wide = file_wide_path(filename);
    wchar_t wide_mode[16];
    size_t n = strlen(mode);
    if (!wide) return NULL;
    if (n >= sizeof(wide_mode) / sizeof(wide_mode[0])) {
        free(wide); errno = EINVAL; return NULL;
    }
    for (size_t i = 0; i <= n; i++) wide_mode[i] = (unsigned char)mode[i];
    FILE *file = _wfopen(wide, wide_mode);
    int saved_errno = errno;
    free(wide);
    errno = saved_errno;
    return file;
#else
    return fopen(filename, mode);
#endif
}

FILE *file_open_read_utf8(const char *filename) {
    return file_open_utf8(filename, "rb");
}

bool file_path_info_utf8(const char *path, uint64_t *size, bool *is_dir) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t *wide = file_wide_path(path);
    if (!wide) return false;
    WIN32_FILE_ATTRIBUTE_DATA info;
    bool ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &info) != 0;
    free(wide);
    if (!ok) return false;
    if (size) *size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    if (is_dir) *is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat info;
    if (!path || stat(path, &info) != 0) return false;
    if (size) *size = (uint64_t)info.st_size;
    if (is_dir) *is_dir = S_ISDIR(info.st_mode);
#endif
    return true;
}

int file_mkdir_utf8(const char *path, int mode) {
#if defined(_WIN32) || defined(_WIN64)
    (void)mode;
    wchar_t *wide = file_wide_path(path);
    if (!wide) return -1;
    int result = _wmkdir(wide);
    int saved_errno = errno;
    free(wide); errno = saved_errno;
    return result;
#else
    if (!path) { errno = EINVAL; return -1; }
    return mkdir(path, (mode_t)mode);
#endif
}

bool file_disk_free_space_utf8(const char *path, uint64_t *available) {
    if (!path || !available) return false;
#if defined(_WIN32) || defined(_WIN64)
    wchar_t *wide = file_wide_path(path);
    if (!wide) return false;
    ULARGE_INTEGER bytes;
    bool ok = GetDiskFreeSpaceExW(wide, &bytes, NULL, NULL) != 0;
    free(wide);
    if (!ok) return false;
    *available = bytes.QuadPart;
#else
    struct statvfs info;
    if (statvfs(path, &info) != 0) return false;
    *available = (uint64_t)info.f_bavail * (uint64_t)info.f_frsize;
#endif
    return true;
}

char *file_getenv_utf8(const char *name) {
    if (!name || !name[0]) return NULL;
#if defined(_WIN32) || defined(_WIN64)
    wchar_t *wide_name = file_wide_path(name);
    if (!wide_name) return NULL;
    SetLastError(ERROR_SUCCESS);
    DWORD count = GetEnvironmentVariableW(wide_name, NULL, 0);
    if (!count) {
        DWORD error = GetLastError(); free(wide_name);
        if (error == ERROR_ENVVAR_NOT_FOUND) return NULL;
        char *empty = malloc(1); if (empty) empty[0] = '\0'; return empty;
    }
    wchar_t *wide = malloc((size_t)count * sizeof(*wide));
    if (!wide) { free(wide_name); return NULL; }
    SetLastError(ERROR_SUCCESS);
    DWORD actual = GetEnvironmentVariableW(wide_name, wide, count);
    DWORD error = GetLastError();
    free(wide_name);
    if (actual >= count || (!actual && error != ERROR_SUCCESS)) { free(wide); return NULL; }
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    char *value = bytes > 0 ? malloc((size_t)bytes) : NULL;
    if (value && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, value, bytes, NULL, NULL)) {
        free(value); value = NULL;
    }
    free(wide); return value;
#else
    const char *value = getenv(name);
    if (!value) return NULL;
    size_t bytes = strlen(value) + 1;
    char *copy = malloc(bytes);
    if (copy) memcpy(copy, value, bytes);
    return copy;
#endif
}

int file_open_write(FILE **f, const char *filename, bool overwrite, bool interactive)
{
    if (strcmp(filename, "-") == 0) {
        *f = stdout;
        return 0;
    }

    if (file_path_info_utf8(filename, NULL, NULL) && !overwrite) {
        if (interactive) {
            char ch = 0;
            fprintf(stderr, "File '%s' already exists. Overwrite? (y/n) ", filename);
            if (scanf(" %c", &ch) != 1) {
                return -1;
            }
            if (ch != 'y' && ch != 'Y') {
                return -1;
            }
        } else {
            /* Non-interactive mode, file exists, no overwrite permission */
            return -1;
        }
    }

    *f = file_open_utf8(filename, "wb");
    if (!(*f)) {
        fprintf(stderr, "Failed to open %s\n", filename);
        return -2;
    }

    return 0;
}

void file_close_if_not_stdout(FILE *f)
{
    if (f && f != stdout) {
        fclose(f);
    }
}
