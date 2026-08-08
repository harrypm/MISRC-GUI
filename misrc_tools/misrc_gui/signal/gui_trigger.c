/*
 * MISRC GUI - Trigger Detection Module
 *
 * Centralized trigger detection for oscilloscope and CVBS decoder.
 */

#include "gui_trigger.h"

//-----------------------------------------------------------------------------
// CVBS Signal Level Analysis (Histogram-Based)
//-----------------------------------------------------------------------------

// Build a histogram of sample values, returns min/max found
static void build_sample_histogram(const int16_t *buf, size_t count,
                                   uint32_t *hist, int num_bins,
                                   int16_t *out_min, int16_t *out_max) {
    // First pass: find min/max (sample every 8th for speed)
    int16_t sig_min = buf[0];
    int16_t sig_max = buf[0];
    for (size_t i = 0; i < count; i += 8) {
        if (buf[i] < sig_min) sig_min = buf[i];
        if (buf[i] > sig_max) sig_max = buf[i];
    }
    *out_min = sig_min;
    *out_max = sig_max;

    // Clear histogram
    for (int i = 0; i < num_bins; i++) hist[i] = 0;

    // Build histogram (sample every 4th for balance of speed/accuracy)
    int range = sig_max - sig_min;
    if (range < 1) range = 1;
    float scale = (float)(num_bins - 1) / (float)range;

    for (size_t i = 0; i < count; i += 4) {
        int bin = (int)((buf[i] - sig_min) * scale);
        if (bin < 0) bin = 0;
        if (bin >= num_bins) bin = num_bins - 1;
        hist[bin]++;
    }
}

// Minimum peak separation as fraction of total bins (5% = sync-to-blanking gap)
#define CVBS_MIN_PEAK_SEPARATION 0.05f

// Find two lowest peaks in histogram (sync tip and blanking level)
// Returns true if both peaks found, false otherwise
static bool find_two_lowest_peaks(const uint32_t *hist, int num_bins,
                                  size_t sample_count,
                                  int *peak1_bin, int *peak2_bin) {
    (void)sample_count;

    // Calculate mean of histogram
    uint64_t sum = 0;
    for (int i = 0; i < num_bins; i++) {
        sum += hist[i];
    }
    uint32_t mean = (uint32_t)(sum / num_bins);

    // Find first peak above mean (sync tip)
    int p1 = -1;
    for (int i = 0; i < num_bins; i++) {
        if (hist[i] > mean) {
            // Found start of peak region, find local maximum
            uint32_t max_val = hist[i];
            int max_idx = i;
            while (i < num_bins && hist[i] > mean) {
                if (hist[i] > max_val) {
                    max_val = hist[i];
                    max_idx = i;
                }
                i++;
            }
            p1 = max_idx;
            break;
        }
    }

    if (p1 < 0) return false;

    // Find second peak above mean (blanking level)
    int p2 = -1;
    for (int i = p1 + 1; i < num_bins; i++) {
        if (hist[i] > mean) {
            // Found start of peak region, find local maximum
            uint32_t max_val = hist[i];
            int max_idx = i;
            while (i < num_bins && hist[i] > mean) {
                if (hist[i] > max_val) {
                    max_val = hist[i];
                    max_idx = i;
                }
                i++;
            }
            p2 = max_idx;
            break;
        }
    }

    if (p2 < 0) return false;

    // Validate peak separation (must be at least 5% of range apart)
    int min_separation = (int)(num_bins * CVBS_MIN_PEAK_SEPARATION);
    if ((p2 - p1) < min_separation) {
        return false;  // Peaks too close together, likely noise
    }

    *peak1_bin = p1;
    *peak2_bin = p2;
    return true;
}

void trigger_analyze_cvbs_levels(const int16_t *buf, size_t count,
                                  cvbs_levels_t *levels) {
    if (!buf || count == 0 || !levels) return;

    // Stack-allocated histogram (256 bins = 1KB)
    uint32_t hist[CVBS_HIST_BINS];
    int16_t sig_min, sig_max;

    // Build histogram and get min/max
    build_sample_histogram(buf, count, hist, CVBS_HIST_BINS, &sig_min, &sig_max);

    levels->sig_min = sig_min;
    levels->sig_max = sig_max;
    levels->range = sig_max - sig_min;

    // Need minimum signal range
    if (levels->range < 100) {
        // Signal too weak, set threshold to middle
        levels->sync_threshold = sig_min + levels->range / 2;
        levels->black_level = levels->sync_threshold;
        levels->white_level = sig_max;
        return;
    }

    // Find sync tip and blanking level peaks
    int peak1_bin, peak2_bin;
    if (find_two_lowest_peaks(hist, CVBS_HIST_BINS, count, &peak1_bin, &peak2_bin)) {
        // Convert bin indices back to sample values
        float bin_to_val = (float)levels->range / (float)(CVBS_HIST_BINS - 1);
        int16_t sync_tip = sig_min + (int16_t)(peak1_bin * bin_to_val);
        int16_t blanking = sig_min + (int16_t)(peak2_bin * bin_to_val);

        // Sync threshold is midpoint between sync tip and blanking
        levels->sync_threshold = (sync_tip + blanking) / 2;
        levels->black_level = blanking;

        // White level is 90% of way from blanking to max
        levels->white_level = blanking + (int16_t)((sig_max - blanking) * 0.90f);
    } else {
        // Couldn't find two peaks - set invalid state
        // (range set to 0 signals detection failure to caller)
        levels->range = 0;
        levels->sync_threshold = sig_min;
        levels->black_level = sig_min;
        levels->white_level = sig_max;
    }
}

//-----------------------------------------------------------------------------
// Edge Trigger Detection
//-----------------------------------------------------------------------------

ssize_t trigger_find_rising_edge(const int16_t *buf, size_t count,
                                  int16_t level, size_t min_index) {
    if (!buf || count < 2) return -1;

    size_t start = (min_index > 1) ? min_index : 1;

    for (size_t i = start; i < count; i++) {
        int16_t prev = buf[i - 1];
        int16_t curr = buf[i];

        // Rising edge: cross from below to at-or-above level
        if (prev < level && curr >= level) {
            return (ssize_t)i;
        }
    }

    return -1;
}

ssize_t trigger_find_falling_edge(const int16_t *buf, size_t count,
                                   int16_t level, size_t min_index) {
    if (!buf || count < 2) return -1;

    size_t start = (min_index > 1) ? min_index : 1;

    for (size_t i = start; i < count; i++) {
        int16_t prev = buf[i - 1];
        int16_t curr = buf[i];

        // Falling edge: cross from above to at-or-below level
        if (prev > level && curr <= level) {
            return (ssize_t)i;
        }
    }

    return -1;
}

//-----------------------------------------------------------------------------
// CVBS Sync Detection
//-----------------------------------------------------------------------------

// Internal: scan for hsync pulses, optionally collecting all positions
// If out_positions is NULL, returns after first hsync found
// Returns count of hsyncs found, or -1 on error
static ssize_t find_hsyncs_internal(const int16_t *buf, size_t count,
                                    size_t min_index, int16_t threshold,
                                    size_t *out_positions, size_t max_count) {
    size_t start = (min_index > 1) ? min_index : 1;
    size_t limit = count - CVBS_HSYNC_MAX_WIDTH;
    size_t found = 0;

    for (size_t i = start; i < limit; i++) {
        // Falling edge into sync pulse
        if (buf[i - 1] > threshold && buf[i] <= threshold) {
            // Measure pulse width
            size_t end = i;
            while (end < count && buf[end] <= threshold) end++;

            size_t width = end - i;
            if (width >= CVBS_HSYNC_MIN_WIDTH && width <= CVBS_HSYNC_MAX_WIDTH) {
                if (out_positions) {
                    if (found < max_count) {
                        out_positions[found] = end;
                    }
                    found++;
                } else {
                    return (ssize_t)end;  // Return first hsync position
                }
            }
            i = end;  // Skip past this pulse
        }
    }

    return out_positions ? (ssize_t)found : -1;
}

ssize_t trigger_find_cvbs_hsync(const int16_t *buf, size_t count,
                                 size_t min_index) {
    if (!buf || count < CVBS_HSYNC_MAX_WIDTH + 2) return -1;

    cvbs_levels_t levels;
    trigger_analyze_cvbs_levels(buf, count, &levels);
    if (levels.range < 100) return -1;

    return find_hsyncs_internal(buf, count, min_index, levels.sync_threshold,
                                NULL, 0);
}

ssize_t trigger_find_all_cvbs_hsyncs(const int16_t *buf, size_t count,
                                      size_t *out_positions, size_t max_count) {
    if (!buf || count < CVBS_HSYNC_MAX_WIDTH + 2 || !out_positions || max_count == 0)
        return -1;

    cvbs_levels_t levels;
    trigger_analyze_cvbs_levels(buf, count, &levels);
    if (levels.range < 100) return -1;

    return find_hsyncs_internal(buf, count, 1, levels.sync_threshold,
                                out_positions, max_count);
}

//-----------------------------------------------------------------------------
// Main Trigger Dispatch
//-----------------------------------------------------------------------------

ssize_t trigger_find_from_config(const int16_t *buf, size_t count,
                                  const channel_trigger_t *trig, size_t min_index) {
    if (!trig || !trig->enabled || !buf || count < 2) return -1;

    switch (trig->trigger_mode) {
        case TRIGGER_MODE_RISING:
            return trigger_find_rising_edge(buf, count, trig->level, min_index);
        case TRIGGER_MODE_FALLING:
            return trigger_find_falling_edge(buf, count, trig->level, min_index);
        case TRIGGER_MODE_CVBS_HSYNC:
            return trigger_find_cvbs_hsync(buf, count, min_index);
        default:
            return trigger_find_rising_edge(buf, count, trig->level, min_index);
    }
}
