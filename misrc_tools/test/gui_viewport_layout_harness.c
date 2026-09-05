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

Vector2 MeasureTextEx(Font font, const char *text, float size, float spacing)
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
            expect_true(required <= available, "the timeline cannot push its parent wider than the window");
            saw_stacked |= layout.stacked;
            saw_inline |= !layout.stacked;
            if (width >= 960) {
                expect_true(!layout.stacked, "ordinary wide windows retain a single timeline row");
                expect_true(layout.track_width == (width < 1150 ? 240 : 300),
                            "wide windows retain the original track width");
            }
            for (int zoom = 75; zoom <= 200; zoom += 5) {
                s_ui_scale_percent = zoom;
                gui_playback_timeline_layout_t same = gui_ui_playback_timeline_layout(NULL, width, spacing);
                expect_true(same.label_width == layout.label_width && same.track_width == layout.track_width &&
                            same.left_padding == layout.left_padding && same.right_padding == layout.right_padding &&
                            same.stacked == layout.stacked,
                            "a fixed logical viewport and font determine the entire timeline geometry");
            }
        }
    }
    expect_true(saw_stacked && saw_inline, "both compact and normal timeline arrangements are covered");
    printf("Viewport layout: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
