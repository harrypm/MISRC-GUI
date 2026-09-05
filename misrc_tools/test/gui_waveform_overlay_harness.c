/*
 * CPU-only regression tests for the production waveform overlay and clicks.
 * Include the implementation to exercise its private per-panel rectangles.
 * Link with optimization and LTO to remove the unrelated implementation:
 * no GPU, capture backend, resampler or signal processing is needed.
 * Deterministic text metrics test layout decisions, not font rasterization.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../misrc_gui/visualization/gui_oscilloscope.c"

typedef struct {
    Rectangle rect;
    char text[64];
} overlay_draw_t;

static overlay_draw_t draw_calls[128];
static int draw_count;
static int failures;
static int checks;
static const char *test_case;
static Vector2 test_mouse = {-1000, -1000};
static gui_app_t test_app;
static float test_text_width_scale = 1;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL [%s]: %s\n", test_case, message);
        failures++;
    }
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

static bool inside(Rectangle inner, Rectangle outer)
{
    return inner.x >= outer.x - 0.01f && inner.y >= outer.y - 0.01f &&
        inner.x + inner.width <= outer.x + outer.width + 0.01f &&
        inner.y + inner.height <= outer.y + outer.height + 0.01f;
}

static bool overlaps(Rectangle a, Rectangle b)
{
    return fminf(a.x + a.width, b.x + b.width) - fmaxf(a.x, b.x) > 0.01f &&
        fminf(a.y + a.height, b.y + b.height) - fmaxf(a.y, b.y) > 0.01f;
}

static Vector2 center(Rectangle rect)
{
    return (Vector2){rect.x + rect.width / 2, rect.y + rect.height / 2};
}

static bool intersection_center(Rectangle a, Rectangle b, Vector2 *point)
{
    float left = fmaxf(a.x, b.x);
    float top = fmaxf(a.y, b.y);
    float right = fminf(a.x + a.width, b.x + b.width);
    float bottom = fminf(a.y + a.height, b.y + b.height);
    if (right <= left || bottom <= top) return false;
    *point = (Vector2){(left + right) / 2, (top + bottom) / 2};
    return true;
}

static void record_draw(Rectangle rect, const char *text)
{
    if (draw_count >= (int)(sizeof(draw_calls) / sizeof(draw_calls[0]))) {
        fprintf(stderr, "Too many overlay draw calls\n");
        exit(2);
    }
    overlay_draw_t *call = &draw_calls[draw_count++];
    call->rect = rect;
    snprintf(call->text, sizeof(call->text), "%s", text ? text : "");
}

/* Raylib/UI boundaries: record the coordinates passed by production code. */
int gui_text_measure(const char *text, int font_size)
{
    float width = 0;
    for (; *text; text++) {
        width += strchr("ilI1 ", *text) ? 4 :
            (strchr("WM", *text) ? 11 : 8);
    }
    return (int)ceilf(width * font_size / 16.0f * test_text_width_scale);
}

void gui_text_draw(const char *text, float x, float y, int font_size, Color color)
{
    (void)color;
    record_draw((Rectangle){x, y, (float)gui_text_measure(text, font_size),
                            (float)font_size}, text);
}

int gui_text_measure_mono(const char *text, int font_size)
{
    return (int)ceilf(strlen(text) * font_size * 0.6f);
}

void gui_text_draw_mono(const char *text, float x, float y, int font_size, Color color)
{
    (void)color;
    record_draw((Rectangle){x, y, (float)gui_text_measure_mono(text, font_size),
                            (float)font_size}, text);
}

void DrawRectangle(int x, int y, int width, int height, Color color)
{
    (void)x; (void)y; (void)width; (void)height; (void)color;
}

void DrawLineV(Vector2 start, Vector2 end, Color color)
{
    (void)start; (void)end; (void)color;
}

void DrawLineEx(Vector2 start, Vector2 end, float thickness, Color color)
{
    (void)start; (void)end; (void)thickness; (void)color;
}

void DrawRectangleLinesEx(Rectangle rect, float thickness, Color color)
{
    (void)rect; (void)thickness; (void)color;
}

void DrawRectangleRounded(Rectangle rect, float roundness, int segments, Color color)
{
    (void)roundness; (void)segments; (void)color;
    record_draw(rect, NULL);
}

void DrawRectangleRec(Rectangle rect, Color color)
{
    (void)color;
    record_draw(rect, NULL);
}

void DrawTriangle(Vector2 a, Vector2 b, Vector2 c, Color color)
{
    (void)color;
    float left = fminf(a.x, fminf(b.x, c.x));
    float top = fminf(a.y, fminf(b.y, c.y));
    record_draw((Rectangle){left, top, fmaxf(a.x, fmaxf(b.x, c.x)) - left,
                            fmaxf(a.y, fmaxf(b.y, c.y)) - top}, NULL);
}

bool CheckCollisionPointRec(Vector2 point, Rectangle rect)
{
    return point.x >= rect.x && point.x < rect.x + rect.width &&
        point.y >= rect.y && point.y < rect.y + rect.height;
}

bool CheckCollisionRecs(Rectangle first, Rectangle second)
{
    return first.x < second.x + second.width && first.x + first.width > second.x &&
        first.y < second.y + second.height && first.y + first.height > second.y;
}

bool IsMouseButtonPressed(int button)
{
    (void)button;
    return false;
}

Vector2 gui_ui_get_mouse_position(void)
{
    return test_mouse;
}

Color gui_dropdown_option_color(bool selected, bool hovered)
{
    return selected ? COLOR_BUTTON_ACTIVE :
        (hovered ? COLOR_BUTTON_HOVER : COLOR_BUTTON);
}

static void init_state(waveform_panel_state_t *state)
{
    memset(state, 0, sizeof(*state));
    atomic_flag_clear(&state->data_lock);
    state->initialized = true;
    state->render_mode = WAVEFORM_MODE_PHOSPHOR;
    state->trigger_mode = TRIGGER_MODE_RISING;
    state->zoom_scale = 1;
}

static void render(waveform_panel_state_t *state, Rectangle bounds)
{
    draw_count = 0;
    waveform_render_overlay(state, bounds);
}

static const overlay_draw_t *find_text(const char *text)
{
    for (int i = 0; i < draw_count; i++) {
        if (strcmp(draw_calls[i].text, text) == 0) return &draw_calls[i];
    }
    return NULL;
}

static int find_rectangle_draw(Rectangle rect)
{
    for (int i = 0; i < draw_count; i++) {
        Rectangle drawn = draw_calls[i].rect;
        if (!draw_calls[i].text[0] && near(drawn.x, rect.x) && near(drawn.y, rect.y) &&
            near(drawn.width, rect.width) && near(drawn.height, rect.height)) return i;
    }
    return -1;
}

static void check_popup_draw_order(waveform_panel_state_t *state, Rectangle first_option)
{
    int mode = find_rectangle_draw(state->render_mode_btn_rect);
    int trigger = find_rectangle_draw(state->trigger_btn_rect);
    int option = find_rectangle_draw(first_option);
    expect_true(mode >= 0 && trigger >= 0 && option > mode && option > trigger,
                "popup options are drawn after both underlying buttons");
}

static void check_closed_overlay(waveform_panel_state_t *state, Rectangle bounds)
{
    float channel_width = fmaxf(gui_text_measure("CH A", FONT_SIZE_OSC_LABEL),
                                gui_text_measure("CH B", FONT_SIZE_OSC_LABEL));
    Rectangle channel_label = {bounds.x + 8, bounds.y + 4,
                               channel_width, FONT_SIZE_OSC_LABEL};
    const overlay_draw_t *mode = find_text("Mode:");
    const overlay_draw_t *trig = find_text("Trig:");
    expect_true(mode && trig, "both complete labels remain visible");
    if (mode && trig) {
        expect_true(near(mode->rect.y + FONT_SIZE_DROPDOWN_OPT / 2.0f,
                         state->render_mode_btn_rect.y + 9) ||
                    mode->rect.y + mode->rect.height <= state->render_mode_btn_rect.y,
                    "Mode label stays beside or directly above its button");
        expect_true(near(trig->rect.y + FONT_SIZE_DROPDOWN_OPT / 2.0f,
                         state->trigger_btn_rect.y + 9) ||
                    trig->rect.y + trig->rect.height <= state->trigger_btn_rect.y,
                    "Trig label stays beside or directly above its button");
        if (!near(state->render_mode_btn_rect.y, state->trigger_btn_rect.y)) {
            expect_true(near(mode->rect.x, trig->rect.x),
                        "wrapped labels share a left-aligned column");
            expect_true(near(state->render_mode_btn_rect.x, state->trigger_btn_rect.x) &&
                        near(state->render_mode_btn_rect.width, state->trigger_btn_rect.width),
                        "wrapped buttons share a right-aligned equal-width column");
        }
    }
    expect_true(state->render_mode_btn_rect.width >= 85 &&
                state->trigger_btn_rect.width >= 98,
                "button widths never shrink below the original logical sizes");
    expect_true(near(state->render_mode_btn_rect.height, 18) &&
                near(state->trigger_btn_rect.height, 18),
                "button heights retain their normal logical size");
    for (int i = 0; i < draw_count; i++) {
        expect_true(inside(draw_calls[i].rect, bounds),
                    "closed-overlay draw call remains inside its panel");
        expect_true(!overlaps(draw_calls[i].rect, channel_label),
                    "overlay leaves the CH A/CH B label rectangle clear");
        expect_true(!overlaps(draw_calls[i].rect, state->time_div_rect),
                    "controls leave only the actual time/div rectangle clear");
    }
}

static int find_single_row_threshold(waveform_panel_state_t *state)
{
    for (int width = 170; width <= 600; width++) {
        render(state, (Rectangle){73, 41, (float)width, 400});
        if (near(state->render_mode_btn_rect.y, state->trigger_btn_rect.y)) {
            return width;
        }
    }
    return 0;
}

static void test_layout(waveform_panel_state_t *state)
{
    test_case = "wide overlay preserves original positions";
    init_state(state);
    Rectangle bounds = {73, 41, 700, 400};
    render(state, bounds);
    check_closed_overlay(state, bounds);
    float expected_trigger_x = bounds.x + bounds.width - state->trigger_btn_rect.width - 8;
    float expected_mode_x = expected_trigger_x -
        gui_text_measure("Trig:", FONT_SIZE_DROPDOWN_OPT) - 8 -
        state->render_mode_btn_rect.width - 8;
    expect_true(near(state->trigger_btn_rect.x, expected_trigger_x) &&
                near(state->render_mode_btn_rect.x, expected_mode_x),
                "wide overlay keeps the old right-anchored x positions");
    expect_true(near(state->render_mode_btn_rect.y, bounds.y + 8) &&
                near(state->trigger_btn_rect.y, bounds.y + 8),
                "wide overlay retains one row");

    int threshold = find_single_row_threshold(state);
    test_case = "wrap threshold";
    expect_true(threshold > 170 && threshold < 600, "finite wrap threshold exists");
    if (threshold <= 170 || threshold >= 600) return;
    for (int delta = -1; delta <= 1; delta++) {
        bounds.width = (float)(threshold + delta);
        render(state, bounds);
        check_closed_overlay(state, bounds);
        bool single_row = near(state->render_mode_btn_rect.y, state->trigger_btn_rect.y);
        expect_true(single_row == (delta >= 0), "threshold minus/at/plus one is stable");
        if (delta < 0) {
            expect_true(state->trigger_btn_rect.y >=
                        state->render_mode_btn_rect.y + state->render_mode_btn_rect.height,
                        "entire trigger group moves below Mode");
        }
    }

    test_case = "75-200 percent logical bounds";
    static const int scales[] = {75, 80, 90, 100, 110, 120, 130,
                                 140, 150, 160, 170, 180, 190, 200};
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        float scale = scales[i] / 100.0f;
        bounds = (Rectangle){137 / scale, 83 / scale, 420 / scale, 800 / scale};
        render(state, bounds);
        check_closed_overlay(state, bounds);
    }
}

static void check_button_text(const char *label, Rectangle button)
{
    const overlay_draw_t *text = find_text(label);
    expect_true(text != NULL, "complete selected label is drawn without abbreviation");
    if (text) {
        expect_true(inside(text->rect, button) && text->rect.x >= button.x + 4,
                    "selected text fits its button with left padding");
        expect_true(near(text->rect.height, FONT_SIZE_DROPDOWN_OPT),
                    "selected text keeps the global logical font size");
    }
    int index = find_rectangle_draw(button);
    expect_true(index >= 0 && index + 2 < draw_count,
                "button background, text and arrow are recorded");
    if (index >= 0 && index + 2 < draw_count) {
        Rectangle arrow = draw_calls[index + 2].rect;
        expect_true(inside(arrow, button), "dropdown arrow remains inside its button");
        if (text) {
            expect_true(text->rect.x + text->rect.width + 4 <= arrow.x,
                        "complete selected label leaves space before the arrow");
        }
    }
}

static void test_measured_button_widths(waveform_panel_state_t *state)
{
    test_case = "measured labels and stable button widths";
    static const float text_scales[] = {1, 1.5f};
    for (size_t metric = 0; metric < sizeof(text_scales) / sizeof(text_scales[0]); metric++) {
        test_text_width_scale = text_scales[metric];
        init_state(state);
        int threshold = find_single_row_threshold(state);
        expect_true(threshold > 170 && threshold < 600,
                    "wider text metrics still produce a finite wrap threshold");
        Rectangle bounds = {73, 41, (float)(threshold - 1), 400};
        render(state, bounds);
        check_closed_overlay(state, bounds);
        float mode_width = state->render_mode_btn_rect.width;
        float trigger_width = state->trigger_btn_rect.width;
        expect_true(near(mode_width, trigger_width), "both wrapped buttons use the larger width");
        check_button_text("Phosphor", state->render_mode_btn_rect);
        check_button_text("Off", state->trigger_btn_rect);

        for (int mode = 0; mode < WAVEFORM_MODE_COUNT; mode++) {
            state->render_mode = (waveform_render_mode_t)mode;
            for (int trigger = 0; trigger < TRIGGER_MODE_COUNT; trigger++) {
                state->trigger_enabled = true;
                state->trigger_mode = (trigger_mode_t)trigger;
                for (int source = 0; source < TRIGGER_SOURCE_COUNT; source++) {
                    state->trigger_source = (trigger_source_t)source;
                    render(state, bounds);
                    expect_true(near(state->render_mode_btn_rect.width, mode_width) &&
                                near(state->trigger_btn_rect.width, trigger_width),
                                "changing Mode, trigger mode or source never changes button widths");
                    char trigger_label[24];
                    snprintf(trigger_label, sizeof(trigger_label), "%s/%s",
                             s_trigger_mode_short_labels[trigger], s_trigger_source_labels[source]);
                    check_button_text(s_render_mode_labels[mode], state->render_mode_btn_rect);
                    check_button_text(trigger_label, state->trigger_btn_rect);
                }
            }
        }
        bounds.width = mode_width + 16;
        render(state, bounds);
        check_closed_overlay(state, bounds);
    }
    test_text_width_scale = 1;
}

static void test_compact_time_label_clearance(waveform_panel_state_t *state)
{
    test_case = "compact rows reserve only the actual time/div rectangle";
    Rectangle bounds = {73, 41, 320, 400};
    init_state(state);
    state->time_div_rect = (Rectangle){bounds.x + 8, bounds.y + 26, 90, FONT_SIZE_OSC_DIV};
    render(state, bounds);
    check_closed_overlay(state, bounds);
    expect_true(near(state->render_mode_btn_rect.y, bounds.y + 8),
                "right-aligned Mode stays beside the channel label when it fits");
    expect_true(near(state->trigger_btn_rect.y,
                     state->render_mode_btn_rect.y + state->render_mode_btn_rect.height + 2),
                "non-overlapping time/div does not force an empty strip between rows");
    float compact_y = state->trigger_btn_rect.y;

    const overlay_draw_t *trigger = find_text("Trig:");
    expect_true(trigger != NULL, "trigger label is available for the clearance boundary");
    if (!trigger) return;
    float label_x = trigger->rect.x;
    state->time_div_rect.width = label_x - state->time_div_rect.x + 1;
    render(state, bounds);
    check_closed_overlay(state, bounds);
    expect_true(state->trigger_btn_rect.y >=
                state->time_div_rect.y + state->time_div_rect.height + 2 &&
                state->trigger_btn_rect.y > compact_y,
                "a real horizontal collision moves only the affected lower row below time/div");
    expect_true(near(state->render_mode_btn_rect.y, bounds.y + 8),
                "time/div collision does not move the unrelated upper row");

    state->time_div_rect = (Rectangle){0};
    render(state, bounds);
    check_closed_overlay(state, bounds);
    expect_true(near(state->trigger_btn_rect.y, compact_y),
                "removing the time/div label immediately restores compact spacing");
}

static void test_grid_time_label_cache(waveform_panel_state_t *state)
{
    test_case = "grid reports and clears its actual time/div bounds";
    init_state(state);
    draw_count = 0;
    draw_channel_grid(73, 41, 320, 400, "CH A", COLOR_TEXT, true,
                      1, 20000000, false, -1, &state->time_div_rect);
    const overlay_draw_t *label = NULL;
    for (int i = 0; i < draw_count; i++) {
        if (strstr(draw_calls[i].text, "/div")) label = &draw_calls[i];
    }
    expect_true(label != NULL, "enabled grid draws its time/div label");
    if (label) {
        expect_true(near(state->time_div_rect.x, label->rect.x) &&
                    near(state->time_div_rect.y, label->rect.y) &&
                    near(state->time_div_rect.width, label->rect.width + 1) &&
                    near(state->time_div_rect.height, label->rect.height),
                    "cached bounds use the drawn mono text metrics plus rounding tolerance");
    }
    for (int invalid = 0; invalid < 3; invalid++) {
        state->time_div_rect = (Rectangle){1, 2, 300, 14};
        draw_count = 0;
        draw_channel_grid(73, 41, 320, 400, "CH A", COLOR_TEXT, invalid != 0,
                          invalid == 2 ? 0 : 1, invalid == 1 ? 0 : 20000000,
                          false, -1, &state->time_div_rect);
        expect_true(near(state->time_div_rect.width, 0) &&
                    near(state->time_div_rect.height, 0),
                    "hidden grid or invalid timing clears the previous label rectangle");
    }
}

static void test_clicks(waveform_panel_state_t *state)
{
    test_case = "wrapped dropdown clicks";
    Rectangle bounds = {91, 53, 210, 400};
    init_state(state);
    render(state, bounds);
    expect_true(waveform_panel_handle_click(state, &test_app, 0,
                center(state->render_mode_btn_rect), bounds), "Mode button consumes click");
    expect_true(state->render_mode_dropdown_open, "Mode menu opens");
    render(state, bounds);
    for (int i = 0; i < WAVEFORM_MODE_COUNT; i++) {
        state->render_mode_dropdown_open = true;
        render(state, bounds);
        check_popup_draw_order(state, state->render_mode_opts_rect[0]);
        expect_true(waveform_panel_handle_click(state, &test_app, 0,
                    center(state->render_mode_opts_rect[i]), bounds),
                    "drawn Mode option consumes click");
        expect_true(state->render_mode == (waveform_render_mode_t)i &&
                    !state->render_mode_dropdown_open && !state->trigger_dropdown_open,
                    "drawn Mode option selects the correct render mode");
    }

    state->render_mode_dropdown_open = true;
    render(state, bounds);
    bool found_overlap = false;
    for (int i = 0; i < WAVEFORM_MODE_COUNT; i++) {
        Vector2 point;
        if (!intersection_center(state->render_mode_opts_rect[i],
                                 state->trigger_btn_rect, &point)) continue;
        found_overlap = true;
        expect_true(waveform_panel_handle_click(state, &test_app, 0, point, bounds),
                    "overlapping foreground option consumes click");
        expect_true(state->render_mode == (waveform_render_mode_t)i &&
                    !state->render_mode_dropdown_open && !state->trigger_dropdown_open,
                    "foreground Mode option wins over the Trig button");
        break;
    }
    expect_true(found_overlap, "fixture places a Mode option over Trig");
    state->render_mode_dropdown_open = false;

    for (int i = 0; i < TRIGGER_MODE_COUNT; i++) {
        state->trigger_enabled = false;
        state->trigger_dropdown_open = true;
        render(state, bounds);
        check_popup_draw_order(state, state->trigger_mode_opts_rect[0]);
        expect_true(waveform_panel_handle_click(state, &test_app, 1,
                    center(state->trigger_mode_opts_rect[i + 1]), bounds),
                    "drawn trigger option consumes click");
        expect_true(state->trigger_enabled && state->trigger_mode == (trigger_mode_t)i &&
                    state->trigger_source == TRIGGER_SOURCE_CH2 && !state->trigger_dropdown_open,
                    "trigger mode uses the clicked panel's channel default");
    }
    for (int i = 0; i < TRIGGER_SOURCE_COUNT; i++) {
        state->trigger_dropdown_open = true;
        render(state, bounds);
        expect_true(waveform_panel_handle_click(state, &test_app, 1,
                    center(state->trigger_source_opts_rect[i]), bounds),
                    "drawn source option consumes click");
        expect_true(state->trigger_source == (trigger_source_t)i &&
                    !state->trigger_dropdown_open, "CH1/CH2/CH3 select the correct source");
    }
    state->trigger_dropdown_open = true;
    render(state, bounds);
    expect_true(waveform_panel_handle_click(state, &test_app, 1,
                center(state->trigger_mode_opts_rect[0]), bounds), "Off option consumes click");
    expect_true(!state->trigger_enabled, "Off disables the trigger");
}

static void test_short_panels(waveform_panel_state_t *state)
{
    test_case = "short-panel popup bounds and hit testing";
    static const int heights[] = {180, 184, 200, 206, 230};
    init_state(state);
    render(state, (Rectangle){91, 53, 700, 400});
    /* A panel must hold the measured button plus its two 8px margins. */
    float minimum_width = state->trigger_btn_rect.width + 16;
    float widths[] = {minimum_width, minimum_width + 1, minimum_width + 6, 210, 700};
    bool found_mode_overlap = false;
    bool found_header_overlap = false;
    for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
        for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
            Rectangle bounds = {91, 53, (float)widths[w], (float)heights[h]};
            init_state(state);
            state->time_div_rect = (Rectangle){bounds.x + 8, bounds.y + 26,
                                               90, FONT_SIZE_OSC_DIV};
            state->trigger_enabled = true;
            state->trigger_dropdown_open = true;
            render(state, bounds);
            check_popup_draw_order(state, state->trigger_mode_opts_rect[0]);
            for (int i = 0; i < draw_count; i++) {
                expect_true(inside(draw_calls[i].rect, bounds),
                            "short-panel popup and text remain inside the panel");
            }
            for (int i = 0; i < TRIGGER_SOURCE_COUNT; i++) {
                state->trigger_dropdown_open = true;
                render(state, bounds);
                expect_true(waveform_panel_handle_click(state, &test_app, 0,
                            center(state->trigger_source_opts_rect[i]), bounds),
                            "shifted source option consumes click");
                expect_true(state->trigger_source == (trigger_source_t)i,
                            "shifted menu uses its rendered source rectangles");
            }

            state->trigger_dropdown_open = true;
            render(state, bounds);
            for (int i = 0; i <= TRIGGER_MODE_COUNT; i++) {
                Vector2 point;
                if (!intersection_center(state->trigger_mode_opts_rect[i],
                                         state->render_mode_btn_rect, &point)) continue;
                found_mode_overlap = true;
                state->trigger_enabled = true;
                expect_true(waveform_panel_handle_click(state, &test_app, 0, point, bounds),
                            "shifted trigger option over Mode consumes click");
                expect_true(!state->render_mode_dropdown_open && !state->trigger_dropdown_open &&
                            (i == 0 ? !state->trigger_enabled :
                             state->trigger_enabled && state->trigger_mode == (trigger_mode_t)(i - 1)),
                            "foreground trigger option wins over Mode button");
                break;
            }

            state->trigger_dropdown_open = true;
            render(state, bounds);
            Rectangle header = state->trigger_mode_opts_rect[TRIGGER_MODE_COUNT];
            header.y += header.height;
            Vector2 point;
            if (intersection_center(header, state->trigger_btn_rect, &point) ||
                intersection_center(header, state->render_mode_btn_rect, &point)) {
                found_header_overlap = true;
                trigger_mode_t previous_mode = state->trigger_mode;
                trigger_source_t previous_source = state->trigger_source;
                bool previous_enabled = state->trigger_enabled;
                expect_true(waveform_panel_handle_click(state, &test_app, 0, point, bounds),
                            "Channel header consumes a click over an underlying button");
                expect_true(!state->render_mode_dropdown_open &&
                            state->trigger_mode == previous_mode &&
                            state->trigger_source == previous_source &&
                            state->trigger_enabled == previous_enabled,
                            "Channel header cannot activate an underlying control");
            }
        }
    }
    expect_true(found_mode_overlap, "fixture covers a trigger menu overlapping Mode");
    expect_true(found_header_overlap, "fixture covers Channel header overlapping a button");
}

static void test_panel_independence(waveform_panel_state_t *first)
{
    test_case = "separate panels and nonzero origins";
    waveform_panel_state_t second;
    init_state(first);
    init_state(&second);
    Rectangle a = {23, 71, 700, 400};
    Rectangle b = {911, 613, 210, 400};
    render(first, a);
    Rectangle first_button = first->render_mode_btn_rect;
    render(&second, b);
    check_closed_overlay(&second, b);
    expect_true(near(first->render_mode_btn_rect.x, first_button.x) &&
                near(first->render_mode_btn_rect.y, first_button.y),
                "rendering another panel does not replace the first hit rectangles");
    waveform_panel_handle_click(&second, &test_app, 1, center(second.trigger_btn_rect), b);
    expect_true(second.trigger_dropdown_open && !first->trigger_dropdown_open,
                "clicking panel B leaves panel A dropdown state unchanged");
    waveform_panel_handle_click(first, &test_app, 0, center(first_button), a);
    expect_true(first->render_mode_dropdown_open && !second.render_mode_dropdown_open,
                "clicking panel A leaves panel B Mode state unchanged");
}

int main(void)
{
    waveform_panel_state_t state;
    test_app.settings.amplitude_scale = 1;
    test_case = "uninitialized overlay";
    init_state(&state);
    state.initialized = false;
    render(&state, (Rectangle){0, 0, 500, 400});
    expect_true(draw_count == 0, "uninitialized panel draws nothing");
    waveform_render_overlay(NULL, (Rectangle){0, 0, 500, 400});
    expect_true(draw_count == 0, "null panel draws nothing");
    test_layout(&state);
    test_measured_button_widths(&state);
    test_compact_time_label_clearance(&state);
    test_grid_time_label_cache(&state);
    test_clicks(&state);
    test_short_panels(&state);
    test_panel_independence(&state);
    printf("Waveform overlay: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
