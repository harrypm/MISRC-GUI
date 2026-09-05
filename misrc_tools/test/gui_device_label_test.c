#include "../misrc_gui/ui/gui_device_label.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    const struct {
        ddd_device_profile_t profile;
        bool clockgen;
        const char *full;
        const char *compact;
    } cases[] = {
        {DDD_DEVICE_PROTOCOL_V1, false, "[DdD] Domesday Duplicator", "[DdD] DdD"},
        {DDD_DEVICE_LEGACY, false, "[DdD] Domesday Duplicator (legacy)", "[DdD] LEG"},
        {DDD_DEVICE_PROTOCOL_V1, true,
         "[DdD] Domesday Duplicator + Clockgen", "[DdD] +CG"},
        {DDD_DEVICE_LEGACY, true,
         "[DdD] Domesday Duplicator (legacy) + Clockgen", "[DdD] LEG+CG"},
        {DDD_DEVICE_UNSUPPORTED, false,
         "[DdD] Domesday Duplicator (unsupported)", "[DdD] unsupported"},
        {DDD_DEVICE_UNSUPPORTED, true,
         "[DdD] Domesday Duplicator (unsupported) + Clockgen", "[DdD] unsupported+CG"},
        {(ddd_device_profile_t)99, false,
         "[DdD] Domesday Duplicator (unsupported)", "[DdD] unsupported"},
        {(ddd_device_profile_t)99, true,
         "[DdD] Domesday Duplicator (unsupported) + Clockgen", "[DdD] unsupported+CG"},
    };
    char label[64];
    char original[] = "[DdD] Domesday Duplicator (legacy firmware)";

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gui_device_format_label(original, cases[i].profile,
                                cases[i].clockgen, false, label, sizeof(label));
        CHECK(strcmp(label, cases[i].full) == 0);
        CHECK(strncmp(label, "[DdD] ", 6) == 0);
        gui_device_format_label(original, cases[i].profile,
                                cases[i].clockgen, true, label, sizeof(label));
        CHECK(strcmp(label, cases[i].compact) == 0);
        CHECK(strncmp(label, "[DdD] ", 6) == 0);
        CHECK(strlen(label) <= 20);
        /* Switching back must restore the full identity, not a prior
         * abbreviated or truncated display buffer. */
        gui_device_format_label(original, cases[i].profile,
                                cases[i].clockgen, false, label, sizeof(label));
        CHECK(strcmp(label, cases[i].full) == 0);
    }
    CHECK(strcmp(original,
                 "[DdD] Domesday Duplicator (legacy firmware)") == 0);

    const char *other_names[] = {
        "MISRC", "[CXADC] CXADC Clockgen", "[MISRC] MISRC Clockgen",
        "[RTL-SDR] RTL2838 #1", "[RTL-SDR] RTL2838 #2", "Test Signal",
        "[DdD] text in a non-DdD name",
    };
    for (size_t i = 0; i < sizeof(other_names) / sizeof(other_names[0]); i++) {
        gui_device_format_label(other_names[i], DDD_DEVICE_NOT_DDD,
                                false, false, label, sizeof(label));
        CHECK(strcmp(label, other_names[i]) == 0);
        gui_device_format_label(other_names[i], DDD_DEVICE_NOT_DDD,
                                true, true, label, sizeof(label));
        CHECK(strcmp(label, other_names[i]) == 0);
    }

    /* Size checks include bytes beyond the output range to detect overruns. */
    char bounded[8] = "guarded";
    gui_device_format_label(original, DDD_DEVICE_LEGACY,
                            true, false, bounded, 4);
    CHECK(memcmp(bounded, "[Dd\0ded", sizeof(bounded)) == 0);
    gui_device_format_label("0123456789", DDD_DEVICE_NOT_DDD,
                            false, false, bounded, 4);
    CHECK(memcmp(bounded, "012\0ded", sizeof(bounded)) == 0);
    gui_device_format_label(original, DDD_DEVICE_PROTOCOL_V1,
                            false, false, bounded, 4);
    CHECK(strcmp(bounded, "[Dd") == 0);
    gui_device_format_label(original, DDD_DEVICE_LEGACY,
                            false, false, bounded, 1);
    CHECK(bounded[0] == '\0' && bounded[1] == 'D');
    bounded[0] = 'X';
    gui_device_format_label(original, DDD_DEVICE_LEGACY,
                            false, false, bounded, 0);
    CHECK(bounded[0] == 'X');
    gui_device_format_label(original, DDD_DEVICE_LEGACY,
                            false, false, NULL, sizeof(label));
    gui_device_format_label(NULL, DDD_DEVICE_NOT_DDD,
                            false, false, label, sizeof(label));
    CHECK(label[0] == '\0');
    gui_device_format_label(NULL, DDD_DEVICE_LEGACY,
                            false, false, label, sizeof(label));
    CHECK(strcmp(label, "[DdD] Domesday Duplicator (legacy)") == 0);

    puts("Device selector label tests passed");
    return 0;
}
