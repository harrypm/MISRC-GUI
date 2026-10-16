#include "utf8_argv.h"
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

static char **utf8_args;
static int utf8_argc;
static void free_utf8_args(void) {
    if (!utf8_args) return;
    for (int i = 0; i < utf8_argc; i++) free(utf8_args[i]);
    free(utf8_args); utf8_args = NULL;
}
#endif

bool file_init_utf8_argv(int *argc, char ***argv) {
#ifdef _WIN32
    if (!utf8_args) {
        int count = 0;
        wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!wide) return false;
        utf8_args = calloc((size_t)count + 1, sizeof(*utf8_args));
        utf8_argc = count;
        bool ok = utf8_args != NULL;
        for (int i = 0; ok && i < count; i++) {
            int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
            utf8_args[i] = bytes > 0 ? malloc((size_t)bytes) : NULL;
            ok = utf8_args[i] && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                wide[i], -1, utf8_args[i], bytes, NULL, NULL) != 0;
        }
        LocalFree(wide);
        if (!ok || atexit(free_utf8_args) != 0) { free_utf8_args(); return false; }
    }
    *argc = utf8_argc;
    *argv = utf8_args;
#else
    (void)argc; (void)argv;
#endif
    return true;
}
