/* CPU-only checks for the production menu and playback width budgets. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../misrc_gui/ui/gui_ui.c"

static int checks;
static int failures;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

Font GetFontDefault(void)
{
    static GlyphInfo glyph;
    return (Font){ .baseSize = 18, .glyphCount = 1, .glyphs = &glyph };
}

Vector2 gui_font_measure_text(Font font, const char *text, float size, float spacing)
{
    (void)font;
    size_t length = strlen(text);
    return (Vector2){length * size * 0.6f + (length ? length - 1 : 0) * spacing, size};
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

int main(void)
{
    char display[64];
    const char *unicode_name = "\xe3\x80\x8c\xe3\x83\xbb \xe3\x80\x8d" "Tape.flac";
    gui_ui_copy_text_prefix(display, sizeof(display), unicode_name);
    expect_true(strcmp(display, unicode_name) == 0,
                "short Unicode filenames keep their original bytes");
    gui_ui_copy_text_prefix(display, 6, unicode_name);
    expect_true(strcmp(display, "\xe3\x80\x8c") == 0,
                "basename truncation never keeps part of a UTF-8 character");
    gui_ui_copy_path_tail(display, sizeof(display),
        "0123456789abcdef\xf0\x9f\x8e\xac" "abcdefghijklmnopqrstuvwx");
    expect_true(strcmp(display, "...abcdefghijklmnopqrstuvwx") == 0,
                "path tails skip a partial four-byte character at the cut");
    gui_ui_copy_path_tail(display, 5,
        "0123456789abcdef\xf0\x9f\x8e\xac" "abcdefghijklmnopqrstuvwx");
    expect_true(strcmp(display, "...a") == 0, "small path displays respect capacity");
    display[0] = 'x';
    gui_ui_copy_path_tail(display, 0, "0123456789012345678901234567890123456789");
    expect_true(display[0] == 'x', "zero-sized path displays are not written");

    const int widths[] = {80, 160, 320, 465, 960, 1920};
    const int heights[] = {80, 160, 180, 425, 540, 1080};
    const int counts[] = {2, 7, 20};
    for (unsigned int w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        for (unsigned int h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
            for (int position = 0; position < 5; position++) {
                Clay_BoundingBox anchor = {widths[w] - 100, (heights[h] - 18) * position / 4.0f, 96, 18};
                for (unsigned int n = 0; n < sizeof(counts) / sizeof(counts[0]); n++) {
                    float content_height = counts[n] * 20;
                    Clay_BoundingBox menu = gui_ui_channel_menu_bounds(anchor, 96, content_height, widths[w], heights[h]);
                    expect_true(menu.x >= 4 && menu.y >= 4, "menu starts inside the viewport margin");
                    expect_true(menu.x + menu.width <= widths[w] - 4 + 0.01f &&
                                menu.y + menu.height <= heights[h] - 4 + 0.01f,
                                "menu ends inside the viewport margin");
                    expect_true(menu.width > 0 && menu.height > 0 && menu.height <= content_height,
                                "menu viewport is positive and cannot exceed its content");
                    float below = heights[h] - 4 - anchor.y - anchor.height;
                    float above = anchor.y - 4;
                    if (below >= content_height) {
                        expect_true(near(menu.y, anchor.y + anchor.height) && near(menu.height, content_height),
                                    "a full menu opens below when it fits");
                    } else if (above >= content_height) {
                        expect_true(near(menu.y + menu.height, anchor.y) && near(menu.height, content_height),
                                    "a full menu flips upward when only above fits");
                    } else {
                        expect_true(near(menu.height, fmaxf(above, below)),
                                    "a tall menu uses the larger side as its scroll viewport");
                    }
                }
            }
        }
    }

    const int timeline_widths[] = {160, 180, 240, 320, 465, 899, 900, 960, 1149, 1150, 1920};
    bool saw_stacked = false;
    bool saw_inline = false;
    for (unsigned int i = 0; i < sizeof(timeline_widths) / sizeof(timeline_widths[0]); i++) {
        int width = timeline_widths[i];
        for (int compact = 0; compact < 2; compact++) {
            gui_ui_channel_spacing_t spacing = {compact ? 35 : 70, compact ? 2 : 8};
            gui_playback_timeline_layout_t layout = gui_ui_playback_timeline_layout(NULL, width, spacing);
            int available = width - spacing.horizontal_gap * 4 - layout.left_padding - layout.right_padding;
            expect_true(layout.label_width > 0 && layout.track_width > 0,
                        "timeline text and scrub track retain a positive width");
            int required = layout.stacked ? (layout.label_width > layout.track_width ? layout.label_width : layout.track_width)
                                          : layout.label_width + 8 + layout.track_width;
            int info_width = 0;
            const int slots[] = {layout.name_width, layout.readout_width, layout.fileinfo_width};
            for (unsigned int slot = 0; slot < sizeof(slots) / sizeof(slots[0]); slot++) {
                if (slots[slot] > 0) info_width += (info_width ? 8 : 0) + slots[slot];
            }
            if (layout.stacked) {
                if (info_width > required) required = info_width;
            } else if (info_width) {
                required += 8 + info_width;
            }
            expect_true(required <= available, "the timeline cannot push its parent wider than the window");
            saw_stacked |= layout.stacked;
            saw_inline |= !layout.stacked;
            if (width >= 960) {
                expect_true(!layout.stacked, "ordinary wide windows retain a single timeline row");
                expect_true(layout.track_width >= 64 && layout.track_width <= (width < 1150 ? 240 : 300),
                            "inline file details leave a usable bounded scrub track");
                if (width >= 1920) {
                    expect_true(layout.track_width == 300,
                                "wide windows retain the preferred full track width");
                }
            }
            for (int zoom = 75; zoom <= 200; zoom += 5) {
                s_ui_scale_percent = zoom;
                gui_playback_timeline_layout_t same = gui_ui_playback_timeline_layout(NULL, width, spacing);
                expect_true(same.label_width == layout.label_width && same.track_width == layout.track_width &&
                            same.left_padding == layout.left_padding && same.right_padding == layout.right_padding &&
                            same.name_width == layout.name_width && same.readout_width == layout.readout_width &&
                            same.fileinfo_width == layout.fileinfo_width &&
                            same.stacked == layout.stacked,
                            "a fixed logical viewport and font determine the entire timeline geometry");
            }
        }
    }
    expect_true(saw_stacked && saw_inline, "both compact and normal timeline arrangements are covered");
    printf("Viewport layout: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
