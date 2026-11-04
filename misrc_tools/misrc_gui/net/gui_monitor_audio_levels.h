/* Monitor & Remote-only four-channel audio peak status extension.
 * This is metadata, never a raw audio stream. Legacy MISRC-M/2 servers can
 * omit the optional fields: the caller then displays zero, not stale peaks. */
#ifndef GUI_MONITOR_AUDIO_LEVELS_H
#define GUI_MONITOR_AUDIO_LEVELS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUI_MONITOR_AUDIO_LEVEL_CHANNELS 4
#define GUI_MONITOR_AUDIO_PEAK_MAX 8388608u /* absolute min of signed 24-bit */

static inline bool gui_monitor_audio_levels_parse_status(
    const char *body, uint32_t peaks[GUI_MONITOR_AUDIO_LEVEL_CHANNELS])
{
    if (!peaks) return false;
    memset(peaks, 0, GUI_MONITOR_AUDIO_LEVEL_CHANNELS * sizeof(peaks[0]));
    if (!body) return false;

    const char *const names[GUI_MONITOR_AUDIO_LEVEL_CHANNELS] = {
        "audio_peak_ch1", "audio_peak_ch2", "audio_peak_ch3", "audio_peak_ch4"
    };
    uint32_t parsed[GUI_MONITOR_AUDIO_LEVEL_CHANNELS];
    for (unsigned ch = 0; ch < GUI_MONITOR_AUDIO_LEVEL_CHANNELS; ch++) {
        char key[32];
        snprintf(key, sizeof(key), "\"%s\":", names[ch]);
        const char *value = strstr(body, key);
        if (!value) return false;
        value += strlen(key);
        if (*value < '0' || *value > '9') return false;
        char *end = NULL;
        unsigned long long peak = strtoull(value, &end, 10);
        if (end == value || (*end != ',' && *end != '}' && *end != '\n') ||
            peak > GUI_MONITOR_AUDIO_PEAK_MAX)
            return false;
        parsed[ch] = (uint32_t)peak;
    }
    memcpy(peaks, parsed, sizeof(parsed));
    return true;
}

#endif /* GUI_MONITOR_AUDIO_LEVELS_H */
