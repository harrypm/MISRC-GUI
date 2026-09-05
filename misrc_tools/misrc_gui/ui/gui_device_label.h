/* MISRC GUI - Full and compact device labels for the device selector. */

#ifndef GUI_DEVICE_LABEL_H
#define GUI_DEVICE_LABEL_H

#include <stdbool.h>
#include <stddef.h>

#include "../../common/ddd_protocol.h"

/* This only formats display text; the original device name and identity stay
 * unchanged. Pass DDD_DEVICE_NOT_DDD to preserve other devices' names, and use
 * the existing Clockgen mode helper for clockgen. Compact labels retain the
 * firmware distinction with LEG for legacy firmware and abbreviate Clockgen.
 * Normal labels retain the full Domesday Duplicator name. The output is always
 * terminated when label is non-NULL and label_size is nonzero. */
void gui_device_format_label(const char *name,
                             ddd_device_profile_t profile,
                             bool clockgen,
                             bool compact,
                             char *label,
                             size_t label_size);

#endif /* GUI_DEVICE_LABEL_H */
