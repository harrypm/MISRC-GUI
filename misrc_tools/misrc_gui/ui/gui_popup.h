/*
 * MISRC GUI - Popup/Modal System
 *
 * Provides reusable confirmation dialogs and info popups.
 * Follows the dropdown pattern with centralized state management.
 */

#ifndef GUI_POPUP_H
#define GUI_POPUP_H

#include <stdbool.h>
#include <stddef.h>

//-----------------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------------

typedef enum {
    POPUP_TYPE_NONE = 0,
    POPUP_TYPE_CONFIRM,      // Yes/No or custom button text
    POPUP_TYPE_INFO,         // OK only (informational)
} popup_type_t;

typedef enum {
    POPUP_RESULT_NONE = 0,   // Popup still open, no decision yet
    POPUP_RESULT_YES,        // User clicked Yes/OK/primary button
    POPUP_RESULT_NO,         // User clicked No/Cancel/secondary button
    POPUP_RESULT_DISMISSED   // User pressed ESC or clicked outside
} popup_result_t;

//-----------------------------------------------------------------------------
// API Functions
//-----------------------------------------------------------------------------

/* Show a confirmation popup with two buttons.
 *
 * @param title      Dialog title (max 63 chars)
 * @param message    Dialog message (max 511 chars)
 * @param yes_text   Primary button text (e.g., "Yes", "Overwrite")
 * @param no_text    Secondary button text (e.g., "No", "Cancel")
 * @param user_data  Optional context pointer, retrievable via gui_popup_get_user_data()
 *
 * Non-blocking: returns immediately. Check gui_popup_get_result() on subsequent frames.
 */
void gui_popup_confirm(const char *title, const char *message,
                       const char *yes_text, const char *no_text,
                       void *user_data);

/* Show an info popup with a single OK button.
 *
 * @param title      Dialog title (max 63 chars)
 * @param message    Dialog message (max 511 chars)
 *
 * Non-blocking: returns immediately. Check gui_popup_get_result() on subsequent frames.
 */
void gui_popup_info(const char *title, const char *message);

/* Check if a popup is currently open.
 *
 * @return true if popup is visible, false otherwise
 */
bool gui_popup_is_open(void);

/* Get the popup result.
 *
 * @return POPUP_RESULT_NONE while popup is open, otherwise the user's choice.
 *         Result persists after popup closes until a new popup is opened.
 */
popup_result_t gui_popup_get_result(void);

/* Get the user_data from the current/last popup.
 *
 * @return The user_data pointer passed to gui_popup_confirm(), or NULL
 */
void *gui_popup_get_user_data(void);

/* Close the popup and set result to DISMISSED.
 *
 * Called by ESC key handler or clicking outside the dialog.
 */
void gui_popup_dismiss(void);

//-----------------------------------------------------------------------------
// Integration Functions (called by gui_ui.c)
//-----------------------------------------------------------------------------

/* Render the popup overlay.
 *
 * Call at the end of gui_render_layout() to render on top of everything.
 * Only renders if a popup is currently open.
 */
void gui_popup_render(void);

/* Handle popup button interactions.
 *
 * Call at the start of gui_handle_interactions() for modal behavior.
 *
 * @return true if popup consumed the interaction (caller should skip other UI handling)
 */
bool gui_popup_handle_interactions(void);

#endif // GUI_POPUP_H
