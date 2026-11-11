/* Dependency-free regression tests for optional Monitor & Remote CH1-CH4
 * audio peak metadata (no raw audio transport or GUI dependency). */
#include "../misrc_gui/net/gui_monitor_audio_levels.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

static void expect_zero(const uint32_t peaks[4])
{
    for (int ch = 0; ch < 4; ch++) assert(peaks[ch] == 0);
}

int main(void)
{
    uint32_t peaks[4] = {1, 1, 1, 1};
    const char *valid =
        "{\"protocol\":\"MISRC-M/2\",\"audio_peak_ch1\":123,"
        "\"audio_peak_ch2\":8388607,\"audio_peak_ch3\":8388608,"
        "\"audio_peak_ch4\":0,\"aud_pct\":45}";
    assert(gui_monitor_audio_levels_parse_status(valid, peaks));
    assert(peaks[0] == 123 && peaks[1] == 8388607 &&
           peaks[2] == 8388608 && peaks[3] == 0);

    /* An old server has no peak fields: do not retain previous readings. */
    assert(!gui_monitor_audio_levels_parse_status(
        "{\"protocol\":\"MISRC-M/2\",\"aud_pct\":10}", peaks));
    expect_zero(peaks);

    /* Partial, invalid, negative or out-of-range snapshots fail atomically. */
    assert(!gui_monitor_audio_levels_parse_status(
        "{\"audio_peak_ch1\":1,\"audio_peak_ch2\":2,\"audio_peak_ch3\":3}", peaks));
    expect_zero(peaks);
    assert(!gui_monitor_audio_levels_parse_status(
        "{\"audio_peak_ch1\":1,\"audio_peak_ch2\":-1,"
        "\"audio_peak_ch3\":3,\"audio_peak_ch4\":4}", peaks));
    expect_zero(peaks);
    assert(!gui_monitor_audio_levels_parse_status(
        "{\"audio_peak_ch1\":1,\"audio_peak_ch2\":2,"
        "\"audio_peak_ch3\":3,\"audio_peak_ch4\":8388609}", peaks));
    expect_zero(peaks);
    assert(!gui_monitor_audio_levels_parse_status(
        "{\"audio_peak_ch1\":1,\"audio_peak_ch2\":2,"
        "\"audio_peak_ch3\":3,\"audio_peak_ch4\":4x}", peaks));
    expect_zero(peaks);
    assert(!gui_monitor_audio_levels_parse_status(NULL, peaks));
    expect_zero(peaks);
    assert(!gui_monitor_audio_levels_parse_status(valid, NULL));
    puts("Monitor audio level status regression tests passed");
    return 0;
}
