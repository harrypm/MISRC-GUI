#include "gui_device_label.h"

#include <stdio.h>

void gui_device_format_label(const char *name,
                             ddd_device_profile_t profile,
                             bool clockgen,
                             bool compact,
                             char *label,
                             size_t label_size)
{
    if (!label || label_size == 0) return;

    if (profile == DDD_DEVICE_NOT_DDD) {
        snprintf(label, label_size, "%s", name ? name : "");
        return;
    }

    const char *base;
    switch (profile) {
        case DDD_DEVICE_LEGACY:
            base = compact ? "[DdD] LEG" : "[DdD] Domesday Duplicator (legacy)";
            break;
        case DDD_DEVICE_PROTOCOL_V1:
            base = compact ? (clockgen ? "[DdD] " : "[DdD] DdD")
                           : "[DdD] Domesday Duplicator";
            break;
        default:
            base = compact ? "[DdD] unsupported" : "[DdD] Domesday Duplicator (unsupported)";
            break;
    }

    snprintf(label, label_size, "%s%s", base,
             clockgen ? (compact ? "+CG" : " + Clockgen") : "");
}
