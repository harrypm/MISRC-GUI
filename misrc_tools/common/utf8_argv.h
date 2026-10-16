/* Windows command lines are UTF-16. Keep application argv in UTF-8, while
 * leaving POSIX argv bytes unchanged. Converted storage lives until exit. */
#ifndef MISRC_UTF8_ARGV_H
#define MISRC_UTF8_ARGV_H
#include <stdbool.h>
bool file_init_utf8_argv(int *argc, char ***argv);
#endif
