/*
 * DdD source detection (input/gui_source_meta.c), which gates the CVBS 50 ohm
 * mode: .lds / .ldf, a DdD JSON sidecar, a MISRC-GUI capture log naming the
 * ddd device. Other files (and other devices' logs) are not DdD.
 */
#include "../misrc_gui/input/gui_source_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); s_fail++; } } while (0)

static char s_dir[512];

static void path_in(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", s_dir, name);
}

static void write_file(const char *name, const char *text) {
    char p[768];
    path_in(p, sizeof(p), name);
    FILE *f = fopen(p, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", p); exit(2); }
    fputs(text, f);
    fclose(f);
}

static bool is_ddd(const char *name) {
    char p[768];
    path_in(p, sizeof(p), name);
    return gui_source_file_is_ddd(p);
}

int main(void) {
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMPDIR");
    if (!tmp) tmp = ".";
    snprintf(s_dir, sizeof(s_dir), "%s", tmp);

    CHECK(gui_source_file_is_ddd("C:/caps/disc_side1.lds"), ".lds is DdD");
    CHECK(gui_source_file_is_ddd("/caps/disc.LDF"), ".ldf is DdD (any case)");
    CHECK(!gui_source_file_is_ddd("/caps/tape.flac"), "a bare FLAC is not DdD");
    CHECK(!gui_source_file_is_ddd("/caps.lds/tape.flac"), "a .lds directory name is not the file's extension");
    CHECK(!gui_source_file_is_ddd(""), "empty path");

    // DdD application JSON sidecar.
    write_file("misrc_meta_test_disc.json",
               "{\n    \"captureInfo\": {\n        \"transferCount\": 42,\n        \"sequenceMarkersPresent\": true\n    }\n}\n");
    CHECK(is_ddd("misrc_meta_test_disc.flac"), "DdD JSON sidecar");
    write_file("misrc_meta_test_other.json", "{ \"title\": \"not a DdD capture\" }\n");
    CHECK(!is_ddd("misrc_meta_test_other.flac"), "unrelated JSON is not DdD");

    // MISRC-GUI capture log of the same take.
    write_file("misrc_meta_test_2026.11.25_10.00.00_misrc_capture.log",
               "[INFO] capture_device_name: DomesdayDuplicator\n[INFO] capture_device_type: ddd\n");
    CHECK(is_ddd("misrc_meta_test_2026.11.25_10.00.00_RF_16-bit_40msps.flac"), "MISRC log names ddd");
    write_file("misrc_meta_test_2026.11.25_11.00.00_misrc_capture.log",
               "[INFO] capture_device_type: hsdaoh\n");
    CHECK(!is_ddd("misrc_meta_test_2026.11.25_11.00.00_RF_8-bit_40msps.flac"), "hsdaoh log is not DdD");
    CHECK(!is_ddd("misrc_meta_test_2026.11.25_12.00.00_RF_8-bit_40msps.flac"), "no log, not DdD");

    const char *made[] = { "misrc_meta_test_disc.json", "misrc_meta_test_other.json",
                           "misrc_meta_test_2026.11.25_10.00.00_misrc_capture.log",
                           "misrc_meta_test_2026.11.25_11.00.00_misrc_capture.log" };
    for (size_t i = 0; i < sizeof(made) / sizeof(made[0]); i++) {
        char p[768];
        path_in(p, sizeof(p), made[i]);
        remove(p);
    }
    if (s_fail) return 1;
    printf("DdD source: .lds/.ldf, DdD JSON, MISRC ddd log detected; others rejected\n");
    return 0;
}
