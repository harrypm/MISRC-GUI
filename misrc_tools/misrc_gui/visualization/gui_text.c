/*
 * MISRC GUI - Text Rendering Utilities Implementation
 *
 * Centralized text rendering with custom font support.
 */

#include "gui_text.h"
#include "../ui/gui_font.h"

// App reference for font access
static gui_app_t *s_text_app = NULL;

//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------

void gui_text_set_app(gui_app_t *app) {
    s_text_app = app;
}

//-----------------------------------------------------------------------------
// Text Drawing
//-----------------------------------------------------------------------------

void gui_text_draw(const char *text, float x, float y, int fontSize, Color color) {
    if (s_text_app && s_text_app->fonts) {
        Font font = s_text_app->fonts[0];  // Index 0 = Inter (primary)
        gui_font_draw_text(font, text, (Vector2){x, y}, (float)fontSize, 1.0f, color);
    } else {
        gui_font_draw_text(GetFontDefault(), text, (Vector2){x, y}, (float)fontSize, 1.0f, color);
    }
}

void gui_text_draw_mono(const char *text, float x, float y, int fontSize, Color color) {
    if (s_text_app && s_text_app->fonts) {
        Font font = s_text_app->fonts[1];  // Index 1 = Space Mono
        gui_font_draw_text(font, text, (Vector2){x, y}, (float)fontSize, 1.0f, color);
    } else {
        gui_font_draw_text(GetFontDefault(), text, (Vector2){x, y}, (float)fontSize, 1.0f, color);
    }
}

//-----------------------------------------------------------------------------
// Text Measurement
//-----------------------------------------------------------------------------

int gui_text_measure(const char *text, int fontSize) {
    if (s_text_app && s_text_app->fonts) {
        Font font = s_text_app->fonts[0];  // Index 0 = Inter (primary)
        Vector2 size = gui_font_measure_text(font, text, (float)fontSize, 1.0f);
        return (int)size.x;
    }
    return (int)gui_font_measure_text(GetFontDefault(), text, (float)fontSize, 1.0f).x;
}

int gui_text_measure_mono(const char *text, int fontSize) {
    if (s_text_app && s_text_app->fonts) {
        Font font = s_text_app->fonts[1];  // Index 1 = Space Mono
        Vector2 size = gui_font_measure_text(font, text, (float)fontSize, 1.0f);
        return (int)size.x;
    }
    return (int)gui_font_measure_text(GetFontDefault(), text, (float)fontSize, 1.0f).x;
}
