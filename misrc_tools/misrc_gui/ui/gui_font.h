#ifndef GUI_FONT_H
#define GUI_FONT_H

#include <stddef.h>
#include "raylib.h"

/* UI-thread only. Keep the embedded fonts for their existing glyphs and lazily
 * cache missing glyphs from installed fonts. Measurement and drawing must use
 * the same fallback, including Clay slices which are not NUL terminated. */
Vector2 gui_font_measure_text(Font font, const char *text, float size, float spacing);
Vector2 gui_font_measure_text_n(Font font, const char *text, size_t length, float size, float spacing);
void gui_font_draw_text(Font font, const char *text, Vector2 position, float size, float spacing, Color tint);
void gui_font_unload(void);

#endif
