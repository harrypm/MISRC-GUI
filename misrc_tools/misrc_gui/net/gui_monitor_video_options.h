#ifndef GUI_MONITOR_VIDEO_OPTIONS_H
#define GUI_MONITOR_VIDEO_OPTIONS_H
#include <stdbool.h>
#include <stdint.h>
/* Monitor-only wire settings. Harry's existing decoder and UI structures are
 * intentionally unchanged. Values are validated before entering worker threads.
 * A new upstream control must be manually added to this monitor adapter. */
enum {
    MVD_CTL_MODE=0, MVD_CTL_SYSTEM=2, MVD_CTL_FRAME=4,
    MVD_CTL_LEVELS=5, MVD_CTL_SYNC=6, MVD_CTL_COLOUR=7,
    MVD_CTL_TAPE=8, MVD_CTL_BETA=9, MVD_CTL_MESECAM=11,
    MVD_CTL_BUDGET=12, MVD_CTL_TAPE_SYSTEM=15, MVD_CTL_TAPE_FAMILY=17,
    MVD_CTL_TAPE_SPEED=20, MVD_CTL_OSD=22, MVD_CTL_STATS=24
};
static inline unsigned mvd_ctl_get(uint32_t ctl, unsigned shift, unsigned mask) {
    return (ctl >> shift) & mask;
}
static inline uint32_t mvd_ctl_set(uint32_t ctl, unsigned shift,
                                   unsigned mask, unsigned value) {
    return (ctl & ~((uint32_t)mask << shift)) |
           ((uint32_t)(value & mask) << shift);
}
static inline uint32_t mvd_ctl_default(unsigned mode) {
    uint32_t ctl=0;
    ctl=mvd_ctl_set(ctl,MVD_CTL_MODE,3u,mode);
    ctl=mvd_ctl_set(ctl,MVD_CTL_SYNC,1u,1u);   /* Fixed */
    ctl=mvd_ctl_set(ctl,MVD_CTL_COLOUR,1u,1u); /* Auto */
    ctl=mvd_ctl_set(ctl,MVD_CTL_OSD,3u,1u);    /* Minimal */
    ctl=mvd_ctl_set(ctl,MVD_CTL_BUDGET,7u,2u); /* 1 logical core */
    if(mode==3)ctl=mvd_ctl_set(ctl,MVD_CTL_SYSTEM,3u,1u);
    return ctl;
}
static inline bool mvd_ctl_valid(uint32_t c,unsigned mode) {
    return mode<=3 && mvd_ctl_get(c,MVD_CTL_MODE,3u)==mode &&
        mvd_ctl_get(c,MVD_CTL_SYSTEM,3u)<=2 &&
        mvd_ctl_get(c,MVD_CTL_BETA,3u)<=3 &&
        mvd_ctl_get(c,MVD_CTL_BUDGET,7u)<=5 &&
        mvd_ctl_get(c,MVD_CTL_TAPE_SYSTEM,3u)<=2 &&
        mvd_ctl_get(c,MVD_CTL_TAPE_FAMILY,7u)<=5 &&
        mvd_ctl_get(c,MVD_CTL_TAPE_SPEED,3u)<=2 &&
        mvd_ctl_get(c,MVD_CTL_OSD,3u)<=2 &&
        mvd_ctl_get(c,MVD_CTL_STATS,3u)<=2;
}
static inline unsigned mvd_ctl_budget(uint32_t c) {
    static const unsigned budgets[]={50,75,100,150,200,300};
    unsigned n=mvd_ctl_get(c,MVD_CTL_BUDGET,7u);
    return budgets[n<=5?n:2];
}
static inline unsigned mvd_ctl_rf_profile(uint32_t c) {
    const unsigned system=mvd_ctl_get(c,MVD_CTL_SYSTEM,3u);
    if(mvd_ctl_get(c,MVD_CTL_TAPE,1u)) {
        switch(mvd_ctl_get(c,MVD_CTL_BETA,3u)) {
            case 1: return 3; /* Betamax Hi-Fi */
            case 2: return 5; /* SuperBeta Hi-Fi (matches Local client) */
            case 3: return 8; /* ED Beta */
            default:return 2; /* Betamax */
        }
    }
    if(system==1)return 1; /* NTSC VHS */
    if(system==2)return mvd_ctl_get(c,MVD_CTL_MESECAM,1u)?7:6;
    return 0; /* PAL VHS */
}
#endif
