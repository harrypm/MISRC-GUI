/*
 * Source metadata: is a channel's RF a DomesdayDuplicator (DdD) capture?
 *
 * The CVBS decoder's 50 Ω mode (upstream PR #31) corrects the DdD's 50 Ω
 * AC-coupled input; harrypm 2026-11-25: "it should only be enabled if a DdD
 * device is being used or a DdD metadata noted file is being used".
 *
 * A channel is DdD when:
 * - the selected capture device is a DdD, or
 * - the playback file for that channel is DdD:
 *   - a packed .lds or an ld-compress .ldf (DdD formats), or
 *   - a DdD application JSON sidecar (<name>.json with "captureInfo"), or
 *   - a MISRC-GUI capture log next to it (<base>_<timestamp>_misrc_capture.log)
 *     recording "capture_device_type: ddd".
 */
#ifndef GUI_SOURCE_META_H
#define GUI_SOURCE_META_H

#include <stdbool.h>

struct gui_app;

/* File test only (no caching): the playback-file half of the rule. */
bool gui_source_file_is_ddd(const char *path);

/* Device or file, per channel (0 = A, 1 = B). Main thread; the file
 * verdict is cached per path (re-checked when the file changes). */
bool gui_source_is_ddd(struct gui_app *app, int channel);

#endif
