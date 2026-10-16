#include "../misrc_gui/ui/gui_text_edit.h"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    /* ASCII, three-byte Chinese and Japanese, and a four-byte emoji. */
    const char mixed[] = "A\xe4\xb8\xad\xe3\x80\x8c\xf0\x9f\x93\xbc" "Z";
    const int stops[] = {0, 1, 4, 7, 11, 12};
    int cursor = 0;
    for (size_t i = 1; i < sizeof(stops) / sizeof(stops[0]); ++i) {
        cursor = gui_text_next(mixed, cursor);
        CHECK(cursor == stops[i]);
    }
    CHECK(gui_text_next(mixed, cursor) == cursor);
    for (int i = 4; i >= 0; --i) {
        cursor = gui_text_previous(mixed, cursor);
        CHECK(cursor == stops[i]);
    }
    CHECK(gui_text_previous(mixed, 0) == 0);
    CHECK(gui_text_boundary(mixed, 2) == 1);
    CHECK(gui_text_boundary(mixed, 10) == 7);

    char text[64] = "prefix/";
    cursor = 7;
    CHECK(gui_text_replace(text, sizeof(text), &cursor, cursor, cursor, mixed, strlen(mixed)));
    CHECK(strcmp(text, "prefix/A\xe4\xb8\xad\xe3\x80\x8c\xf0\x9f\x93\xbc" "Z") == 0);
    CHECK(cursor == 19);

    /* Backspace removes the complete emoji, delete removes the complete CJK glyph. */
    strcpy(text, mixed);
    cursor = 11;
    CHECK(gui_text_replace(text, sizeof(text), &cursor, gui_text_previous(text, cursor), cursor, NULL, 0));
    CHECK(strcmp(text, "A\xe4\xb8\xad\xe3\x80\x8c" "Z") == 0);
    CHECK(cursor == 7);
    cursor = 1;
    CHECK(gui_text_replace(text, sizeof(text), &cursor, cursor, gui_text_next(text, cursor), NULL, 0));
    CHECK(strcmp(text, "A\xe3\x80\x8c" "Z") == 0);
    CHECK(cursor == 1);

    /* A replacement must fit as a whole, including its terminating NUL. */
    char small[8] = "A\xe4\xb8\xad" "Z";
    cursor = 4;
    CHECK(!gui_text_replace(small, sizeof(small), &cursor, 1, 4, mixed, strlen(mixed)));
    CHECK(strcmp(small, "A\xe4\xb8\xad" "Z") == 0 && cursor == 4);
    CHECK(gui_text_replace(small, sizeof(small), &cursor, 1, 4, "\xf0\x9f\x93\xbc", 4));
    CHECK(strcmp(small, "A\xf0\x9f\x93\xbc" "Z") == 0 && cursor == 5);
    CHECK(gui_text_replace(small, sizeof(small), &cursor, 5, 5, "!", 1));
    CHECK(!gui_text_replace(small, sizeof(small), &cursor, 6, 6, "x", 1));
    CHECK(strcmp(small, "A\xf0\x9f\x93\xbc!Z") == 0);

    CHECK(gui_text_path_char_allowed(0x4e2d, false));
    CHECK(gui_text_path_char_allowed(0x300c, true));
    CHECK(gui_text_path_char_allowed(0x1f4fc, false));
    CHECK(gui_text_path_char_allowed(' ', false));
    CHECK(gui_text_path_char_allowed('/', true));
    CHECK(!gui_text_path_char_allowed('/', false));
    CHECK(!gui_text_path_char_allowed('\n', true));
    CHECK(!gui_text_path_char_allowed(0, true));
    CHECK(!gui_text_path_char_allowed(0xd800, true));
    CHECK(!gui_text_path_char_allowed(0x110000, true));
#ifdef _WIN32
    CHECK(!gui_text_path_char_allowed('"', true));
    CHECK(!gui_text_path_char_allowed('?', true));
    CHECK(!gui_text_path_char_allowed('\\', false));
    CHECK(gui_text_path_char_allowed('\\', true));
#else
    CHECK(gui_text_path_char_allowed('"', true));
    CHECK(gui_text_path_char_allowed('?', true));
    CHECK(gui_text_path_char_allowed('\\', false));
    CHECK(gui_text_path_char_allowed(':', false));
#endif
    puts("UTF-8 text editing: PASS");
    return 0;
}
