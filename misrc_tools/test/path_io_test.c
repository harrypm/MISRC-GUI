/* Filesystem regression driver. Python creates/inspects names independently
 * so a writer and reader agreeing on the same mojibake path cannot pass. */
#include "../common/file_utils.h"
#include "../common/utf8_argv.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(file_init_utf8_argv(&argc, &argv) && argc == 2);
    char path[FILE_PATH_MAX];
    const char *name = "\xE4\xB8\xAD\xE6\x96\x87\xF0\x9F\x93\xBC.bin";
    char *environment = file_getenv_utf8("MISRC_PATH_TEST_ENV");
    assert(environment && !strcmp(environment, argv[1]));
    free(environment);
    environment = file_getenv_utf8("MISRC_PATH_TEST_EMPTY");
    assert(environment && !environment[0]); free(environment);
    assert(file_getenv_utf8("MISRC_PATH_TEST_ABSENT") == NULL);
    uint64_t available = 0, bytes = 0;
    bool directory = false;
    assert(file_path_info_utf8(argv[1], NULL, &directory) && directory);
    assert(file_disk_free_space_utf8(argv[1], &available) && available > 0);
    int n = snprintf(path, sizeof(path), "%s/%s", argv[1], name);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *output = NULL;
    assert(file_open_write(&output, path, false, false) == 0);
    assert(fwrite("path-bytes", 1, 10, output) == 10 && fclose(output) == 0);
    assert(file_path_info_utf8(path, &bytes, &directory) && bytes == 10 && !directory);
    assert(file_open_write(&output, path, false, false) == -1);
    FILE *input = file_open_read_utf8(path);
    char data[10]; assert(input && fread(data, 1, sizeof(data), input) == sizeof(data));
    assert(!memcmp(data, "path-bytes", sizeof(data)) && fclose(input) == 0);
    n = snprintf(path, sizeof(path), "%s/%s", argv[1], "\xE7\x9B\xAE\xE5\xBD\x95");
    assert(n > 0 && (size_t)n < sizeof(path));
    assert(file_mkdir_utf8(path, 0700) == 0);
    assert(file_path_info_utf8(path, NULL, &directory) && directory);
    return 0;
}
