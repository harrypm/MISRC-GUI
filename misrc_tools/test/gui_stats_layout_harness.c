/*
 * CPU-only checks for the production channel-stats geometry and counters.
 * Include private UI helpers and use LTO to discard unrelated GUI/backend code.
 * Font metrics are deterministic fixtures, not a rasterization or hardware test.
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

static bool near(float first, float second)
{
    return fabsf(first - second) < 0.01f;
}

Font GetFontDefault(void)
{
    static GlyphInfo glyph;
    return (Font){ .baseSize = 16, .glyphCount = 1, .glyphs = &glyph };
}

Vector2 MeasureTextEx(Font font, const char *text, float font_size, float spacing)
{
    (void)font;
    float width = 0;
    for (size_t i = 0; text[i]; i++) {
        char character = text[i];
        float advance = character == '1' ? 4 : character == '7' ? 9 :
            character == '+' || character == '-' ? 6 :
            character == ':' || character == ' ' ? 3 :
            character >= 'A' && character <= 'Z' ? 8 : 7;
        width += advance * font_size / 16.0f * test_font_width;
        if (i) width += spacing;
    }
    return (Vector2){width, font_size};
}

const char *panel_view_type_name(panel_view_type_t type)
{
    static const char *names[] = {
        "Waveform", "FFT", "Video", "Histogram", "Waterfall", "Spectro", "Demod"
    };
    return type >= 0 && type < PANEL_VIEW_COUNT ? names[type] : "Unknown";
}

bool panel_view_type_available(panel_view_type_t type)
{
    return type >= 0 && type < PANEL_VIEW_COUNT;
}

static void reset_app(void)
{
    memset(&test_app, 0, sizeof(test_app));
    test_app.device_count = 1;
    test_app.selected_device = 0;
    test_app.devices[0].type = DEVICE_TYPE_HSDAOH;
    atomic_init(&test_app.clip_count_a_pos, 0);
    atomic_init(&test_app.clip_count_a_neg, 0);
    atomic_init(&test_app.clip_count_b_pos, 0);
    atomic_init(&test_app.clip_count_b_neg, 0);
    atomic_init(&test_app.recording_raw_a, 0);
    atomic_init(&test_app.recording_raw_b, 0);
    atomic_init(&test_app.recording_compressed_a, 0);
    atomic_init(&test_app.recording_compressed_b, 0);
    test_font_width = 1;
    s_ui_scale_percent = 200;
}

static const int test_scales[] = {75, 100, 110, 150, 170, 200};
static const uint32_t test_counts[] = {
    0, 1, 7, 8, 9, 10, 11, 77, 88, 99, 100, 111, 777, 888, 999,
    1000, 1001, 1111, 7777, 8888, 9999, 10000, 11111, 77777, 88888,
    99999, 100000, 111111, 777777, 888888, 999999,
    1000000, 1000001, 1111111, 7777777, 8888888, 9999999,
    10000000, 11111111, 77777777, 88888888, 99999999,
    100000000, 111111111, 777777777, 888888888, 999999999,
    1000000000, 1000000001, 1111111111, 1777777777, 1888888888,
    UINT32_MAX - 1, UINT32_MAX
};

static int text_width(const char *text)
{
    return (int)ceilf(MeasureTextEx(GetFontDefault(), text, 16, 0).x);
}

static void expect_same_layout(gui_channel_stats_layout_t actual,
                               gui_channel_stats_layout_t expected)
{
    expect_true(near(actual.width, expected.width), "live values do not change panel width");
    expect_true(actual.label_width == expected.label_width,
                "live values do not change the shared label column");
    expect_true(actual.view_width == expected.view_width,
                "live values do not change controls or numeric slots");
    expect_true(actual.padding == expected.padding,
                "live values do not change panel padding");
    expect_true(actual.gap == expected.gap, "live values do not change row gaps");
}

static void check_static_content(gui_channel_stats_layout_t layout,
                                  bool compact, int requested_gap)
{
    const char *labels[] = {"Peak:", "Clip:", "Layout:", "Left:", "Right:", "View:", "DC:"};
    int gap = compact ? (requested_gap > 0 ? requested_gap : 1) : 4;
    expect_true(layout.gap == gap && layout.padding == (compact ? gap : 6),
                "spacing depends on the requested static layout mode only");
    expect_true(layout.width > 0 && layout.label_width > 0 && layout.view_width > 0,
                "all geometry dimensions are positive");
    if (!compact) {
        float global = s_ui_scale_percent / 100.0f;
        float relative = global <= 1 ? 1 : (1 + 0.6f * (global - 1)) / global;
        expect_true(layout.width >= ceilf(185 * relative),
                    "normal mode retains the damped-width minimum");
        expect_true(layout.label_width >= 50, "normal mode retains the original label minimum");
    }
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        expect_true(text_width(labels[i]) <= layout.label_width,
                    "complete static labels fit the label column");
    }
    for (int view = 0; view < PANEL_VIEW_COUNT; view++) {
        expect_true(text_width(panel_view_type_name((panel_view_type_t)view)) + 4 <= layout.view_width,
                    "every complete view name fits its button and dropdown allowance");
    }
    const char *controls[] = {"Single", "Split"};
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        expect_true(text_width(controls[i]) <= layout.view_width,
                    "complete layout labels fit the shared control allowance");
    }
    expect_true(near(layout.label_width + layout.gap + layout.view_width +
                     layout.padding * 2, layout.width),
                "columns and insets exactly fill the sidebar width");
    expect_true(44 + layout.gap <= layout.view_width,
                "two existing CXADC DC buttons retain their full width");
    expect_true(text_width("RAW: 17179869184.00 GB") <= layout.width - layout.padding * 2,
                "the full RAW size readout does not gain another line");
    int slot = (layout.view_width - layout.gap) / 2;
    expect_true(text_width("+100%") <= slot && text_width("-100%") <= slot,
                "both complete full-scale peaks fit independent fixed slots");
    const char *compact_max[] = {"+777K", "-777K", "+777M", "-777M", "+777G", "-777G"};
    for (size_t i = 0; i < sizeof(compact_max) / sizeof(compact_max[0]); i++) {
        expect_true(text_width(compact_max[i]) <= slot,
                    "signed compact counters reserve the widest digit, not just 8");
    }
}

static void set_live_values(uint32_t first, uint32_t second, unsigned int state)
{
    atomic_store(&test_app.clip_count_a_pos, first);
    atomic_store(&test_app.clip_count_a_neg, second);
    atomic_store(&test_app.clip_count_b_pos, UINT32_MAX - first);
    atomic_store(&test_app.clip_count_b_neg, UINT32_MAX - second);
    const float peaks[] = {0, 0.01f, 0.99f, 1};
    test_app.vu_a.peak_pos = peaks[state % 4];
    test_app.vu_a.peak_neg = peaks[(state + 1) % 4];
    test_app.vu_b.peak_pos = peaks[(state + 2) % 4];
    test_app.vu_b.peak_neg = peaks[(state + 3) % 4];
    test_app.is_capturing = (state & 1) != 0;
    test_app.is_recording = (state & 2) != 0;
    test_app.capture_has_channel_b = (state & 4) != 0;
    test_app.settings.capture_b = (state & 8) != 0;
    test_app.settings.use_flac = (state & 16) != 0;
    atomic_store(&test_app.recording_raw_a, (state & 4) ? UINT64_MAX : 0);
    atomic_store(&test_app.recording_raw_b, (state & 8) ? UINT64_C(1099511627776) : 0);
    atomic_store(&test_app.recording_compressed_a, (state & 16) ? UINT64_MAX : 0);
    atomic_store(&test_app.recording_compressed_b, (state & 32) ? 1 : 0);
    test_app.recording_start_time = state & 1 ? 3600 : 0;
    test_app.last_recording_duration_s = state & 32 ? 3600001 : 0;
}

static void check_live_value_invariance(gui_channel_stats_layout_t layout,
                                        bool compact, int gap)
{
    for (size_t i = 0; i < sizeof(test_counts) / sizeof(test_counts[0]); i++) {
        for (unsigned int state = 0; state < 64; state++) {
            set_live_values(test_counts[i], test_counts[(i + 17) %
                            (sizeof(test_counts) / sizeof(test_counts[0]))], state);
            expect_same_layout(gui_ui_stats_layout(&test_app, compact, gap), layout);
        }
    }
    const device_type_t devices[] = {DEVICE_TYPE_DDD, DEVICE_TYPE_CXADC,
                                     DEVICE_TYPE_HSDAOH, DEVICE_TYPE_PLAYBACK};
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        test_app.devices[0].type = devices[i];
        set_live_values(UINT32_MAX, 0, 63);
        expect_same_layout(gui_ui_stats_layout(&test_app, compact, gap), layout);
    }
}

static void check_formatted_counter(uint32_t value, char sign, int slot)
{
    char exact[16], expected[16];
    snprintf(exact, sizeof(exact), "%c%u", sign, value);
    bool exact_fits = text_width(exact) <= slot;
    if (exact_fits || value < 1000) {
        snprintf(expected, sizeof(expected), "%s", exact);
    } else {
        uint32_t divisor = value >= 1000000000U ? 1000000000U :
                           value >= 1000000U ? 1000000U : 1000U;
        char suffix = divisor == 1000000000U ? 'G' : divisor == 1000000U ? 'M' : 'K';
        snprintf(expected, sizeof(expected), "%c%u%c", sign, value / divisor, suffix);
    }
    unsigned char guarded[18];
    memset(guarded, 0xa5, sizeof(guarded));
    char *actual = (char *)&guarded[1];
    gui_ui_format_clip_counter(&test_app, actual, 16, value, sign, slot);
    expect_true(guarded[0] == 0xa5 && guarded[17] == 0xa5,
                "counter formatting stays within the output buffer");
    bool terminated = memchr(actual, '\0', 16) != NULL;
    expect_true(terminated, "counter formatting terminates its output");
    if (!terminated) return;
    expect_true(actual[0] == sign, "positive and negative counter signs are always retained");
    expect_true(strcmp(actual, expected) == 0,
                exact_fits ? "a fitting exact counter is never abbreviated" :
                             "an overflowing counter uses the expected signed K/M/G magnitude");
    expect_true(text_width(actual) <= slot, "the complete displayed counter fits its fixed slot");
}

static void check_counter_formatting(gui_channel_stats_layout_t layout)
{
    int slot = (layout.view_width - layout.gap) / 2;
    for (size_t i = 0; i < sizeof(test_counts) / sizeof(test_counts[0]); i++) {
        check_formatted_counter(test_counts[i], '+', slot);
        check_formatted_counter(test_counts[i], '-', slot);
    }
    /* Exercise both sides of every unit transition, independent of whether
     * that number happens to fit exactly in the current sidebar slot. */
    const uint32_t boundaries[] = {999, 1000, 1001, 999999, 1000000, 1000001,
                                  999999999, 1000000000, 1000000001, UINT32_MAX};
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        for (int negative = 0; negative < 2; negative++) {
            char sign = negative ? '-' : '+';
            char exact[16];
            snprintf(exact, sizeof(exact), "%c%u", sign, boundaries[i]);
            int exact_width = text_width(exact);
            check_formatted_counter(boundaries[i], sign, exact_width);
            if (boundaries[i] >= 1000)
                check_formatted_counter(boundaries[i], sign, exact_width - 1);
        }
    }
}

static void test_static_layout_and_counters(void)
{
    static char current_case[128];
    test_case = current_case;
    for (int wide_font = 0; wide_font < 2; wide_font++) {
        for (size_t scale = 0; scale < sizeof(test_scales) / sizeof(test_scales[0]); scale++) {
            for (int compact = 0; compact < 2; compact++) {
                for (int gap = 0; gap <= 4; gap++) {
                    reset_app();
                    test_font_width = wide_font ? 2 : 1;
                    s_ui_scale_percent = test_scales[scale];
                    snprintf(current_case, sizeof(current_case), "scale=%d compact=%d gap=%d font=%.0fx",
                             s_ui_scale_percent, compact, gap, test_font_width);
                    gui_channel_stats_layout_t layout = gui_ui_stats_layout(&test_app, compact != 0, gap);
                    check_static_content(layout, compact != 0, gap);
                    check_live_value_invariance(layout, compact != 0, gap);
                    check_counter_formatting(layout);
                    if (wide_font) {
                        test_font_width = 1;
                        gui_channel_stats_layout_t normal_font = gui_ui_stats_layout(&test_app, compact != 0, gap);
                        test_font_width = 2;
                        expect_true(layout.width > normal_font.width && layout.width > 185,
                                    "wide fonts raise the static readability floor beyond 185px");
                        expect_true(layout.label_width > normal_font.label_width &&
                                    layout.view_width > normal_font.view_width,
                                    "wide fonts enlarge label and value allowances together");
                    }
                }
            }
        }
    }
}

static void test_fractional_font_budget(void)
{
    reset_app();
    test_case = "fractional glyph advances";
    test_font_width = 1.125f;
    gui_channel_stats_layout_t layout = gui_ui_stats_layout(&test_app, true, 1);
    int slot = (layout.view_width - layout.gap) / 2;
    int numeric_width = layout.label_width + layout.gap + text_width("+777M") * 2 + layout.gap;
    int record_width = text_width("RAW: 17179869184.00 GB");
    expect_true(near(layout.width, fmaxf(numeric_width, record_width) + layout.padding * 2),
                "compact widths retain recording space without rounding counters glyph by glyph");
    check_static_content(layout, true, 1);
    check_counter_formatting(layout);
    check_live_value_invariance(layout, true, 1);
    for (int value = 0; value <= 999; value++) {
        for (int negative = 0; negative < 2; negative++) {
            char sign = negative ? '-' : '+';
            for (size_t unit = 0; unit < 3; unit++) {
                char text[16];
                snprintf(text, sizeof(text), "%c%d%c", sign, value, "KMG"[unit]);
                expect_true(text_width(text) <= slot,
                            "all mixed-digit compact counters fit the tighter fixed slots");
            }
            if (value <= 100) {
                char text[16];
                snprintf(text, sizeof(text), "%c%d%%", sign, value);
                expect_true(text_width(text) <= slot,
                            "all full-range percentages fit the tighter fixed slots");
            }
        }
    }
}

int main(void)
{
    test_static_layout_and_counters();
    test_fractional_font_budget();
    printf("Channel stats layout: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
