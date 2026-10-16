#ifndef GUI_TEXT_EDIT_H
#define GUI_TEXT_EDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Offsets stay in bytes, but editing and selection stop at UTF-8 boundaries. */
static inline int gui_text_boundary(const char *text, int offset)
{
    int length = (int)strlen(text);
    if (offset < 0) return 0;
    if (offset > length) offset = length;
    while (offset > 0 && ((unsigned char)text[offset] & 0xc0) == 0x80) --offset;
    return offset;
}

static inline int gui_text_previous(const char *text, int offset)
{
    offset = gui_text_boundary(text, offset);
    return offset > 0 ? gui_text_boundary(text, offset - 1) : 0;
}

static inline int gui_text_next(const char *text, int offset)
{
    offset = gui_text_boundary(text, offset);
    if (text[offset]) ++offset;
    while (((unsigned char)text[offset] & 0xc0) == 0x80) ++offset;
    return offset;
}

/* Reject a full replacement on overflow: a partial paste could name another file.
 * The inserted UTF-8 bytes must not overlap dst. */
static inline bool gui_text_replace(char *dst, size_t capacity, int *cursor,
                                    int start, int end, const char *text, size_t count)
{
    if (!dst || !cursor || capacity == 0 || (!text && count)) return false;
    size_t length = strlen(dst);
    start = gui_text_boundary(dst, start);
    end = gui_text_boundary(dst, end);
    if (end < start) return false;
    size_t retained = length - (size_t)(end - start);
    if (retained >= capacity || count >= capacity - retained) return false;
    memmove(dst + start + count, dst + end, length - (size_t)end + 1);
    if (count) memcpy(dst + start, text, count);
    *cursor = start + (int)count;
    return true;
}

static inline bool gui_text_path_char_allowed(int codepoint, bool directory)
{
    /* These are single-line editors. Paths selected by the file picker need not
     * pass this input filter, so legal POSIX control bytes are preserved there. */
    if (codepoint < 32 || codepoint == 127 || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
#ifdef _WIN32
    if (codepoint == '*' || codepoint == '?' || codepoint == '"' ||
        codepoint == '<' || codepoint == '>' || codepoint == '|') return false;
    return directory || (codepoint != '/' && codepoint != '\\' && codepoint != ':');
#else
    return directory || codepoint != '/';
#endif
}

#endif
