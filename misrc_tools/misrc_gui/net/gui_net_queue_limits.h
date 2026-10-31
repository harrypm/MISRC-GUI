/* Shared limits for the legacy full-bandwidth RF/Audio HTTP fanout.
 * Independent of the UI's capture/recording Memory budget.
 * The Monitor & Remote preview protocol does not use this fanout.
 */
#ifndef GUI_NET_QUEUE_LIMITS_H
#define GUI_NET_QUEUE_LIMITS_H

#include <stddef.h>
#include <stdint.h>

#define GUI_NET_RF_QUEUE_LIMIT_BYTES    ((size_t)256U * 1024U * 1024U)
#define GUI_NET_AUDIO_QUEUE_LIMIT_BYTES ((size_t)64U * 1024U * 1024U)
#define GUI_NET_QUEUE_WARN_PERCENT 75U

/* Use a widened multiplication to keep percentages correct on 32-bit builds. */
static inline unsigned gui_net_queue_percent(size_t queued, size_t limit)
{
    if (limit == 0) return 0;
    if (queued >= limit) return 100;
    return (unsigned)(((uint64_t)queued * 100U) / (uint64_t)limit);
}

static inline size_t gui_net_queue_warn_bytes(size_t limit)
{
    return (limit / 100U) * GUI_NET_QUEUE_WARN_PERCENT;
}

#endif /* GUI_NET_QUEUE_LIMITS_H */
