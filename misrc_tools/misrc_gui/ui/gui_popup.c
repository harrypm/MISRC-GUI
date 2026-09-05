/*
 * MISRC GUI - Popup/Modal System Implementation
 *
 * Provides reusable confirmation dialogs and info popups.
 * Uses Clay floating elements for overlay rendering.
 */

#include "gui_popup.h"
#include "gui_ui.h"
#include <clay.h>
#include <raylib.h>
#include <string.h>

//-----------------------------------------------------------------------------
// Colors (popup-specific, extending gui_ui.h colors)
//-----------------------------------------------------------------------------

#define COLOR_POPUP_OVERLAY   (Color){ 0, 0, 0, 180 }
#define COLOR_POPUP_BG        (Color){ 50, 50, 60, 255 }
#define COLOR_POPUP_BORDER    (Color){ 80, 80, 95, 255 }
#define COLOR_POPUP_TITLE     (Color){ 240, 240, 250, 255 }
#define COLOR_POPUP_TEXT      (Color){ 200, 200, 215, 255 }
#define COLOR_POPUP_BTN       (Color){ 70, 70, 82, 255 }
#define COLOR_POPUP_BTN_HOVER (Color){ 90, 90, 105, 255 }
#define COLOR_POPUP_BTN_YES   (Color){ 60, 130, 80, 255 }
#define COLOR_POPUP_BTN_YES_H (Color){ 70, 150, 90, 255 }

//-----------------------------------------------------------------------------
// State
//-----------------------------------------------------------------------------

static struct {
    popup_type_t type;
    char title[64];
    char message[512];
    char yes_text[32];
    char no_text[32];
    popup_result_t result;
    void *user_data;
} s_popup = {0};

//-----------------------------------------------------------------------------
// Helper
//-----------------------------------------------------------------------------

static inline Clay_Color to_clay_color(Color c) {
    return (Clay_Color){ c.r, c.g, c.b, c.a };
}

static Clay_String make_string(const char *str) {
    return (Clay_String){ .isStaticallyAllocated = false, .length = (int32_t)strlen(str), .chars = str };
}

//-----------------------------------------------------------------------------
// API Functions
//-----------------------------------------------------------------------------

void gui_popup_confirm(const char *title, const char *message,
                       const char *yes_text, const char *no_text,
                       void *user_data)
{
    s_popup.type = POPUP_TYPE_CONFIRM;
    strncpy(s_popup.title, title ? title : "Confirm", sizeof(s_popup.title) - 1);
    s_popup.title[sizeof(s_popup.title) - 1] = '\0';
    strncpy(s_popup.message, message ? message : "", sizeof(s_popup.message) - 1);
    s_popup.message[sizeof(s_popup.message) - 1] = '\0';
    strncpy(s_popup.yes_text, yes_text ? yes_text : "Yes", sizeof(s_popup.yes_text) - 1);
    s_popup.yes_text[sizeof(s_popup.yes_text) - 1] = '\0';
    strncpy(s_popup.no_text, no_text ? no_text : "No", sizeof(s_popup.no_text) - 1);
    s_popup.no_text[sizeof(s_popup.no_text) - 1] = '\0';
    s_popup.result = POPUP_RESULT_NONE;
    s_popup.user_data = user_data;
}

void gui_popup_info(const char *title, const char *message)
{
    s_popup.type = POPUP_TYPE_INFO;
    strncpy(s_popup.title, title ? title : "Info", sizeof(s_popup.title) - 1);
    s_popup.title[sizeof(s_popup.title) - 1] = '\0';
    strncpy(s_popup.message, message ? message : "", sizeof(s_popup.message) - 1);
    s_popup.message[sizeof(s_popup.message) - 1] = '\0';
    strncpy(s_popup.yes_text, "OK", sizeof(s_popup.yes_text) - 1);
    s_popup.yes_text[sizeof(s_popup.yes_text) - 1] = '\0';
    s_popup.no_text[0] = '\0';
    s_popup.result = POPUP_RESULT_NONE;
    s_popup.user_data = NULL;
}

bool gui_popup_is_open(void)
{
    return s_popup.type != POPUP_TYPE_NONE && s_popup.result == POPUP_RESULT_NONE;
}

popup_result_t gui_popup_get_result(void)
{
    return s_popup.result;
}

void *gui_popup_get_user_data(void)
{
    return s_popup.user_data;
}

void gui_popup_dismiss(void)
{
    if (s_popup.type != POPUP_TYPE_NONE && s_popup.result == POPUP_RESULT_NONE) {
        s_popup.result = POPUP_RESULT_DISMISSED;
    }
}

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

void gui_popup_render(void)
{
    if (!gui_popup_is_open()) {
        return;
    }

    int popup_max_width =
        gui_ui_modal_max_extent(gui_ui_get_layout_width(), 548);
    int popup_max_height =
        gui_ui_modal_max_extent(gui_ui_get_layout_height(), 600);
    int popup_min_width = popup_max_width < 320 ? popup_max_width : 320;
    int popup_min_height = popup_max_height < 156 ? popup_max_height : 156;
    int popup_message_max_width = popup_max_width > 48 ? popup_max_width - 48 : 1;

    // Full-screen dimming overlay
    CLAY(CLAY_ID("PopupOverlay"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .zIndex = 1000
        },
        .backgroundColor = to_clay_color(COLOR_POPUP_OVERLAY)
    }) {
        // Dialog box
        CLAY(CLAY_ID("PopupDialog"), {
            .layout = {
                .sizing = {
                    CLAY_SIZING_FIT(.min = popup_min_width,
                                    .max = popup_max_width),
                    CLAY_SIZING_FIT(.min = popup_min_height,
                                    .max = popup_max_height)
                },
                .padding = { 24, 24, 20, 20 },
                .childGap = 16,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .backgroundColor = to_clay_color(COLOR_POPUP_BG),
            .cornerRadius = CLAY_CORNER_RADIUS(8),
            .border = {
                .width = { 1, 1, 1, 1 },
                .color = to_clay_color(COLOR_POPUP_BORDER)
            }
        }) {
            // Keep the decision buttons fixed and let the title/message share
            // the only scroll container. This keeps Overwrite/Cancel reachable
            // even at the 320x180 logical minimum used by 200% zoom.
            CLAY(CLAY_ID("PopupContentScroll"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .childGap = 16
                },
                .clip = {
                    .horizontal = true,
                    .vertical = true,
                    .childOffset = Clay_GetScrollOffset()
                }
            }) {
                // Title
                CLAY(CLAY_ID("PopupTitle"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }
                    }
                }) {
                    CLAY_TEXT(make_string(s_popup.title),
                        CLAY_TEXT_CONFIG({
                            .fontSize = FONT_SIZE_HEADING,
                            .textColor = to_clay_color(COLOR_POPUP_TITLE)
                        }));
                }

                // Message
                CLAY(CLAY_ID("PopupMessage"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(.max = popup_message_max_width), CLAY_SIZING_FIT(0) },
                        .padding = { 0, 0, 8, 8 }
                    }
                }) {
                    CLAY_TEXT(make_string(s_popup.message),
                        CLAY_TEXT_CONFIG({
                            .fontSize = FONT_SIZE_NORMAL,
                            .textColor = to_clay_color(COLOR_POPUP_TEXT)
                        }));
                }
            }

            // Button row
            CLAY(CLAY_ID("PopupButtons"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .childGap = 12,
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childAlignment = { .x = CLAY_ALIGN_X_RIGHT, .y = CLAY_ALIGN_Y_CENTER }
                }
            }) {
                // For confirm dialogs: No/Cancel button first (on left)
                if (s_popup.type == POPUP_TYPE_CONFIRM) {
                    bool no_hover = Clay_PointerOver(CLAY_ID("PopupButtonNo"));
                    CLAY(CLAY_ID("PopupButtonNo"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_FIT(.min = 80), CLAY_SIZING_FIXED(36) },
                            .padding = { 16, 16, 0, 0 },
                            .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                        },
                        .backgroundColor = to_clay_color(no_hover ? COLOR_POPUP_BTN_HOVER : COLOR_POPUP_BTN),
                        .cornerRadius = CLAY_CORNER_RADIUS(4)
                    }) {
                        CLAY_TEXT(make_string(s_popup.no_text),
                            CLAY_TEXT_CONFIG({
                                .fontSize = FONT_SIZE_NORMAL,
                                .textColor = to_clay_color(COLOR_TEXT)
                            }));
                    }
                }

                // Yes/OK button (primary action, on right)
                bool yes_hover = Clay_PointerOver(CLAY_ID("PopupButtonYes"));
                CLAY(CLAY_ID("PopupButtonYes"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(.min = 80), CLAY_SIZING_FIXED(36) },
                        .padding = { 16, 16, 0, 0 },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(yes_hover ? COLOR_POPUP_BTN_YES_H : COLOR_POPUP_BTN_YES),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    CLAY_TEXT(make_string(s_popup.yes_text),
                        CLAY_TEXT_CONFIG({
                            .fontSize = FONT_SIZE_NORMAL,
                            .textColor = to_clay_color(COLOR_TEXT)
                        }));
                }
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Interaction Handling
//-----------------------------------------------------------------------------

bool gui_popup_handle_interactions(void)
{
    if (!gui_popup_is_open()) {
        return false;
    }

    // If popup is open, consume all mouse interactions (modal behavior)
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        // Check Yes/OK button
        if (Clay_PointerOver(CLAY_ID("PopupButtonYes"))) {
            s_popup.result = POPUP_RESULT_YES;
            return true;
        }

        // Check No/Cancel button (confirm dialog only)
        if (s_popup.type == POPUP_TYPE_CONFIRM && Clay_PointerOver(CLAY_ID("PopupButtonNo"))) {
            s_popup.result = POPUP_RESULT_NO;
            return true;
        }

        // Check if clicked on dialog (don't dismiss)
        if (Clay_PointerOver(CLAY_ID("PopupDialog"))) {
            return true;
        }

        // Clicked on overlay (outside dialog) - dismiss
        if (Clay_PointerOver(CLAY_ID("PopupOverlay"))) {
            s_popup.result = POPUP_RESULT_DISMISSED;
            return true;
        }
    }

    // Popup is open, consume interactions to block underlying UI
    return true;
}
