/* Display geometry contracts, independent of Raylib and decoder state. */
#include "../misrc_gui/signal/gui_rf_preview_geometry.h"
#include <math.h>
#include <stdio.h>

static int failures;

static void expect(int condition, const char *name, const char *message) {
    if (condition) return;
    fprintf(stderr, "%s: %s\n", name, message);
    failures++;
}

static int near(double actual, double expected) {
    return fabs(actual - expected) < .0001 * fmax(1.0, fabs(expected));
}

static void check_frame(const char *name, int frame_width, int frame_height,
                        int active_height, double active_fraction,
                        double expected_aspect, gui_rf_preview_rect_t panel) {
    gui_rf_preview_rect_t draw = gui_rf_preview_fit_frame(frame_width, frame_height,
        panel.x, panel.y, panel.width, panel.height);
    expect(draw.width > 0 && draw.height > 0, name, "nonempty panel displays a frame");
    expect(draw.x >= panel.x - .001f && draw.y >= panel.y - .001f
        && draw.x + draw.width <= panel.x + panel.width + .001f
        && draw.y + draw.height <= panel.y + panel.height + .001f,
        name, "frame fits inside panel");
    expect(near(draw.x + draw.width / 2, panel.x + panel.width / 2)
        && near(draw.y + draw.height / 2, panel.y + panel.height / 2),
        name, "frame is centred in both directions");
    expect(near(draw.width, panel.width) || near(draw.height, panel.height),
        name, "frame fills at least one panel dimension");
    expect(near(draw.width / draw.height, expected_aspect),
        name, "outer frame uses the required display aspect");
    /* A 4:3 subject must retain its shape when Full adds horizontal AND
     * vertical blanking, rather than simply treating all texture pixels as
     * square. This assertion fails for the previous width/height-only fit. */
    double picture_width = draw.width * active_fraction;
    double picture_height = draw.height * active_height / frame_height;
    expect(near(picture_width / picture_height, 4.0 / 3.0),
        name, "active picture remains 4:3 inside the delivered frame");
}

int main(void) {
    const struct {
        const char *name;
        int width, active_height, full_height;
        double active_fraction, full_aspect;
    } profiles[] = {
        {"PAL VHS", 768, 576, 626, .8125, 1.5099533054804621},
        {"SECAM VHS", 768, 576, 626, .8125, 1.5099533054804621},
        {"MESECAM VHS", 768, 576, 626, .8125, 1.5099533054804621},
        {"NTSC VHS", 640, 480, 526, .835576923076923, 1.4561556266325963},
        {"NTSC Betamax", 640, 480, 526, .835576923076923, 1.4561556266325963},
        {"NTSC Betamax Hi-Fi", 640, 480, 526, .835576923076923, 1.4561556266325963},
        {"NTSC SuperBeta", 640, 480, 526, .835576923076923, 1.4561556266325963},
        {"NTSC SuperBeta Hi-Fi", 640, 480, 526, .835576923076923, 1.4561556266325963},
    };
    const gui_rf_preview_rect_t panels[] = {
        {17, 31, 320, 900}, {45, 12, 1700, 500},
        {7, 9, 801, 801}, {2, 4, 91, 49},
    };
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        for (size_t j = 0; j < sizeof(panels) / sizeof(panels[0]); ++j) {
            check_frame(profiles[i].name, profiles[i].width, profiles[i].active_height,
                profiles[i].active_height, 1.0, 4.0 / 3.0, panels[j]);
            check_frame(profiles[i].name, profiles[i].width, profiles[i].full_height,
                profiles[i].active_height, profiles[i].active_fraction,
                profiles[i].full_aspect, panels[j]);
        }
    }
    /* No requested mode/system is consulted: a previous Active texture still
     * displays at 4:3 until the first Full texture arrives, and vice versa. */
    expect(near(gui_rf_preview_display_aspect(768, 576), 4.0 / 3.0)
        && near(gui_rf_preview_display_aspect(640, 480), 4.0 / 3.0),
        "mode changes", "actual Active dimensions retain the previous geometry");
    expect(near(gui_rf_preview_display_aspect(768, 626), 1.5099533054804621)
        && near(gui_rf_preview_display_aspect(640, 526), 1.4561556266325963),
        "mode changes", "actual Full dimensions apply the matching system correction");
    expect(near(gui_rf_preview_display_aspect(320, 200), 1.6),
        "other dimensions", "unrecognised frame dimensions retain natural aspect");
    expect(near(gui_rf_preview_display_aspect(0, 0), 4.0 / 3.0),
        "no frame", "invalid dimensions fall back to 4:3");
    gui_rf_preview_rect_t empty = gui_rf_preview_fit_frame(768, 626, 7, 9, 0, 100);
    expect(empty.x == 7 && empty.y == 9 && empty.width == 0 && empty.height == 0,
        "empty panel", "zero-width panel produces an empty rectangle");
    empty = gui_rf_preview_fit_frame(640, 526, 7, 9, 100, -1);
    expect(empty.width == 0 && empty.height == 0,
        "empty panel", "negative-height panel produces an empty rectangle");
    if (failures) return 1;
    puts("RF display geometry: Active/Full, PAL/SECAM/MESECAM/NTSC/Beta and panel fits passed");
    return 0;
}
