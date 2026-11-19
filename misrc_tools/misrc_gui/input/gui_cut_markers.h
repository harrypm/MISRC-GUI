/*
 * MISRC GUI - playback cut markers (IN / OUT) for FLAC-Chop
 *
 * harrypm 2026-11-19: "add a marker option icon next to the play/loop icon
 * for cutting". One IN and one OUT per playback channel, in real RF samples
 * (the playhead's units, and FLAC-Chop's --units samples). Marks belong to
 * the file they were set on: loading another file hides them.
 *
 * UI thread only (no locking).
 */

#ifndef GUI_CUT_MARKERS_H
#define GUI_CUT_MARKERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool has_in, has_out;
    uint64_t in, out;   // samples; when both are set, in < out
} gui_cut_marks_t;

typedef enum {
    GUI_CUT_SET_OK = 0,
    GUI_CUT_SET_DROPPED_OTHER   // the new mark crossed the other one, which was cleared
} gui_cut_set_result_t;

// Marks of channel ch (0 = A, 1 = B) for the file at path. Empty when the
// marks were set on another file (or path is NULL / empty).
gui_cut_marks_t gui_cut_marks_get(int ch, const char *path);

// Set IN (out_mark false) or OUT at pos on path. IN at or after OUT drops
// OUT; OUT at or before IN drops IN (reported as GUI_CUT_SET_DROPPED_OTHER).
gui_cut_set_result_t gui_cut_marks_set(int ch, const char *path, bool out_mark, uint64_t pos);

void gui_cut_marks_clear(int ch);

// Launch preset (--markers): marks that bind to the first file the channel
// shows. in / out of UINT64_MAX leave that mark unset.
void gui_cut_marks_preset(int ch, uint64_t in, uint64_t out);

// FLAC-Chop launch line for path with the set marks:
//   flac-chop --gui "<path>" [--in N] [--out N] --units samples
// The path is quoted for the host shell (Windows "..", POSIX '..').
// False when path is empty or buf is too small.
bool gui_cut_marks_flac_chop_command(const gui_cut_marks_t *m, const char *path,
                                     char *buf, size_t size);

#endif // GUI_CUT_MARKERS_H
