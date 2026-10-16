#include "gui_font.h"
#include "../../common/file_utils.h"
#include "rlgl.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Small per-glyph atlases avoid rebuilding textures already referenced by the
 * current render batch. Both successful lookups and misses are cached. */
#define FALLBACK_SIZE 64
/* Enough for every non-ASCII character in a complete maximum-length path,
 * including multiple visible path fields, without per-frame LRU churn. */
#define GLYPH_CACHE_SIZE FILE_PATH_MAX
#define SOURCE_CACHE_SIZE 8
#define SOURCE_BYTES_MAX (64u * 1024u * 1024u)

typedef struct {
    unsigned char *data;
    size_t size;
    char path[FILE_PATH_MAX];
    unsigned int face;
} font_source_t;

typedef struct {
    int codepoint;
    uint64_t used;
    Font font;
} cached_glyph_t;

static font_source_t sources[SOURCE_CACHE_SIZE];
static size_t source_count, source_bytes;
static cached_glyph_t glyph_cache[GLYPH_CACHE_SIZE];
static uint64_t cache_clock;

static void font_release(Font font) {
    if (!font.glyphs || font.glyphs == GetFontDefault().glyphs) return;
    if (font.texture.id) UnloadTexture(font.texture);
    UnloadFontData(font.glyphs, font.glyphCount);
    MemFree(font.recs);
}

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void write_be32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value >> 24); p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8); p[3] = (unsigned char)value;
}

/* raylib's TTF loader expects a standalone SFNT at offset zero. System CJK
 * fonts commonly use TTC collections, whose table offsets are file-relative.
 * Extract the selected face, bounds-checking every table before copying it. */
static unsigned char *font_extract_face(const unsigned char *data, size_t size,
                                        unsigned int face, size_t *out_size) {
    size_t offset = 0;
    if (size < 12) return NULL;
    if (memcmp(data, "ttcf", 4) == 0) {
        uint32_t count = read_be32(data + 8);
        if (count > (size - 12) / 4 || face >= count) return NULL;
        offset = read_be32(data + 12 + face * 4);
    } else if (face != 0) return NULL;
    if (offset > size - 12) return NULL;
    const unsigned char *header = data + offset;
    if (memcmp(header, "\0\1\0\0", 4) && memcmp(header, "OTTO", 4) && memcmp(header, "true", 4)) return NULL;
    size_t count = ((size_t)header[4] << 8) | header[5];
    if (!count || count > (size - offset - 12) / 16) return NULL;
    size_t total = 12 + count * 16;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *table = header + 12 + i * 16;
        size_t start = read_be32(table + 8), bytes = read_be32(table + 12);
        if (start > size || bytes > size - start || bytes > SOURCE_BYTES_MAX - 3) return NULL;
        size_t padded = (bytes + 3) & ~(size_t)3;
        if (total > SOURCE_BYTES_MAX - padded) return NULL;
        total += padded;
    }
    unsigned char *copy = calloc(1, total);
    if (!copy) return NULL;
    size_t cursor = 12 + count * 16;
    memcpy(copy, header, cursor);
    for (size_t i = 0; i < count; i++) {
        const unsigned char *table = header + 12 + i * 16;
        size_t start = read_be32(table + 8), bytes = read_be32(table + 12);
        memcpy(copy + cursor, data + start, bytes);
        write_be32(copy + 12 + i * 16 + 8, (uint32_t)cursor);
        cursor += (bytes + 3) & ~(size_t)3;
    }
    *out_size = total;
    return copy;
}

static bool font_has_glyph(Font font, int codepoint) {
    if (!font.glyphs || !font.recs || font.baseSize <= 0) return false;
    int index = GetGlyphIndex(font, codepoint);
    return index >= 0 && index < font.glyphCount && font.glyphs[index].value == codepoint &&
        (font.glyphs[index].advanceX > 0 || font.recs[index].width > 0);
}

static unsigned int read_be16(const unsigned char *p) {
    return ((unsigned int)p[0] << 8) | p[1];
}

static const unsigned char *font_table(const font_source_t *source, const char *tag, size_t *size) {
    if (source->size < 12) return NULL;
    size_t count = read_be16(source->data + 4);
    if (count > (source->size - 12) / 16) return NULL;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *entry = source->data + 12 + i * 16;
        if (memcmp(entry, tag, 4)) continue;
        size_t offset = read_be32(entry + 8), bytes = read_be32(entry + 12);
        if (offset > source->size || bytes > source->size - offset) return NULL;
        *size = bytes;
        return source->data + offset;
    }
    return NULL;
}

/* raylib omits the advance of an empty glyph except ASCII space. Recover it
 * from the same Unicode cmap and horizontal metrics without inventing a width
 * or mistaking an absent character for a valid blank glyph. */
static bool font_empty_advance(const font_source_t *source, int codepoint, int *advance) {
    size_t cmap_size, hhea_size, hmtx_size, maxp_size;
    const unsigned char *cmap = font_table(source, "cmap", &cmap_size);
    const unsigned char *hhea = font_table(source, "hhea", &hhea_size);
    const unsigned char *hmtx = font_table(source, "hmtx", &hmtx_size);
    const unsigned char *maxp = font_table(source, "maxp", &maxp_size);
    if (!cmap || cmap_size < 4 || !hhea || hhea_size < 36 || !hmtx || !maxp || maxp_size < 6) return false;
    size_t count = read_be16(cmap + 2), offset = 0;
    if (count > (cmap_size - 4) / 8) return false;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *entry = cmap + 4 + i * 8;
        unsigned int platform = read_be16(entry), encoding = read_be16(entry + 2);
        if (platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10)))
            offset = read_be32(entry + 4);
    }
    if (!offset || offset > cmap_size - 2) return false;
    const unsigned char *map = cmap + offset;
    size_t available = cmap_size - offset;
    unsigned int format = read_be16(map), glyph = 0;
    if (format == 4) {
        if (available < 16 || codepoint > 0xffff) return false;
        size_t length = read_be16(map + 2), segments = read_be16(map + 6) / 2;
        if (length < 16 || length > available || !segments || segments > (length - 16) / 8) return false;
        for (size_t i = 0; i < segments; i++) {
            unsigned int end = read_be16(map + 14 + 2 * i);
            if ((unsigned int)codepoint > end) continue;
            unsigned int start = read_be16(map + 16 + 2 * segments + 2 * i);
            if ((unsigned int)codepoint < start) break;
            unsigned int delta = read_be16(map + 16 + 4 * segments + 2 * i);
            size_t range_pos = 16 + 6 * segments + 2 * i;
            unsigned int range = read_be16(map + range_pos);
            if (range) {
                size_t pos = range_pos + range + 2 * ((unsigned int)codepoint - start);
                if (pos > length - 2) return false;
                glyph = read_be16(map + pos);
                if (glyph) glyph = (glyph + delta) & 0xffff;
            } else glyph = ((unsigned int)codepoint + delta) & 0xffff;
            break;
        }
    } else if (format == 12) {
        if (available < 16) return false;
        size_t length = read_be32(map + 4), groups = read_be32(map + 12);
        if (length < 16 || length > available || groups > (length - 16) / 12) return false;
        for (size_t i = 0; i < groups; i++) {
            const unsigned char *group = map + 16 + 12 * i;
            uint32_t first = read_be32(group), last = read_be32(group + 4), index = read_be32(group + 8);
            if ((uint32_t)codepoint < first || (uint32_t)codepoint > last) continue;
            if ((uint32_t)codepoint - first > UINT32_MAX - index) return false;
            glyph = index + (uint32_t)codepoint - first;
            break;
        }
    } else return false;
    unsigned int glyph_count = read_be16(maxp + 4), metrics = read_be16(hhea + 34);
    int height = (int)(int16_t)read_be16(hhea + 4) - (int)(int16_t)read_be16(hhea + 6);
    if (!glyph || glyph >= glyph_count || !metrics || metrics > glyph_count || height <= 0 || metrics > hmtx_size / 4)
        return false;
    unsigned int index = glyph < metrics ? glyph : metrics - 1;
    *advance = (int)((float)read_be16(hmtx + 4 * index) * ((float)FALLBACK_SIZE / (float)height));
    return true;
}

static Font font_source_glyph(font_source_t *source, int codepoint) {
    Font font = {0};
    if (!source) return font;
    GlyphInfo *glyph = LoadFontData(source->data, (int)source->size, FALLBACK_SIZE,
                                   &codepoint, 1, FONT_DEFAULT);
    if (!glyph) return font;
    if (!glyph[0].image.data || glyph[0].image.width <= 0) {
        int advance;
        if (!font_empty_advance(source, codepoint, &advance)) { UnloadFontData(glyph, 1); return font; }
        Rectangle *rec = MemAlloc(sizeof(*rec));
        if (!rec) { UnloadFontData(glyph, 1); return font; }
        *rec = (Rectangle){0};
        glyph[0].advanceX = advance;
        return (Font){ .baseSize = FALLBACK_SIZE, .glyphCount = 1, .glyphs = glyph, .recs = rec };
    }
    /* A single narrow/tall glyph must not use raylib's area-based atlas size:
     * the chosen atlas can be shorter than the bitmap. Size both dimensions
     * explicitly and put the grayscale coverage in the alpha channel. */
    int width = glyph[0].image.width, height = glyph[0].image.height;
    if (width > 1024 || height <= 0 || height > 1024) { UnloadFontData(glyph, 1); return font; }
    Image atlas = { .width = width + 2, .height = height + 2, .mipmaps = 1,
                    .format = PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA };
    atlas.data = MemAlloc((unsigned int)(atlas.width * atlas.height * 2));
    Rectangle *rec = MemAlloc(sizeof(*rec));
    if (!atlas.data || !rec) {
        MemFree(atlas.data); MemFree(rec); UnloadFontData(glyph, 1); return font;
    }
    for (int i = 0; i < atlas.width * atlas.height; i++) {
        ((unsigned char *)atlas.data)[i * 2] = 255;
        ((unsigned char *)atlas.data)[i * 2 + 1] = 0;
    }
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++)
        ((unsigned char *)atlas.data)[((y + 1) * atlas.width + x + 1) * 2 + 1] =
            ((unsigned char *)glyph[0].image.data)[y * width + x];
    *rec = (Rectangle){1, 1, (float)width, (float)height};
    font = (Font){ .baseSize = FALLBACK_SIZE, .glyphCount = 1, .glyphPadding = 1,
                   .glyphs = glyph, .recs = rec };
    if (IsWindowReady()) font.texture = LoadTextureFromImage(atlas);
    UnloadImage(atlas);
    if (font.texture.id) SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    return font;
}

static Font font_file_glyph(const char *path, unsigned int face, int codepoint) {
    Font font = {0};
    for (size_t i = 0; i < source_count; i++) {
        if (sources[i].face == face && strcmp(sources[i].path, path) == 0)
            return font_source_glyph(&sources[i], codepoint);
    }
    if (strlen(path) >= FILE_PATH_MAX) return font;
    uint64_t bytes;
    if (!file_path_info_utf8(path, &bytes, NULL) || bytes < 12 || bytes > SOURCE_BYTES_MAX) return font;
    FILE *file = file_open_read_utf8(path);
    if (!file) return font;
    unsigned char *data = malloc((size_t)bytes);
    bool ok = data && fread(data, 1, (size_t)bytes, file) == (size_t)bytes;
    fclose(file);
    size_t size = 0;
    unsigned char *sfnt = ok ? font_extract_face(data, (size_t)bytes, face, &size) : NULL;
    free(data);
    if (!sfnt) return font;
    font_source_t source = { .data = sfnt, .size = size, .face = face };
    font = font_source_glyph(&source, codepoint);
    /* Failed candidates must not consume the cache. At capacity, the glyph
     * still works: its bitmap/metrics own their data independently of SFNT. */
    if (font.glyphs && source_count < SOURCE_CACHE_SIZE && size <= SOURCE_BYTES_MAX - source_bytes) {
        memcpy(source.path, path, strlen(path) + 1);
        sources[source_count++] = source;
        source_bytes += size;
    } else free(sfnt);
    return font;
}

static Font font_find_glyph(int codepoint) {
    Font font = {0};
    for (size_t i = 0; i < source_count; i++) {
        font = font_source_glyph(&sources[i], codepoint);
        if (font.glyphs) return font;
    }
#ifdef _WIN32
    static const char *names[] = {
        "segoeui.ttf", "msyh.ttc", "msjh.ttc", "meiryo.ttc",
        "msgothic.ttc", "malgun.ttf", "seguisym.ttf", "seguiemj.ttf", "arialuni.ttf"
    };
    char *windows_dir = file_getenv_utf8("WINDIR");
    if (!windows_dir || !windows_dir[0]) { free(windows_dir); windows_dir = file_getenv_utf8("SYSTEMROOT"); }
    const char *directory = windows_dir && windows_dir[0] ? windows_dir : "C:/Windows";
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[FILE_PATH_MAX];
        int count = snprintf(path, sizeof(path), "%s/Fonts/%s", directory, names[i]);
        if (count < 0 || (size_t)count >= sizeof(path)) continue;
        font = font_file_glyph(path, 0, codepoint);
        if (font.glyphs) break;
    }
    free(windows_dir);
#elif defined(__linux__) && !defined(__ANDROID__)
    /* Fontconfig knows user-installed fonts as well as distribution-specific
     * locations. Only the numeric codepoint enters this fixed shell command.
     * No line trimming: a legal POSIX font path may itself contain newlines. */
    char command[128], result[FILE_PATH_MAX + 32];
    snprintf(command, sizeof(command), "fc-match -f '%%{index}:%%{file}' ':charset=%x' 2>/dev/null", codepoint);
    FILE *pipe = popen(command, "r");
    if (pipe) {
        size_t count = fread(result, 1, sizeof(result) - 1, pipe);
        bool complete = feof(pipe) && !ferror(pipe);
        int status = pclose(pipe);
        result[count] = '\0';
        char *end;
        unsigned long face = strtoul(result, &end, 10);
        if (complete && status == 0 && end != result && *end == ':' && face <= UINT32_MAX) {
            font = font_file_glyph(end + 1, (unsigned int)(face & 0xffff), codepoint);
        }
    }
#elif defined(__APPLE__)
    font = font_file_glyph("/System/Library/Fonts/PingFang.ttc", 0, codepoint);
#elif defined(__ANDROID__)
    font = font_file_glyph("/system/fonts/NotoSansCJK-Regular.ttc", 0, codepoint);
#endif
    return font;
}

static Font font_for_codepoint(Font preferred, int codepoint) {
    if (!preferred.glyphs) preferred = GetFontDefault();
    if (codepoint < 128 || font_has_glyph(preferred, codepoint)) return preferred;
    size_t oldest = 0;
    for (size_t i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (glyph_cache[i].used && glyph_cache[i].codepoint == codepoint) {
            glyph_cache[i].used = ++cache_clock;
            return glyph_cache[i].font.glyphs ? glyph_cache[i].font : preferred;
        }
        if (glyph_cache[i].used < glyph_cache[oldest].used) oldest = i;
    }
    cached_glyph_t *entry = &glyph_cache[oldest];
    if (entry->font.glyphs) {
        if (IsWindowReady()) rlDrawRenderBatchActive();
        font_release(entry->font);
    }
    entry->codepoint = codepoint; entry->used = ++cache_clock;
    entry->font = font_find_glyph(codepoint);
    return entry->font.glyphs ? entry->font : preferred;
}

static int font_next_codepoint(const char *text, size_t length, size_t *used) {
    char bounded[5] = {0};
    memcpy(bounded, text, length < 4 ? length : 4);
    int count = 0;
    int codepoint = GetCodepointNext(bounded, &count);
    *used = count > 0 && (size_t)count <= length ? (size_t)count : 1;
    return codepoint;
}

static float font_advance(Font font, int codepoint, float size) {
    if (!font.glyphs || !font.recs || font.baseSize <= 0) return 0;
    int index = GetGlyphIndex(font, codepoint);
    float width = font.glyphs[index].advanceX ? (float)font.glyphs[index].advanceX :
        font.recs[index].width + (float)font.glyphs[index].offsetX;
    return width * size / (float)font.baseSize;
}

Vector2 gui_font_measure_text_n(Font font, const char *text, size_t length, float size, float spacing) {
    Vector2 measured = {0, size};
    if (!text || !length) return measured;
    if (!font.glyphs) font = GetFontDefault();
    float line = 0;
    bool has_character = false;
    float line_step = MeasureTextEx(font, " \n ", size, spacing).y - size;
    for (size_t offset = 0; offset < length && text[offset];) {
        size_t bytes;
        int cp = font_next_codepoint(text + offset, length - offset, &bytes);
        offset += bytes;
        if (cp == '\n') {
            if (line > measured.x) measured.x = line;
            line = 0; has_character = false; measured.y += line_step;
        } else {
            if (has_character) line += spacing;
            line += font_advance(font_for_codepoint(font, cp), cp, size);
            has_character = true;
        }
    }
    if (line > measured.x) measured.x = line;
    return measured;
}

Vector2 gui_font_measure_text(Font font, const char *text, float size, float spacing) {
    if (text) {
        const unsigned char *p = (const unsigned char *)text;
        while (*p && *p < 128) ++p;
        if (!*p) return MeasureTextEx(font, text, size, spacing);
    }
    return gui_font_measure_text_n(font, text, text ? strlen(text) : 0, size, spacing);
}

void gui_font_draw_text(Font font, const char *text, Vector2 position, float size, float spacing, Color tint) {
    if (!text) return;
    if (!font.glyphs) font = GetFontDefault();
    const unsigned char *ascii = (const unsigned char *)text;
    while (*ascii && *ascii < 128) ++ascii;
    if (!*ascii) { DrawTextEx(font, text, position, size, spacing, tint); return; }
    Vector2 cursor = position;
    float line_step = MeasureTextEx(font, " \n ", size, spacing).y - size;
    size_t length = strlen(text);
    for (size_t offset = 0; offset < length;) {
        size_t bytes;
        int cp = font_next_codepoint(text + offset, length - offset, &bytes);
        offset += bytes;
        if (cp == '\n') { cursor.x = position.x; cursor.y += line_step; continue; }
        Font selected = font_for_codepoint(font, cp);
        int index = GetGlyphIndex(selected, cp);
        if (cp != ' ' && cp != '\t' && selected.recs && selected.recs[index].width > 0 && selected.recs[index].height > 0)
            DrawTextCodepoint(selected, cp, cursor, size, tint);
        cursor.x += font_advance(selected, cp, size) + spacing;
    }
}

void gui_font_unload(void) {
    if (IsWindowReady()) rlDrawRenderBatchActive();
    for (size_t i = 0; i < GLYPH_CACHE_SIZE; i++) {
        font_release(glyph_cache[i].font);
    }
    for (size_t i = 0; i < source_count; i++) free(sources[i].data);
    memset(glyph_cache, 0, sizeof(glyph_cache));
    memset(sources, 0, sizeof(sources));
    cache_clock = 0; source_count = 0; source_bytes = 0;
}
