/*
 * MISRC GUI - gui_playback selected-file-info stub (CPU-only layout harnesses)
 *
 * gui_ui.c's playback-timeline name budget reads the selected playback
 * file's info (gui_playback_get_selected_file_info). The stats/toolbar/
 * viewport layout harnesses compile gui_ui.c WITHOUT gui_playback.c, and LTO
 * only drops calls the harness never reaches - the timeline layout function
 * IS reached, so the symbol must resolve at link. This TU satisfies it.
 *
 * Kept SEPARATE from gui_playback_audio_stub.c because the playback
 * harnesses (resolve/seek/start/reconnect/watchdog) compile the REAL
 * gui_playback.c and also link the audio stub; a file-info definition there
 * collided with the real one (multiple definition).
 */
#include <string.h>

#include "../misrc_gui/input/gui_playback.h"

bool gui_playback_get_selected_file_info(gui_app_t *app, int channel,
                                         playback_file_info_t *info) {
    (void)app; (void)channel;
    if (info) memset(info, 0, sizeof(*info));
    return false;
}
