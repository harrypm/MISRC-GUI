/*
 * CPU-only checks for the production FFT annotation font metrics and bounds.
 * LTO removes unrelated FFT/GPU code from the included implementation.
 * Deterministic font metrics are fixtures, not rasterization or hardware tests.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../misrc_gui/visualization/gui_fft.c"

typedef struct {
    Rectangle bounds;
    char text[64];
    float font_size;
    int font_id;
} label_draw_t;

static label_draw_t draws[2];
static int draw_count;
static int checks;
static int failures;
static const char *test_case;
static Font test_fonts[2] = {{.baseSize = 17}, {.baseSize = 27}};

static void expect_true(bool condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL [%s]: %s\n", test_case, message);
        failures++;
    }
}

Vector2 MeasureTextEx(Font font, const char *text, float font_size, float spacing) {
    size_t length = strlen(text);
    float advance = font.baseSize == 27 ? 0.61f : 0.47f;
    return (Vector2){length * advance * font_size + (length ? length - 1 : 0) * spacing,
                     font_size};
}

int MeasureText(const char *text, int font_size) {
    return (int)ceilf(strlen(text) * 0.55f * font_size);
}

static void record_draw(const char *text, Vector2 position, Vector2 size,
                        float font_size, int font_id) {
    expect_true(draw_count < 2, "at most two annotation lines");
    if (draw_count >= 2) return;
    label_draw_t *draw = &draws[draw_count++];
    draw->bounds = (Rectangle){position.x, position.y, size.x, size.y};
    snprintf(draw->text, sizeof(draw->text), "%s", text);
    draw->font_size = font_size;
    draw->font_id = font_id;
}

void DrawTextEx(Font font, const char *text, Vector2 position, float font_size,
                float spacing, Color color) {
    (void)color;
    record_draw(text, position, MeasureTextEx(font, text, font_size, spacing),
                font_size, font.baseSize);
}

void DrawText(const char *text, int x, int y, int font_size, Color color) {
    (void)color;
    record_draw(text, (Vector2){(float)x, (float)y},
                (Vector2){(float)MeasureText(text, font_size), (float)font_size}, font_size, 0);
}

static void check_bounds(Rectangle panel, int font_id) {
    for (int i = 0; i < draw_count; i++) {
        Rectangle bounds = draws[i].bounds;
        expect_true(bounds.x >= panel.x && bounds.y >= panel.y &&
                    bounds.x + bounds.width <= panel.x + panel.width + 0.01f &&
                    bounds.y + bounds.height <= panel.y + panel.height + 0.01f,
                    "rendered annotation stays inside the existing panel");
        expect_true(draws[i].font_size == FONT_SIZE_NORMAL, "normal font size is preserved");
        expect_true(draws[i].font_id == font_id, "measurement and rendering use the same font");
    }
    if (draw_count == 2) {
        expect_true(draws[1].bounds.y >= draws[0].bounds.y + draws[0].bounds.height,
                    "split lines do not overlap");
    }
}

static void check_label(Font *fonts, Rectangle panel, Vector2 anchor, int expected_lines) {
    draw_count = 0;
    fft_draw_peak_label(fonts, "-74.0dB", "1.1MHz", anchor, 4, panel);
    expect_true(draw_count == expected_lines, "expected one-line, split or no-room layout");
    check_bounds(panel, fonts ? 27 : 0);
}

int main(void) {
    test_case = "exact mono metrics";
    Vector2 size = fft_measure_text_mono(test_fonts, "-74.0dB 1.1MHz", FONT_SIZE_NORMAL);
    Vector2 expected = MeasureTextEx(test_fonts[1], "-74.0dB 1.1MHz", FONT_SIZE_NORMAL, 1);
    expect_true(size.x == expected.x && size.y == expected.y, "mono font and 18px metrics match");
    expect_true(size.x > fft_measure_text(test_fonts, "-74.0dB 1.1MHz", FONT_SIZE_NORMAL),
                "fixture distinguishes proportional from mono font");

    test_case = "edge anchors and narrow panels";
    const float widths[] = {110, 160, 300};
    const float heights[] = {48, 80, 160};
    const float positions[] = {0, 0.5f, 1};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
            Rectangle panel = {100, 80, widths[w], heights[h]};
            for (size_t px = 0; px < sizeof(positions) / sizeof(positions[0]); px++) {
                for (size_t py = 0; py < sizeof(positions) / sizeof(positions[0]); py++) {
                    Vector2 anchor = {panel.x + panel.width * positions[px],
                                      panel.y + panel.height * positions[py]};
                    check_label(test_fonts, panel, anchor, widths[w] - 4 < size.x ? 2 : 1);
                    if (draw_count == 2) {
                        expect_true(strcmp(draws[0].text, "-74.0dB") == 0 &&
                                    strcmp(draws[1].text, "1.1MHz") == 0,
                                    "both full values remain visible when split");
                    } else {
                        expect_true(strcmp(draws[0].text, "-74.0dB 1.1MHz") == 0,
                                    "wide layout preserves the combined label");
                    }
                }
            }
        }
    }

    test_case = "narrow and short fallback";
    check_label(test_fonts, (Rectangle){10, 20, 64, 80}, (Vector2){11, 21}, 2);
    expect_true(strstr(draws[0].text, "...") != NULL && strstr(draws[1].text, "...") != NULL,
                "oversized individual values have explicit ellipses");
    check_label(test_fonts, (Rectangle){10, 20, 110, 25}, (Vector2){119, 44}, 1);
    expect_true(strstr(draws[0].text, "...") != NULL, "short panels use one contained line");
    check_label(test_fonts, (Rectangle){10, 20, 8, 80}, (Vector2){14, 40}, 0);
    check_label(test_fonts, (Rectangle){10, 20, 110, 17}, (Vector2){14, 24}, 0);

    test_case = "default-font fallback";
    check_label(NULL, (Rectangle){10, 20, 300, 80}, (Vector2){309, 99}, 1);
    check_label(NULL, (Rectangle){10, 20, 110, 80}, (Vector2){11, 21}, 2);

    test_case = "fractional viewport coordinates";
    for (int fallback = 0; fallback < 2; fallback++) {
        Font *fonts = fallback ? NULL : test_fonts;
        check_label(fonts, (Rectangle){10.25f, 20.5f, 110.5f, 80.25f},
                    (Vector2){120.75f, 100.75f}, 2);
        check_label(fonts, (Rectangle){10.75f, 20.25f, 300.5f, 80.5f},
                    (Vector2){10.75f, 20.25f}, 1);
        check_label(fonts, (Rectangle){10.25f, 20.75f, 300.25f, 22.5f},
                    (Vector2){310.5f, 43.25f}, 1);
    }

    test_case = "maximum-length fitting";
    struct {
        char text[64];
        char guard[4];
    } buffer;
    memset(buffer.text, '8', sizeof(buffer.text) - 1);
    buffer.text[sizeof(buffer.text) - 1] = '\0';
    memcpy(buffer.guard, "ABCD", sizeof(buffer.guard));
    fft_fit_peak_line(test_fonts, buffer.text, sizeof(buffer.text), 100);
    expect_true(memcmp(buffer.guard, "ABCD", sizeof(buffer.guard)) == 0, "ellipsis keeps buffer bounds");
    expect_true(fft_measure_text_mono(test_fonts, buffer.text, FONT_SIZE_NORMAL).x <= 100,
                "maximum-length label fits its width");

    printf("FFT labels: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
