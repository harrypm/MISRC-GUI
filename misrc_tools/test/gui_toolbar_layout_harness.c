/*
 * CPU-only checks for the production toolbar connection-button width.
 * Include the private helper and let LTO discard unrelated UI/backend paths.
 * Deterministic font metrics exercise padding without opening a window.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../misrc_gui/ui/gui_ui.c"

static int checks;
static int failures;
static const char *test_case;
static float test_font_width = 1;
static gui_app_t test_app;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL [%s]: %s\n", test_case, message);
        failures++;
    }
}

Font GetFontDefault(void)
{
    static GlyphInfo glyph;
    return (Font){ .baseSize = 16, .glyphCount = 1, .glyphs = &glyph };
}

Vector2 MeasureTextEx(Font font, const char *text, float font_size, float spacing)
{
    float width = 0;
    float face_width = font.texture.id ? 1.25f : 1;
    for (size_t i = 0; text[i]; i++) {
        float advance = text[i] == 'i' ? 3 : text[i] == 'D' ? 9 :
                        text[i] == 'C' ? 8 : 7;
        width += advance * font_size / 16.0f * test_font_width * face_width;
        if (i) width += spacing;
    }
    return (Vector2){width, font_size};
}

static int expected_text_width(const gui_app_t *app, const char *text, int size)
{
    Font font = GetFontDefault();
    if (app && app->fonts && app->fonts[0].texture.id && app->fonts[0].glyphs)
        font = app->fonts[0];
    return (int)ceilf(MeasureTextEx(font, text, (float)size, 0).x);
}

int main(void)
{
    static const int text_sizes[] = {14, 16};
    static const float font_widths[] = {0.25f, 1, 1.75f, 3};
    static const uint64_t live_counts[] = {0, 9, 10, 999, 1000, UINT64_MAX};
    static const int zooms[] = {75, 100, 170, 200};
    Font fonts[2] = {GetFontDefault(), GetFontDefault()};
    fonts[0].texture.id = 1;
    memset(&test_app, 0, sizeof(test_app));

    for (int loaded_font = 0; loaded_font < 3; loaded_font++) {
        // Exercise a missing app, the default font and the loaded UI font.
        test_app.fonts = loaded_font == 2 ? fonts : NULL;
        const gui_app_t *app = loaded_font == 0 ? NULL : &test_app;
        for (size_t font = 0; font < sizeof(font_widths) / sizeof(font_widths[0]); font++) {
            test_font_width = font_widths[font];
            for (size_t size = 0; size < sizeof(text_sizes) / sizeof(text_sizes[0]); size++) {
                int text_size = text_sizes[size];
                for (int compact = 0; compact < 2; compact++) {
                    int widths[2];
                    for (int capturing = 0; capturing < 2; capturing++) {
                        char name[96];
                        snprintf(name, sizeof(name), "font%d width%.2f size%d compact%d capturing%d",
                                 loaded_font, test_font_width, text_size, compact, capturing);
                        test_case = name;
                        const char *label = capturing ? "Disconnect" : "Connect";
                        int padding = compact ? 8 : 12;
                        int minimum = compact ? 32 : 100;
                        int expected = expected_text_width(app, compact ? label : "Disconnect", text_size) + padding;
                        if (expected < minimum) expected = minimum;
                        bool normal_font_exceeds_cap = !compact && expected > 200;
                        if (normal_font_exceeds_cap) expected = 200;
                        // The caller passes the effective local/remote state;
                        // a contradictory app flag must not override it.
                        test_app.is_capturing = !capturing;
                        widths[capturing] = gui_ui_toolbar_connection_width(app, compact, capturing, text_size);
                        expect_true(widths[capturing] == expected,
                                    "width equals measured text plus the specified inset and floor");
                        expect_true(widths[capturing] >= minimum,
                                    "the compact/normal minimum hit width is retained");
                        if (normal_font_exceeds_cap) {
                            expect_true(widths[capturing] == 200,
                                        "normal mode preserves its existing maximum even for artificial wide fonts");
                        } else {
                            expect_true(widths[capturing] - expected_text_width(app, label, text_size) >= padding,
                                        "the complete label fits with both side insets");
                        }
                        for (size_t zoom = 0; zoom < sizeof(zooms) / sizeof(zooms[0]); zoom++) {
                            s_ui_scale_percent = zooms[zoom];
                            for (size_t value = 0; value < sizeof(live_counts) / sizeof(live_counts[0]); value++) {
                                atomic_store(&test_app.samples_a, live_counts[value]);
                                atomic_store(&test_app.error_count, (uint32_t)live_counts[value]);
                                test_app.is_recording = value % 2 != 0;
                                expect_true(gui_ui_toolbar_connection_width(app, compact, capturing, text_size) == expected,
                                            "live counters and global zoom do not change logical button width");
                            }
                        }
                    }
                    test_case = compact ? "compact state widths" : "normal state widths";
                    if (!compact) {
                        expect_true(widths[0] == widths[1],
                                    "normal toolbar reserves Disconnect width in both states");
                    } else if (test_font_width >= 1) {
                        expect_true(widths[0] < widths[1],
                                    "compact Connect does not retain unused Disconnect width");
                    } else {
                        expect_true(widths[0] == 32 && widths[1] == 32,
                                    "very narrow fonts stop at the compact minimum");
                    }
                }
            }
        }
    }
    test_case = "toolbar width accounting";
    for (int tier = GUI_UI_TOOLBAR_TIER_FULL; tier <= GUI_UI_TOOLBAR_TIER_TINY; tier++) {
        gui_ui_toolbar_profile_t profile = gui_ui_toolbar_profile_for_tier(tier);
        int normal = gui_ui_toolbar_required_width(&profile, 45, 160, 90, false);
        int compact = gui_ui_toolbar_required_width(&profile, 45, 160, 90, true);
        expect_true(normal - compact == 8,
                    "compact accounting reclaims only the normal 8px guard");
        profile.connect_width -= 7;
        expect_true(gui_ui_toolbar_required_width(&profile, 45, 160, 90, true) == compact - 7,
                    "the measured row budget reflects connection padding reclaimed one for one");
        expect_true(gui_ui_toolbar_required_width(&profile, 45, 160, 90, false) == normal - 7,
                    "normal accounting preserves all original gaps and its guard");
    }
    printf("Toolbar connection layout: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
