#include <assert.h>
#include <math.h>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include "../misrc_gui/ui/gui_font.c"
#include "../misrc_gui/assets/inter_font_data.h"

static void test_collection(void) {
    unsigned char collection[64] = {0};
    memcpy(collection, "ttcf", 4);
    write_be32(collection + 8, 1); write_be32(collection + 12, 20);
    collection[21] = 1; collection[25] = 1;
    memcpy(collection + 32, "name", 4);
    write_be32(collection + 40, 56); write_be32(collection + 44, 4);
    memcpy(collection + 56, "test", 4);
    size_t size = 0;
    unsigned char *face = font_extract_face(collection, sizeof(collection), 0, &size);
    assert(face && size == 32 && read_be32(face + 20) == 28);
    assert(memcmp(face + 28, "test", 4) == 0);
    free(face);
    assert(!font_extract_face(collection, sizeof(collection), 1, &size));
    for (size_t length = 0; length < 60; length++)
        assert(!font_extract_face(collection, length, 0, &size));
    write_be32(collection + 40, UINT32_MAX);
    assert(!font_extract_face(collection, sizeof(collection), 0, &size));
    write_be32(collection + 40, 56); write_be32(collection + 44, UINT32_MAX);
    assert(!font_extract_face(collection, sizeof(collection), 0, &size));
}

static void write_be16(unsigned char *p, unsigned int value) {
    p[0] = (unsigned char)(value >> 8); p[1] = (unsigned char)value;
}

static void test_empty_metrics_bounds(void) {
    unsigned char data[194] = {0};
    static const char *tags[] = { "cmap", "hhea", "hmtx", "maxp" };
    static const unsigned int offsets[] = {80, 144, 180, 188}, lengths[] = {64, 36, 8, 6};
    write_be16(data + 4, 4);
    for (size_t i = 0; i < 4; i++) {
        unsigned char *entry = data + 12 + 16 * i;
        memcpy(entry, tags[i], 4);
        write_be32(entry + 8, offsets[i]); write_be32(entry + 12, lengths[i]);
    }
    unsigned char *cmap = data + 80, *map = cmap + 12;
    write_be16(cmap + 2, 1); write_be16(cmap + 4, 3); write_be16(cmap + 6, 10);
    write_be32(cmap + 8, 12);
    write_be16(map, 12); write_be32(map + 4, 28); write_be32(map + 12, 1);
    write_be32(map + 16, 0x3000); write_be32(map + 20, 0x3000); write_be32(map + 24, 1);
    write_be16(data + 148, 800); write_be16(data + 150, (unsigned int)(uint16_t)-200);
    write_be16(data + 178, 2); write_be16(data + 184, 1000); write_be16(data + 192, 2);
    font_source_t source = { .data = data, .size = sizeof(data) };
    int advance = -1;
    assert(font_empty_advance(&source, 0x3000, &advance) && advance == 64);
    assert(!font_empty_advance(&source, 0x3001, &advance));
    write_be32(map + 24, 2); assert(!font_empty_advance(&source, 0x3000, &advance));
    write_be32(map + 24, 1);
    write_be32(map + 12, UINT32_MAX); assert(!font_empty_advance(&source, 0x3000, &advance));
    write_be32(map + 12, 1);
    write_be32(cmap + 8, UINT32_MAX); assert(!font_empty_advance(&source, 0x3000, &advance));
    write_be32(cmap + 8, 12);
    write_be16(data + 178, 0); assert(!font_empty_advance(&source, 0x3000, &advance));
    write_be16(data + 178, 2);
    write_be16(data + 150, 800); assert(!font_empty_advance(&source, 0x3000, &advance));
    write_be16(data + 150, (unsigned int)(uint16_t)-200);
    /* Format 4: both direct idDelta and glyphIdArray mapping. */
    memset(map, 0, 34);
    write_be16(map, 4); write_be16(map + 2, 32); write_be16(map + 6, 4);
    write_be16(map + 14, 0x3000); write_be16(map + 16, 0xffff);
    write_be16(map + 20, 0x3000); write_be16(map + 22, 0xffff);
    write_be16(map + 24, (unsigned int)(uint16_t)(1 - 0x3000)); write_be16(map + 26, 1);
    assert(font_empty_advance(&source, 0x3000, &advance) && advance == 64);
    assert(!font_empty_advance(&source, 0x3001, &advance));
    write_be16(map + 2, 34); write_be16(map + 24, 0);
    write_be16(map + 28, 4); write_be16(map + 32, 1);
    assert(font_empty_advance(&source, 0x3000, &advance) && advance == 64);
    write_be16(map + 32, 0); assert(!font_empty_advance(&source, 0x3000, &advance));
    write_be16(map + 32, 1);
    write_be16(map + 28, 0xffff); assert(!font_empty_advance(&source, 0x3000, &advance));
    write_be16(map + 28, 4);
    for (size_t length = 0; length < sizeof(data); length++) {
        source.size = length;
        assert(!font_empty_advance(&source, 0x3000, &advance));
    }
}

static void test_blank_and_candidate_cache(void) {
    /* Embedded Inter has a real non-breaking space; raylib returns no bitmap
     * and no advance for it. This regression does not need any system font. */
    font_source_t source = { .data = (unsigned char *)inter_font_data, .size = inter_font_data_size };
    Font blank = font_source_glyph(&source, 0xa0);
    assert(blank.glyphs && blank.glyphs[0].advanceX > 0);
    assert(blank.recs[0].width == 0 && blank.recs[0].height == 0 && !blank.texture.id);
    assert(font_has_glyph(blank, 0xa0) && font_advance(blank, 0xa0, 64) > 0);
    font_release(blank);
    Font absent = font_source_glyph(&source, 0x10ffff);
    assert(!absent.glyphs);
    char path[80];
    snprintf(path, sizeof(path), "gui_font_test_%ld.ttf", (long)getpid());
    FILE *file = NULL;
    assert(file_open_write(&file, path, false, false) == 0);
    assert(fwrite(inter_font_data, 1, inter_font_data_size, file) == inter_font_data_size);
    assert(fclose(file) == 0);
    for (int i = 0; i < SOURCE_CACHE_SIZE + 1; i++) {
        absent = font_file_glyph(path, 0, 0x10ffff - i);
        assert(!absent.glyphs && source_count == 0 && source_bytes == 0);
    }
    Font found = font_file_glyph(path, 0, 'A');
    assert(found.glyphs && source_count == 1 && source_bytes > 0);
    gui_font_unload();
    /* The generated glyph remains valid after its source data is released. */
    assert(found.glyphs[0].image.data && font_advance(found, 'A', 64) > 0);
    font_release(found);
    /* Exercise both hard cache limits without allocating 64 MiB of fixtures. */
    source_bytes = SOURCE_BYTES_MAX;
    found = font_file_glyph(path, 0, 'A');
    assert(found.glyphs && source_count == 0 && source_bytes == SOURCE_BYTES_MAX);
    font_release(found); source_bytes = 0;
    source_count = SOURCE_CACHE_SIZE;
    found = font_file_glyph(path, 0, 'A');
    assert(found.glyphs && source_count == SOURCE_CACHE_SIZE && source_bytes == 0);
    font_release(found); gui_font_unload();
    assert(remove(path) == 0);
}

int main(void) {
    SetTraceLogLevel(LOG_WARNING);
    test_collection();
    test_empty_metrics_bounds();
    test_blank_and_candidate_cache();
    Font font = LoadFontFromMemory(".ttf", inter_font_data, inter_font_data_size, 64, NULL, 256);
    assert(font.glyphs && font_has_glyph(font, 0xe9));
    const char text[] = "A\xc3\xa9" "B";
    for (int spacing = 0; spacing < 3; spacing++) {
        Vector2 actual = gui_font_measure_text_n(font, text, sizeof(text) - 1, 24, (float)spacing);
        Vector2 expected = MeasureTextEx(font, text, 24, (float)spacing);
        assert(fabsf(actual.x - expected.x) < 0.001f);
        assert(actual.y == expected.y);
    }
    const char slice[] = { (char)0xc3, (char)0xa9, 'W' };
    assert(gui_font_measure_text_n(font, slice, 2, 24, 0).x == MeasureTextEx(font, "\xc3\xa9", 24, 0).x);
    /* Not NUL-terminated, ending inside a character: no read past this slice. */
    char *truncated = malloc(1); assert(truncated); truncated[0] = (char)0xe3;
    Vector2 invalid = gui_font_measure_text_n(font, truncated, 1, 24, 0);
    assert(invalid.x == MeasureTextEx(font, "?", 24, 0).x);
    free(truncated);
    assert(gui_font_measure_text_n(font, "A\nAB", 4, 24, 1).x == MeasureTextEx(font, "AB", 24, 1).x);
    assert(gui_font_measure_text_n(font, "A\nAB", 4, 24, 1).y == MeasureTextEx(font, "A\nAB", 24, 1).y);
    font_release(font);
    gui_font_unload();
    puts("Unicode font measurement, blank glyphs, source cache and TTC bounds: PASS");
    return 0;
}
