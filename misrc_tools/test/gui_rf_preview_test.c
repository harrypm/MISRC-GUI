/* Synthetic RF contract tests, not a claim of tape/hardware qualification.
 * The fixture is FM luma plus VHS color-under, including sync/equalizing
 * cadence, PAL V-switch, alternating recording-head line rotation, and a
 * warped tape time axis. It never feeds decoded RGB/YUV to the decoder. */
#include "../misrc_gui/signal/gui_rf_preview.h"
#include "../misrc_gui/signal/gui_rf_preview_filters.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_PI 3.14159265358979323846
#define TEST_RATE 20000000.0
#define TEST_ED_RATE 20000000.0

typedef struct {
    uint8_t *rgba;
    int width, height, frames;
    bool beta_display; /* Beta preserves its validated full-range active transfer. */
    uint64_t end_sample, first_end_sample;
    gui_rf_decode_status_t status;
} test_frames_t;

static void receive_frame(void *context, const uint8_t *rgba, int width, int height,
                          uint64_t end_sample, const gui_rf_decode_status_t *status) {
    test_frames_t *f=context;
    memcpy(f->rgba,rgba,(size_t)width*height*4u);
    f->width=width; f->height=height;
    if(!f->frames) f->first_end_sample=end_sample;
    f->frames++;
    assert(end_sample>=f->end_sample);
    f->end_sample=end_sample; f->status=*status;
}

static const double bars[8][3] = {
    {1,1,1},{1,1,0},{0,1,1},{0,1,0},{1,0,1},{1,0,0},{0,0,1},{0,0,0}
};

static double clamp_value(double value, double minimum, double maximum) {
    return value<minimum ? minimum : value>maximum ? maximum : value;
}

/* VHS studio-swing display contract shared with the renderer (rf_display_level in
 * gui_rf_preview.h: black lifted above the floor, white with headroom, sync
 * clamped at the floor). Physical 0..1 luminance targets are compared through
 * the same conversion, so the oracle stays exact while the rendered byte
 * values are lifted — one shared definition, never a drifted duplicate. */
static double display_level(double y01) {
    return rf_display_level(y01);
}

/* Expected display value of one bar channel: the physical RGB target
 * through the generator's YUV encode and the renderer's YUV->RGB matrix. */
static double display_bar_channel(int bar,int channel) {
    double r=bars[bar][0],g=bars[bar][1],b=bars[bar][2];
    double y=.299*r+.587*g+.114*b;
    double u=.492*(b-y),v=.877*(r-y),dy=display_level(y);
    double value=channel==0 ? dy+1.140*v
               : channel==1 ? dy-.395*u-.581*v
               : dy+2.032*u;
    return clamp_value(value,0.0,1.0);
}

typedef enum {
    TEST_V_NORMAL, TEST_V_SPLIT_FIRST, TEST_V_NEW_EQ, TEST_V_ISOLATED_FRAGMENT,
    TEST_V_NO_PRE_EQ, TEST_V_ONE_PRE_EQ, TEST_V_BAD_SPACING, TEST_V_PAST_HALF,
    TEST_V_SPLIT_SECOND_H, TEST_V_SPLIT_SHORT_FIRST, TEST_V_SHORT_INTERRUPTED,
    TEST_V_SPLIT_15P5_FIRST, TEST_V_SHORT_BELOW_MIN, TEST_V_SHORT_NO_PRE_EQ,
    TEST_V_SHORT_SECOND_15P5, TEST_V_SPLIT_13P5_FIRST, TEST_V_SHORT_SECOND_13P5,
    TEST_V_H_ONLY
} test_vertical_t;

typedef enum { TEST_Q_GRAY, TEST_Q_COLOR_BARS, TEST_Q_FADE, TEST_Q_MID_BARS,
               TEST_Q_LEVEL_BARS, TEST_Q_UV_VERTICAL, TEST_Q_UV_HORIZONTAL, TEST_Q_UV_DETAIL, TEST_Q_UV_HORIZONTAL_SINE, TEST_Q_UV_STRIPES } test_quality_pattern_t;
typedef enum { STRIPE_BACKGROUND, STRIPE_CHART, STRIPE_CORRUPTED, STRIPE_LEGAL_SHORT } test_stripe_scene_t;
typedef struct {
    test_quality_pattern_t pattern;
    int fields;
    double window_noise_hz, carrier_offset_hz, speed_wander;
    uint64_t field_start_sample[17];
    double beta_audio_amplitude;
    int swap_tracks, switch_track_line;
    int fm_change_field;
    double fm_blank_hz, fm_sync_hz, fm_hz_per_ire;
    double fm_blank_after_hz, fm_sync_after_hz, fm_hz_per_ire_after;
    double burst_gain_jitter, burst_phase_jitter;
    uint32_t burst_noise_seed;
    bool suppress_burst;
    test_stripe_scene_t stripe_scene;
    int chroma_jump_line;
    double chroma_jump_radians;
    bool sparse_bad_bursts;
    int bad_burst_first_line, bad_burst_line_step;
    double bad_burst_gain, bad_burst_phase;
    double detail_amplitude, detail_phase;
    double c_sine_hz, c_sine_amplitude, c_sine_phase;
    double chroma_noise_amplitude, generated_c_noise_rms;
    uint32_t chroma_noise_seed;
} test_quality_t;

/* Active source rows. A two-line color stroke can align with the
 * decoder's output pair or cross its boundary. */
static const struct {
    int row, span;
    double amplitude, x0, x1, min_gain;
} svhs_stripes[] = {
    {64, 2, .085, .12, .84, .375},
    {145, 2, .085, .12, .84, .255},
    {188, 1, .055, .34, .67, .090},
};
static const int svhs_pulse_row = 104;
static const double svhs_pulse_u = .80;
static const int svhs_legal_short_row = 177;
static const double svhs_legal_y = .72;

static double svhs_stripe_polarity(int parity) { return parity ? -1.0 : 1.0; }

/* Video values are a physical FM oracle: blank + 100*Hz/IRE*gray, not a
 * reconstruction of the decoder's porch estimator or render algorithm. */
static double quality_gray(int field,test_quality_pattern_t pattern) {
    return pattern==TEST_Q_FADE ? fmax(0.0,.70-.10*field) : field&1 ? .25 : .65;
}

/* Safe, equal-luminance colors are prescribed in physical UV units. Both
 * original fields have distinct colors and moving boundaries; an interfield
 * blend or a luma-only comb cannot pass by making the image uniformly gray. */
static void quality_uv_target(int parity,bool second,double *u,double *v) {
    *u=parity ? -.10 : .12;
    *v=parity ? .10 : .08;
    if(second) { *u=-*u;*v=-*v; }
}

static double quality_uv_boundary(int parity) { return parity ? .57 : .43; }

/* Independently prescribed equal-Y chroma chart: strong/weak horizontal
 * edges, legitimate vertical color texture and a small E-shaped color glyph.
 * No decoder coefficients or reconstructed output enter this RF oracle. */
static void quality_detail_uv(int parity,double x,int field_line,double amplitude,double phase,double *u,double *v) {
    double q=0.0;
    int column=(int)(x*5.0);if(column>4) column=4;
    if(column==0) q=field_line<80 ? -.08 : .08;
    else if(column==1) q=field_line<80 ? -.0125 : .0125;
    else if(column==2 || column==3) {
        double period=column==2 ? 16.0 : 8.0;
        q=amplitude*sin(2.0*TEST_PI*field_line/period+phase);
    } else {
        bool upright=x>=.825 && x<.845 && field_line>=64 && field_line<100;
        bool cap=x>=.825 && x<.955 && ((field_line>=64 && field_line<68) ||
            (field_line>=80 && field_line<84) || (field_line>=96 && field_line<100));
        if(upright || cap) q=.025;
    }
    *u=(parity ? -.04 : .05)+q;
    *v=(parity ? .05 : .04)+.7*q;
}

/* Fixed-seed, non-periodic burst noise. The known active UV target does not
 * depend on this sequence or on any decoder/candidate coefficients. */
static double quality_burst_noise(uint32_t line,uint32_t seed) {
    uint32_t x=line^seed;
    x^=x>>16;x*=UINT32_C(0x7feb352d);x^=x>>15;
    x*=UINT32_C(0x846ca68b);x^=x>>16;
    return (double)(x&UINT32_C(0xffff))/32767.5-1.0;
}

/* Explicit membership: later SECAM profile IDs are not Beta video. */
static bool test_beta_profile(gui_rf_profile_t profile) {
    return profile==GUI_RF_PROFILE_NTSC_BETAMAX || profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI ||
        profile==GUI_RF_PROFILE_NTSC_SUPERBETA || profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI ||
        profile==GUI_RF_PROFILE_NTSC_ED_BETA;
}

static float *make_rf_signal_profile(gui_rf_profile_t profile, double jitter_us, bool with_chroma,
                            double clock_scale, int damaged_field,
                            test_vertical_t vertical_case, test_quality_t *quality,
                            size_t *length) {
    bool pal=profile==GUI_RF_PROFILE_PAL_VHS || profile==GUI_RF_PROFILE_PAL_SVHS;
    const bool svhs=profile==GUI_RF_PROFILE_PAL_SVHS || profile==GUI_RF_PROFILE_NTSC_SVHS;
    const bool ed=profile==GUI_RF_PROFILE_NTSC_ED_BETA;
    bool superbeta=profile==GUI_RF_PROFILE_NTSC_SUPERBETA || profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI;
    bool beta=test_beta_profile(profile);
    bool hifi=profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI;
    double line_us=pal ? 64.0 : 63.55555555555556;
    /* Independent physical modulation constants; do not query decoder profiles. */
    double carrier=beta ? 688374.125874126 : pal ? 626953.0 : 629370.6293706294;
    double blank=beta ? (hifi ? 4200000.0 : 3942857.1428571427) : pal ? 4100000.0 : 3685714.2857142854;
    double sync=beta ? (hifi ? 3800000.0 : 3600000.0) : pal ? 3800000.0 : 3400000.0;
    double hz_ire=beta ? (hifi ? 10000.0 : 8571.42857142857) : pal ? 7000.0 : 7142.857142857143;
    if(svhs) {
        /* Physical S-VHS endpoints are 5.4MHz sync and 7.0MHz white.
         * PAL's -300mV sync is below its 0..700mV active range; NTSC uses
         * -40..100 IRE. VHS color-under and head rotation are unchanged. */
        sync=5400000.0;
        hz_ire=pal ? 11200.0 : 11428.57142857143;
        blank=pal ? 5880000.0 : 5857142.857142857;
    }
    if(superbeta) {
        /* Independent ordinary-SuperBeta physical endpoints: 4.4MHz at
         * -40 IRE sync and 5.6MHz at 100 IRE white. Hi-Fi does not move them.
         * Do not copy the distinct 4.75/6.15MHz .NET SUPERBETA profile. */
        sync=4400000.0;double white=5600000.0;
        hz_ire=(white-sync)/140.0;blank=sync+40.0*hz_ire;
    }
    if(ed) {
        /* Independent ED physical endpoints; NTSC-J black is 0 IRE.
         * This is a flat preview oracle, not Sony nonlinear LCR emulation. */
        sync=6800000.0;double white=9300000.0;
        hz_ire=(white-sync)/140.0;blank=sync+40.0*hz_ire;
    }
    double burst_start=pal ? 5.6 : 5.3, burst_end=pal ? 7.85 : 7.8;
    double active_start=pal ? 10.5 : 9.45, active_end=pal ? 62.5 : 62.55555555555556;
    int eq_count=pal ? 5 : 6;
    int field_count=quality ? quality->fields : 6;
    assert(field_count>=6 && field_count<=16);
    int starts[17]; starts[0]=0;
    for (int f=0; f<field_count; ++f) starts[f+1]=starts[f]+(pal ? (f&1 ? 313 : 312) : (f&1 ? 262 : 263));
    int half_lines=starts[field_count]*2;
    double *timeline=malloc((size_t)(half_lines+1)*sizeof(double));
    assert(timeline);
    for (int h=0; h<=half_lines; ++h) {
        timeline[h]=h*line_us*.5*clock_scale+jitter_us*sin(h*2.0*TEST_PI/19.0);
        if (quality && quality->speed_wander!=0.0) {
            /* Integrate a slow physical speed variation. Both FM carrier and
             * actual H timing move together; active video values stay fixed. */
            double duration=half_lines*line_us*.5;
            timeline[h]+=quality->speed_wander*duration/(2.0*TEST_PI)
                *(1.0-cos(h*2.0*TEST_PI/half_lines));
        }
    }
    if (quality) for (int f=0; f<=field_count; ++f)
        quality->field_start_sample[f]=(uint64_t)floor(timeline[starts[f]*2]*(ed ? TEST_ED_RATE/1e6 : TEST_RATE/1e6));
    *length=(size_t)floor(timeline[half_lines]*(ed ? TEST_ED_RATE/1e6 : TEST_RATE/1e6));
    float *rf=malloc(*length*sizeof(float)); assert(rf);
    int half=0, field=0;
    double fm_phase=0.0, previous_ideal_us=0.0;
    /* Inverse of the decoder's linear tape de-emphasis shelf. */
    double tau_zero=pal ? 100e-9 : 120e-9, tau_pole=pal ? 400e-9 : 320e-9;
    double k=2.0*TEST_RATE, denom=1.0+k*tau_zero;
    double b0=(1.0+k*tau_pole)/denom, b1=(1.0-k*tau_pole)/denom;
    double a1=(1.0-k*tau_zero)/denom, previous_base=0.0, previous_pre=0.0;
    double shelf[5]={0},previous2_base=0.0,previous2_pre=0.0;
    if (beta && !ed) {
        /* Physical record pre-emphasis at the RF sample rate, independent of
         * the decoder's inverse shelf evaluated at its 5MHz working rate. */
        double amplitude=pow(10.0,8.0/40.0),omega=2.0*TEST_PI*100000.0/TEST_RATE;
        double cs=cos(omega),alpha=sin(omega),root=sqrt(amplitude);
        double den=(amplitude+1.0)-(amplitude-1.0)*cs+2.0*root*alpha;
        shelf[0]=amplitude*((amplitude+1.0)+(amplitude-1.0)*cs+2.0*root*alpha)/den;
        shelf[1]=-2.0*amplitude*((amplitude-1.0)+(amplitude+1.0)*cs)/den;
        shelf[2]=amplitude*((amplitude+1.0)+(amplitude-1.0)*cs-2.0*root*alpha)/den;
        shelf[3]=2.0*((amplitude-1.0)-(amplitude+1.0)*cs)/den;
        shelf[4]=((amplitude+1.0)-(amplitude-1.0)*cs-2.0*root*alpha)/den;
    }
    double c_noise[2][3]={{0}},c_noise_square=0.0;
    uint32_t c_noise_state=quality && quality->chroma_noise_seed ? quality->chroma_noise_seed : UINT32_C(0x5eeda5e5);
    const double c_noise_alpha=1.0-exp(-2.0*TEST_PI*800000.0/TEST_RATE);
    for (size_t n=0; n<*length; ++n) {
        double real_us=n/(ed ? TEST_ED_RATE/1e6 : TEST_RATE/1e6);
        while (half+1<half_lines && timeline[half+1]<=real_us) half++;
        double fraction=(real_us-timeline[half])/(timeline[half+1]-timeline[half]);
        double ideal_us=(half+fraction)*line_us*.5;
        while (field<field_count-1 && ideal_us>=starts[field+1]*line_us) field++;
        double local_us=ideal_us-starts[field]*line_us;
        int line=(int)floor(local_us/line_us);
        double within=local_us-line*line_us;
        int parity=field&1;
        double boundary=pal ? (parity ? 1.0 : .5) : (parity ? .5 : 1.0);
        double eq_begin=boundary*line_us;
        double broad_begin=eq_begin+eq_count*line_us*.5;
        double eq2_begin=broad_begin+eq_count*line_us*.5;
        double vertical_end=eq2_begin+eq_count*line_us*.5;
        bool sync_pulse=false;
        if (local_us<eq_begin) sync_pulse=within<4.7;
        else if (local_us<vertical_end) {
            double section=local_us<broad_begin ? eq_begin : local_us<eq2_begin ? broad_begin : eq2_begin;
            double in_half=fmod(local_us-section,line_us*.5);
            double pulse_width=local_us>=broad_begin && local_us<eq2_begin
                ? (pal ? 27.3 : 27.1) : (pal ? 2.35 : 2.3);
            sync_pulse=in_half<pulse_width;
            if (field==damaged_field && local_us>=broad_begin && local_us<eq2_begin) {
                int broad=(int)((local_us-broad_begin)/(line_us*.5));
                /* Missing first broad pulse and a split second pulse leave
                 * four real broad pulses, but only one spurious preceding EQ.
                 * A valid earlier field must not supply this train's origin. */
                if (broad==0) sync_pulse=false;
                else if (broad==1)
                    sync_pulse=in_half<4.7 || (in_half>=23.0 && in_half<25.3);
            }
            if (jitter_us>0.0 && local_us<broad_begin) {
                double glitch=local_us-(broad_begin-line_us*.5+6.0);
                if (glitch>=0.0 && glitch<1.6) sync_pulse=true;
            }
        } else sync_pulse=within<4.7;
        if (vertical_case!=TEST_V_NORMAL && local_us>=eq_begin && local_us<broad_begin) {
            if (vertical_case==TEST_V_NO_PRE_EQ || vertical_case==TEST_V_SHORT_NO_PRE_EQ) sync_pulse=false;
            if (vertical_case==TEST_V_ONE_PRE_EQ)
                sync_pulse=local_us>=broad_begin-line_us*.5 && sync_pulse;
        }
        if (vertical_case!=TEST_V_NORMAL && local_us>=broad_begin && local_us<eq2_begin) {
            int broad=(int)((local_us-broad_begin)/(line_us*.5));
            double in_half=fmod(local_us-broad_begin,line_us*.5);
            if (broad==0) {
                bool short_first=vertical_case==TEST_V_SPLIT_SHORT_FIRST || vertical_case==TEST_V_SHORT_INTERRUPTED;
                double leading=short_first ? 17.5 : 20.0;
                double tail_start=short_first ? 23.5 : 25.0;
                double tail_end=27.3;
                if (vertical_case>=TEST_V_SPLIT_15P5_FIRST) {
                    bool shorter=vertical_case==TEST_V_SHORT_BELOW_MIN
                        || vertical_case==TEST_V_SHORT_NO_PRE_EQ || vertical_case>=TEST_V_SPLIT_13P5_FIRST;
                    leading=vertical_case==TEST_V_SHORT_BELOW_MIN ? 12.5 : shorter ? 13.5 : 15.5;
                    tail_start=leading+6.0;tail_end=tail_start+(shorter ? 7.8 : 6.0);
                }
                if (vertical_case==TEST_V_PAST_HALF) {
                    tail_start=line_us*.5-1.0; tail_end=line_us*.5+1.3;
                }
                sync_pulse=in_half<leading || (in_half>=tail_start && in_half<tail_end);
            }
            if (vertical_case==TEST_V_ISOLATED_FRAGMENT && broad>0) sync_pulse=false;
            if (vertical_case==TEST_V_BAD_SPACING && broad==1) sync_pulse=false;
            if (vertical_case==TEST_V_SHORT_INTERRUPTED && broad==2) sync_pulse=in_half<4.7;
            if (vertical_case==TEST_V_PAST_HALF && broad==1)
                sync_pulse=in_half>=4.5 && in_half<(pal ? 27.3 : 27.1);
            if (vertical_case==TEST_V_PAST_HALF && local_us>=broad_begin+line_us*.5-1.0
                && local_us<broad_begin+line_us*.5+1.3) sync_pulse=true;
            if (vertical_case==TEST_V_SPLIT_SECOND_H) {
                if (broad==0) sync_pulse=in_half<(pal ? 27.3 : 27.1);
                if (broad==1) sync_pulse=in_half<17.5 || (in_half>=23.5 && in_half<27.3);
            }
            if (vertical_case==TEST_V_SHORT_SECOND_15P5) {
                if (broad==0) sync_pulse=in_half<(pal ? 27.3 : 27.1);
                if (broad==1) sync_pulse=in_half<15.5 || (in_half>=21.5 && in_half<27.5);
            }
            if (vertical_case==TEST_V_SHORT_SECOND_13P5) {
                if (broad==0) sync_pulse=in_half<(pal ? 27.3 : 27.1);
                if (broad==1) sync_pulse=in_half<13.5 || (in_half>=19.5 && in_half<27.3);
            }
        }
        if (vertical_case==TEST_V_NEW_EQ && local_us>=broad_begin
            && local_us<broad_begin+25.0+eq_count*line_us) {
            double new_eq=broad_begin+25.0;
            if (local_us<new_eq) sync_pulse=local_us-broad_begin<20.0;
            else {
                double at=local_us-new_eq;
                double in_half=fmod(at,line_us*.5);
                sync_pulse=in_half<(at<eq_count*line_us*.5 ? (pal ? 2.35 : 2.3)
                                                                        : (pal ? 27.3 : 27.1));
            }
        }
        /* An H-only tape interval carries no equalizing or broad V pulses. */
        if(vertical_case==TEST_V_H_ONLY) sync_pulse=within<4.7;
        double r=0,g=0,b=0,u=0,v=0,luminance=0;
        bool active=local_us>=vertical_end && within>=active_start && within<active_end;
        if (active) {
            int bar=(int)((within-active_start)/(active_end-active_start)*8.0);
            if (bar>7) bar=7;
            r=bars[bar][0]; g=bars[bar][1]; b=bars[bar][2];
            /* Distinct genuine field content. If the monitor bobs/duplicates a
             * single field, adjacent rows in the first bar become equal. */
            if (bar==0) r=g=b=parity ? .22 : .82;
            if (quality && (quality->pattern==TEST_Q_GRAY || quality->pattern==TEST_Q_FADE))
                r=g=b=quality_gray(field,quality->pattern);
            if (quality && quality->pattern==TEST_Q_MID_BARS) {
                /* Stay inside RGB gamut even while Y and C are misaligned.
                 * Otherwise RGB clipping can make recovered Y follow C. */
                r=.3+.4*r;g=.3+.4*g;b=.3+.4*b;
            }
            if (quality && quality->pattern==TEST_Q_LEVEL_BARS) {
                const double levels[]={0.0,.025,.10,.25,.50,.65,.85,1.0};
                r=g=b=levels[bar];
            }
            luminance=.299*r+.587*g+.114*b;
            u=.492*(b-luminance); v=.877*(r-luminance);
            if(quality && (quality->pattern==TEST_Q_UV_VERTICAL ||
                           quality->pattern==TEST_Q_UV_HORIZONTAL)) {
                double color_boundary=quality_uv_boundary(parity);
                bool second=quality->pattern==TEST_Q_UV_VERTICAL
                    ? (within-active_start)/(active_end-active_start)>=color_boundary
                    : line>=(pal ? 22 : 20)+(int)(color_boundary*(pal ? 288 : 240));
                luminance=.5;
                quality_uv_target(parity,second,&u,&v);
            }
            if(quality && quality->pattern==TEST_Q_UV_HORIZONTAL_SINE) {
                luminance=.5;
                double wave=quality->c_sine_amplitude*sin(2.0*TEST_PI*quality->c_sine_hz*
                    (within-active_start)*1e-6+quality->c_sine_phase+(parity ? -.71 : .37));
                u=(parity ? -.04 : .05)+wave;v=(parity ? .05 : .04)+.7*wave;
            }
            if (quality && quality->pattern==TEST_Q_UV_STRIPES) {
                double x = (within-active_start)/(active_end-active_start);
                int row = line-(pal ? 22 : 20);
                if (quality->stripe_scene==STRIPE_LEGAL_SHORT) {
                    /* Legal yellow: R=G=Y/(.299+.587), B=0. Retain both
                     * patches from the original short-stroke RF chart. */
                    luminance = svhs_legal_y;
                    bool yellow = (row==102 && x>=.12 && x<.84) ||
                                  (row==svhs_legal_short_row && x>=.488 && x<.512);
                    u = yellow ? -.492*luminance : 0.0;
                    v = yellow ? .877*(luminance/(.299+.587)-luminance) : 0.0;
                } else {
                    luminance = .5;
                    u = parity ? -.04 : .05;
                    v = parity ? .05 : .04;
                    if (quality->stripe_scene!=STRIPE_BACKGROUND) {
                        for (size_t s=0; s<sizeof(svhs_stripes)/sizeof(svhs_stripes[0]); ++s) {
                            if (row<svhs_stripes[s].row || row>=svhs_stripes[s].row+svhs_stripes[s].span ||
                                x<svhs_stripes[s].x0 || x>=svhs_stripes[s].x1)
                                continue;
                            double step = svhs_stripes[s].amplitude*svhs_stripe_polarity(parity);
                            u += step;
                            v += .7*step;
                        }
                    }
                }
            }
            if(quality && quality->pattern==TEST_Q_UV_DETAIL) {
                luminance=.5;
                quality_detail_uv(parity,(within-active_start)/(active_end-active_start),
                    line-(pal ? 22 : 20),quality->detail_amplitude>0 ? quality->detail_amplitude : .025,
                    quality->detail_phase,&u,&v);
            }
        }
        double physical_blank=blank,physical_sync=sync,physical_hz_ire=hz_ire;
        if(quality && quality->fm_hz_per_ire>0) {
            physical_blank=quality->fm_blank_hz;physical_sync=quality->fm_sync_hz;
            physical_hz_ire=quality->fm_hz_per_ire;
        }
        if(quality && quality->fm_change_field>0 && field>=quality->fm_change_field) {
            physical_blank=quality->fm_blank_after_hz;
            physical_sync=quality->fm_sync_after_hz;
            physical_hz_ire=quality->fm_hz_per_ire_after;
        }
        double frequency=sync_pulse ? physical_sync : physical_blank+100.0*physical_hz_ire*luminance;
        if (quality) {
            /* Only ordinary-line sync-tip/back-porch windows are corrupted.
             * Active gray/bars, burst and the entire V cadence remain intact.
             * All disturbances modulate real RF; no decoded plane is patched. */
            if (local_us>=vertical_end && line>=20 && quality->window_noise_hz>0.0) {
                int source_line=starts[field]+line;
                double porch_error=quality->window_noise_hz*(sin(source_line*2.0*TEST_PI/7.0)
                    +.4*cos(source_line*2.0*TEST_PI/17.0));
                double tip_error=-.25*quality->window_noise_hz*cos(source_line*2.0*TEST_PI/9.0);
                if (source_line%23==7) {
                    porch_error+=1.6*quality->window_noise_hz;
                    tip_error-=.7*quality->window_noise_hz;
                }
                if (!sync_pulse && within>=8.0 && within<9.1) frequency+=porch_error;
                if (sync_pulse && within>=1.7 && within<3.5) frequency+=tip_error;
            }
            /* Carrier-only offset is intentionally distinct from speed drift.
             * Legacy fixed profiles retain it; acquisition-calibrated
             * SuperBeta levels may remove the initial recording offset. */
            frequency+=quality->carrier_offset_hz;
        }
        double base=frequency-physical_blank;
        double pre=ed ? base : beta ? shelf[0]*base+shelf[1]*previous_base+shelf[2]*previous2_base
            -shelf[3]*previous_pre-shelf[4]*previous2_pre
            : b0*base+b1*previous_base-a1*previous_pre;
        previous2_base=previous_base;previous2_pre=previous_pre;
        previous_base=base; previous_pre=pre;
        frequency=ed ? clamp_value(physical_blank+pre,6000000.0,11000000.0)
            : svhs ? clamp_value(physical_blank+pre,3000000.0,9600000.0)
            : clamp_value(physical_blank+pre,2400000.0,6800000.0);
        fm_phase+=2.0*TEST_PI*frequency*(ideal_us-previous_ideal_us)*1e-6;
        previous_ideal_us=ideal_us;
        if (fm_phase>2.0*TEST_PI) fm_phase=fmod(fm_phase,2.0*TEST_PI);
        int vswitch=pal && (line&1) ? -1 : 1;
        bool color_burst=local_us>=vertical_end && within>=burst_start && within<burst_end;
        double burst_phase_noise=0.0;
        if (color_burst) {
            u=pal ? -(150.0/700.0)/sqrt(2.0) : -(150.0/700.0);
            v=pal ? (150.0/700.0)/sqrt(2.0) : 0.0;
            if(quality) {
                uint32_t source_line=(uint32_t)(starts[field]+line);
                double gain=1.0+quality->burst_gain_jitter*
                    quality_burst_noise(source_line,quality->burst_noise_seed);
                u*=gain;v*=gain;
                burst_phase_noise=quality->burst_phase_jitter*
                    quality_burst_noise(source_line,quality->burst_noise_seed^UINT32_C(0xa5a5c3c3));
                int bad_first=quality->bad_burst_first_line>0 ? quality->bad_burst_first_line : 101;
                int bad_step=quality->bad_burst_line_step>0 ? quality->bad_burst_line_step : 50;
                if(quality->sparse_bad_bursts && (line==bad_first || line==bad_first+bad_step ||
                                                  line==bad_first+2*bad_step)) {
                    u*=quality->bad_burst_gain;v*=quality->bad_burst_gain;
                    burst_phase_noise+=quality->bad_burst_phase;
                }
                if(quality->suppress_burst) u=v=0.0;
            }
        }
        int track=parity^(quality ? quality->swap_tracks : 0);
        if (quality && quality->switch_track_line>0 && line>=quality->switch_track_line) track^=1;
        double track_step=beta ? (track ? 0.0 : TEST_PI)
            : pal ? (track ? -TEST_PI*.5 : 0.0)
            : (track ? TEST_PI*.5 : -TEST_PI*.5);
        double chroma_phase=2.0*TEST_PI*carrier*ideal_us*1e-6+track_step*line+.37;
        if(color_burst) chroma_phase+=burst_phase_noise;
        if(quality && quality->chroma_jump_line>0 && line>=quality->chroma_jump_line)
            chroma_phase+=quality->chroma_jump_radians;
        /* NTSC color-under records U*cos+V*sin: the sum-frequency VHS
         * playback heterodyne conjugates this quadrature. PAL uses a swinging
         * burst with its V switch; do not copy the decoder IQ sign as an oracle. */
        double color=u*cos(chroma_phase)+(pal ? -vswitch : 1)*v*sin(chroma_phase);
        double audio=0.0;
        if (quality && quality->beta_audio_amplitude>0.0) {
            /* Two simultaneous FM carriers per recorded head. RF is shared by
             * video and Beta Hi-Fi; these are not decoded PCM injections. */
            static const double ratios[4]={87.75,97.25,106.75,116.25};
            for (int branch=track;branch<4;branch+=2) {
                double carrier_hz=ratios[branch]*(15750000.0/1001.0);
                double tone_hz=branch<2 ? 700.0 : 1900.0;
                double angle=2.0*TEST_PI*carrier_hz*ideal_us*1e-6
                    +(25000.0/tone_hz)*sin(2.0*TEST_PI*tone_hz*ideal_us*1e-6);
                audio+=quality->beta_audio_amplitude*cos(angle);
            }
        }
        double rf_c_noise=0.0;
        if(quality && quality->chroma_noise_amplitude>0.0) {
            /* Continuous fixed-seed baseband IQ noise is low-passed three
             * times BEFORE RF modulation. It covers sync, burst and active
             * intervals, with no zero-order-held wideband images or direct
             * additions to the physical FM/Y oscillator. */
            for(int channel=0;channel<2;channel++) {
                c_noise_state^=c_noise_state<<13;c_noise_state^=c_noise_state>>17;c_noise_state^=c_noise_state<<5;
                double value=(double)(c_noise_state&UINT32_C(0xffff))/32767.5-1.0;
                for(int pole=0;pole<3;pole++) {
                    c_noise[channel][pole]+=c_noise_alpha*(value-c_noise[channel][pole]);
                    value=c_noise[channel][pole];
                }
            }
            double angle=2.0*TEST_PI*carrier*real_us*1e-6+.123;
            rf_c_noise=.20*quality->chroma_noise_amplitude*
                (c_noise[0][2]*cos(angle)+c_noise[1][2]*sin(angle));
            c_noise_square+=rf_c_noise*rf_c_noise;
        }
        /* Add a severe, short C-band RF pulse to only one active line.
         * Alternate the affected pair member between the original fields.
         * The physical FM, burst, and continuous C noise stay identical. */
        if (quality && quality->pattern==TEST_Q_UV_STRIPES &&
            quality->stripe_scene==STRIPE_CORRUPTED && active &&
            line-(pal ? 22 : 20)==svhs_pulse_row+parity) {
            double x = (within-active_start)/(active_end-active_start);
            if (x>=.490 && x<.510)
                rf_c_noise += .20*svhs_pulse_u*svhs_stripe_polarity(parity)*
                    (cos(chroma_phase)+(pal ? -vswitch : 1)*.7*sin(chroma_phase));
        }
        rf[n]=(float)(.72*cos(fm_phase)+(with_chroma ? .20*color : 0.0)+audio+rf_c_noise);
    }
    if(quality) quality->generated_c_noise_rms=sqrt(c_noise_square/(*length));
    free(timeline);
    return rf;
}

static float *make_rf_signal(gui_rf_format_t format, double jitter_us, bool with_chroma,
                            double clock_scale, int damaged_field,
                            test_vertical_t vertical_case, test_quality_t *quality,
                            size_t *length) {
    return make_rf_signal_profile(format==GUI_RF_PAL ? GUI_RF_PROFILE_PAL_VHS : GUI_RF_PROFILE_NTSC_VHS,
                                  jitter_us,with_chroma,clock_scale,damaged_field,vertical_case,quality,length);
}

static float *make_rf_vertical(gui_rf_format_t format, double jitter_us, bool with_chroma,
                              double clock_scale, int damaged_field,
                              test_vertical_t vertical_case, size_t *length) {
    return make_rf_signal(format,jitter_us,with_chroma,clock_scale,damaged_field,
                          vertical_case,NULL,length);
}

static float *make_rf(gui_rf_format_t format, double jitter_us, bool with_chroma,
                      double clock_scale, int damaged_field, size_t *length) {
    return make_rf_vertical(format,jitter_us,with_chroma,clock_scale,damaged_field,TEST_V_NORMAL,length);
}

static void process_fixture(gui_rf_decoder_t *decoder, const float *rf, size_t length,
                            uint64_t offset, test_frames_t *frames) {
    size_t position=0;
    while (position<length) {
        size_t chunk=position&1 ? 32771u : 4093u;
        if (chunk>length-position) chunk=length-position;
        gui_rf_decoder_process(decoder,rf+position,chunk,offset+position,receive_frame,frames);
        position+=chunk;
    }
}

/* Bar centers inside an explicit horizontal span: Active mode maps bars
 * onto the full width, Full mode only onto the active-window columns. */
static double plane_mean_window(const test_frames_t *f, int bar, int parity,
                                int channel, int x0, int x1) {
    double total=0; int count=0;
    int center=x0+(int)((bar+.5)*(x1-x0)/8.0);
    for (int y=120+parity; y<160; y+=2)
        for (int x=center-4; x<=center+4; ++x) {
            total+=f->rgba[((size_t)y*f->width+x)*4u+channel]; count++;
        }
    return total/count/255.0;
}

static double plane_mean(const test_frames_t *f, int bar, int parity, int channel) {
    return plane_mean_window(f,bar,parity,channel,0,f->width);
}

static void check_bars(const test_frames_t *f, double maximum_error) {
    assert(f->frames>=2);
    assert(f->status.h_locked && f->status.v_locked && f->status.color_locked);
    assert(f->status.fields_accepted==(uint64_t)f->status.fields_decoded);
    assert(f->status.vsync_candidates>=f->status.fields_accepted);
    assert(f->status.fields_rejected==f->status.reject_cadence+
        f->status.reject_parity+f->status.reject_line_map);
    double largest_error=0.0;
    for (int bar=1; bar<8; ++bar) for (int parity=0; parity<2; ++parity)
        for (int c=0; c<3; ++c) {
            double actual=plane_mean(f,bar,parity,c);
            double expected=f->beta_display ? bars[bar][c] : display_bar_channel(bar,c);
            double error=fabs(actual-expected);
            if (error>largest_error) largest_error=error;
            if (error>maximum_error) {
                fprintf(stderr,"bar %d field %d channel %d: %.3f expected %.3f\n",
                        bar,parity,c,actual,expected);
                assert(error<=maximum_error);
            }
        }
    double first=plane_mean(f,0,0,1), second=plane_mean(f,0,1,1);
    fprintf(stdout,"%dx%d: %d frames, bars max error %.3f, woven fields %.3f / %.3f\n",
            f->width,f->height,f->frames,largest_error,first,second);
    assert(first>.65 && second<.38 && first-second>.40);
}

static void check_bar_registration(const test_frames_t *f) {
    /* The independent RF modulator changes Y and color-under together at the
     * red/blue boundary. Their nonzero Y and UV steps make this a physical
     * registration oracle, unlike equal-luminance boundaries or gray alone.
     * Average only rows of one original field; never mix its opposite field. */
    int center=f->width*6/8,radius=f->width/32;
    for (int parity=0; parity<2; ++parity) {
        double previous_y=0.0,previous_u=0.0,previous_v=0.0;
        double y_gradient=0.0,c_gradient=0.0;int y_edge=0,c_edge=0;
        for (int x=center-radius; x<=center+radius; ++x) {
            double rgb[3]={0};int count=0;
            for (int row=120+parity; row<160; row+=2) {
                for (int channel=0; channel<3; ++channel)
                    rgb[channel]+=f->rgba[((size_t)row*f->width+x)*4u+channel]/255.0;
                count++;
            }
            for (int channel=0; channel<3; ++channel) rgb[channel]/=count;
            double y=.299*rgb[0]+.587*rgb[1]+.114*rgb[2];
            double u=.492*(rgb[2]-y),v=.877*(rgb[0]-y);
            if (x>center-radius) {
                double dy=fabs(y-previous_y),dc=hypot(u-previous_u,v-previous_v);
                if (dy>y_gradient) { y_gradient=dy;y_edge=x; }
                if (dc>c_gradient) { c_gradient=dc;c_edge=x; }
            }
            previous_y=y;previous_u=u;previous_v=v;
        }
        printf("%dx%d field %d red/blue registration: Y %d C %d, separation %d pixels (steps %.5f/%.5f)\n",
               f->width,f->height,parity,y_edge,c_edge,abs(y_edge-c_edge),y_gradient,c_gradient);
        fflush(stdout);
        /* PAL's existing first-order/chroma path has a <=7px footprint at
         * 768px width. NTSC's 640px path must remain within <=5px. This tests
         * functional registration, not universal tape pre-emphasis phase. */
        int maximum_separation=f->height==576 ? 7 : 5;
        assert(y_gradient>.005 && c_gradient>.01 && abs(y_edge-c_edge)<=maximum_separation);
    }
}

static void run_registration_contract(void) {
    for (int format=GUI_RF_PAL; format<=GUI_RF_NTSC; ++format) {
        gui_rf_decoder_t *decoder=gui_rf_decoder_create((gui_rf_format_t)format);assert(decoder);
        test_frames_t frames={0};
        frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(frames.rgba);
        test_quality_t signal={.pattern=TEST_Q_MID_BARS,.fields=6};
        size_t length;float *rf=make_rf_signal((gui_rf_format_t)format,0.0,true,1.0,-1,
                                               TEST_V_NORMAL,&signal,&length);
        process_fixture(decoder,rf,length,0,&frames);
        assert(frames.frames>=2 && frames.status.h_locked && frames.status.v_locked && frames.status.color_locked);
        check_bar_registration(&frames);
        free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
    }
}

/* Evaluate the returned transfer, without repeating its coefficient design.
 * These targets are the published physical shelf plateaus and midpoint. */
static double transfer_magnitude(const float c[5],double omega) {
    double nr=c[0]+c[1]*cos(omega)+c[2]*cos(2.0*omega);
    double ni=-c[1]*sin(omega)-c[2]*sin(2.0*omega);
    double dr=1.0+c[3]*cos(omega)+c[4]*cos(2.0*omega);
    double di=-c[3]*sin(omega)-c[4]*sin(2.0*omega);
    return sqrt((nr*nr+ni*ni)/(dr*dr+di*di));
}

static void run_deemphasis_contract(void) {
    float c[5];
    assert(gui_rf_ntsc_deemph_coefficients(5000000.0,c,c+1,c+2,c+3,c+4));
    double dc=transfer_magnitude(c,0.0);
    double mid=transfer_magnitude(c,2.0*TEST_PI*273755.82/5000000.0);
    double high=transfer_magnitude(c,TEST_PI);
    printf("NTSC shelf: DC %.9f, midpoint %.9f, Nyquist %.9f\n",dc,mid,high);
    assert(fabs(dc-1.0)<3e-6);
    assert(fabs(mid-pow(10.0,-13.9794/40.0))<3e-6);
    assert(fabs(high-pow(10.0,-13.9794/20.0))<3e-6);
    const double invalid[]={0.0,-1.0,547511.64,80000001.0,INFINITY,NAN};
    const float sentinel[5]={17.0f,-23.0f,31.0f,-43.0f,59.0f};
    for (size_t n=0; n<sizeof(invalid)/sizeof(invalid[0]); ++n) {
        memcpy(c,sentinel,sizeof(c));
        assert(!gui_rf_ntsc_deemph_coefficients(invalid[n],c,c+1,c+2,c+3,c+4));
        assert(memcmp(c,sentinel,sizeof(c))==0);
    }
    for (int missing=0; missing<5; ++missing) {
        float *out[5]={c,c+1,c+2,c+3,c+4};out[missing]=NULL;
        memcpy(c,sentinel,sizeof(c));
        assert(!gui_rf_ntsc_deemph_coefficients(5000000.0,out[0],out[1],out[2],out[3],out[4]));
        assert(memcmp(c,sentinel,sizeof(c))==0);
    }
}

typedef struct { double mean,row_rms,maximum_error; } quality_rows_t;

static quality_rows_t quality_rows(const test_frames_t *f,int parity,int bar,
                                   int channel,double expected) {
    double sum=0.0,sum_square=0.0,maximum_error=0.0;int count=0;
    int x0=f->width*2/5,x1=f->width*3/5;
    if (bar>=0) { int center=(int)((bar+.5)*f->width/8.0);x0=center-4;x1=center+5; }
    for (int y=40+parity; y<f->height-40; y+=2) {
        double row=0.0;
        for (int x=x0; x<x1; ++x) row+=f->rgba[((size_t)y*f->width+x)*4u+channel];
        row/=(x1-x0)*255.0;
        sum+=row;sum_square+=row*row;count++;
        maximum_error=fmax(maximum_error,fabs(row-expected));
    }
    assert(count>100);
    double mean=sum/count;
    quality_rows_t rows={mean,sqrt(fmax(0.0,sum_square/count-mean*mean)),maximum_error};
    return rows;
}

static void check_quality_gray(const test_frames_t *f,const char *name,
                               double offset,double mean_limit,double row_limit,
                               double outlier_limit) {
    assert(f->frames>=2 && f->status.h_locked && f->status.v_locked);
    printf("Quality cadence: %d frames, %llu accepted, %llu rejected (cad %llu/par %llu/map %llu)\n",
           f->frames,(unsigned long long)f->status.fields_accepted,
           (unsigned long long)f->status.fields_rejected,(unsigned long long)f->status.reject_cadence,
           (unsigned long long)f->status.reject_parity,(unsigned long long)f->status.reject_line_map);
    fflush(stdout);
    for (int parity=0; parity<2; ++parity) {
        double physical=quality_gray(parity,TEST_Q_GRAY)+offset;
        double expected=f->beta_display ? clamp_value(physical,0.0,1.0) : display_level(physical);
        quality_rows_t rows=quality_rows(f,parity,-1,1,expected);
        printf("%s field %d: gray %.6f expected %.6f, row RMS %.6f, max error %.6f\n",
               name,parity,rows.mean,expected,rows.row_rms,rows.maximum_error);
        fflush(stdout);
        assert(fabs(rows.mean-expected)<=mean_limit);
        assert(rows.row_rms<=row_limit && rows.maximum_error<=outlier_limit);
    }
    /* Adjacent rows still contain two different original fields. Studio
     * swing compresses the physical .40 gray gap; Beta retains the full-range
     * separation. Both must preserve the original alternating fields. */
    double first=quality_rows(f,0,-1,1,f->beta_display ? .65+offset : display_level(.65+offset)).mean;
    double second=quality_rows(f,1,-1,1,f->beta_display ? .25+offset : display_level(.25+offset)).mean;
    assert(first-second>.33);
}

typedef struct {
    test_frames_t frame;
    const test_quality_t *signal;
    int checked,black_planes;
    double maximum_mean_error,maximum_row_rms,maximum_outlier,maximum_black;
} test_fade_frames_t;

static void receive_fade_frame(void *context,const uint8_t *rgba,int width,int height,
                               uint64_t end_sample,const gui_rf_decode_status_t *status) {
    test_fade_frames_t *f=context;
    receive_frame(&f->frame,rgba,width,height,end_sample,status);
    if (f->frame.frames<2) return;
    int next=0;
    while (next<f->signal->fields && f->signal->field_start_sample[next+1]<=end_sample) next++;
    assert(next>=3 && next<f->signal->fields);
    for (int parity=0; parity<2; ++parity) {
        int source_field=next-1;
        if ((source_field&1)!=parity) source_field--;
        double physical=quality_gray(source_field,TEST_Q_FADE);
        double expected=display_level(physical);
        quality_rows_t rows=quality_rows(&f->frame,parity,-1,1,expected);
        f->maximum_mean_error=fmax(f->maximum_mean_error,fabs(rows.mean-expected));
        f->maximum_row_rms=fmax(f->maximum_row_rms,rows.row_rms);
        f->maximum_outlier=fmax(f->maximum_outlier,rows.maximum_error);
        if (physical<.001) {
            f->maximum_black=fmax(f->maximum_black,rows.mean);f->black_planes++;
        }
        f->checked++;
    }
}

static void run_quality_format(gui_rf_format_t format) {
    const char *system=format==GUI_RF_PAL ? "PAL" : "NTSC";
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(format);assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(frames.rgba);
    test_quality_t signal={.pattern=TEST_Q_GRAY,.fields=8,.window_noise_hz=60000.0};
    size_t length;float *rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    process_fixture(decoder,rf,length,0,&frames);
    printf("%s noisy porch/sync RF\n",system);
    check_quality_gray(&frames,"noisy windows",0.0,.03,.012,.045);
    free(rf);

    signal.pattern=TEST_Q_COLOR_BARS;signal.window_noise_hz=0.0;
    rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    gui_rf_decoder_reset(decoder,format);frames.end_sample=0;frames.frames=0;
    process_fixture(decoder,rf,length,0,&frames);check_bars(&frames,.08);
    size_t pixels=(size_t)frames.width*frames.height*4u;
    uint8_t *clean_bars=malloc(pixels);assert(clean_bars);memcpy(clean_bars,frames.rgba,pixels);
    free(rf);signal.window_noise_hz=60000.0;
    rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    gui_rf_decoder_reset(decoder,format);frames.end_sample=0;frames.frames=0;
    process_fixture(decoder,rf,length,0,&frames);check_bars(&frames,.08);
    double largest_row_rms=0.0,largest_window_rms=0.0,largest_window_error=0.0;
    for (int bar=1; bar<8; ++bar) for (int parity=0; parity<2; ++parity)
        for (int c=0; c<3; ++c) {
            quality_rows_t rows=quality_rows(&frames,parity,bar,c,display_bar_channel(bar,c));
            largest_row_rms=fmax(largest_row_rms,rows.row_rms);
            int center=(int)((bar+.5)*frames.width/8.0),count=0;double sum_square=0.0;
            for (int y=40+parity; y<frames.height-40; y+=2) {
                double delta=0.0;
                for (int x=center-4; x<=center+4; ++x) {
                    size_t n=((size_t)y*frames.width+x)*4u+c;
                    delta+=(int)frames.rgba[n]-(int)clean_bars[n];
                }
                delta/=9.0*255.0;sum_square+=delta*delta;count++;
                largest_window_error=fmax(largest_window_error,fabs(delta));
            }
            largest_window_rms=fmax(largest_window_rms,sqrt(sum_square/count));
        }
    /* Color-under already has line/track texture. The independent paired
     * control changes only porch/tip RF, so its effect on active colors is
     * bounded separately from the clean fixture's chroma reconstruction.
     * Studio swing keeps 100 IRE at 235/255, so near-white channels no
     * longer clip at display 1.0 and their previously hidden chroma
     * texture now counts in this delta; the bound is slightly larger than
     * the old crushed-level calibration for that reason. */
    printf("%s noisy color bars: row RMS %.6f, window-only RMS %.6f, max delta %.6f\n",
           system,largest_row_rms,largest_window_rms,largest_window_error);fflush(stdout);
    assert(largest_window_rms<=.015 && largest_window_error<=.045);
    free(clean_bars);free(rf);

    signal.pattern=TEST_Q_GRAY;signal.window_noise_hz=0.0;signal.carrier_offset_hz=35000.0;
    rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    gui_rf_decoder_reset(decoder,format);frames.end_sample=0;frames.frames=0;
    process_fixture(decoder,rf,length,0,&frames);
    double hz_ire=format==GUI_RF_PAL ? 7000.0 : 7142.857142857143;
    check_quality_gray(&frames,"carrier-only +35kHz",35000.0/(100.0*hz_ire),.03,.012,.045);
    free(rf);

    /* Keep the fixture inside the existing nominal +/-2-line V admission:
     * PAL's 313-line field needs a smaller margin than NTSC's 263-line field.
     * Larger speed excursions are not a supported-preview claim here. */
    signal.carrier_offset_hz=0.0;signal.speed_wander=format==GUI_RF_PAL ? .004 : .006;
    rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    gui_rf_decoder_reset(decoder,format);frames.end_sample=0;frames.frames=0;
    process_fixture(decoder,rf,length,0,&frames);
    check_quality_gray(&frames,format==GUI_RF_PAL ? "physical slow speed +/-0.4%"
                                                : "physical slow speed +/-0.6%",0.0,.06,.020,.075);
    assert(frames.status.reject_cadence==0 && frames.status.reject_line_map==0);
    free(rf);

    signal.pattern=TEST_Q_FADE;signal.fields=10;signal.speed_wander=0.0;signal.window_noise_hz=60000.0;
    rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    gui_rf_decoder_reset(decoder,format);
    test_fade_frames_t fade={.frame={.rgba=frames.rgba},.signal=&signal};
    size_t position=0;
    while (position<length) {
        size_t chunk=4093u;if (chunk>length-position) chunk=length-position;
        gui_rf_decoder_process(decoder,rf+position,chunk,position,receive_fade_frame,&fade);
        position+=chunk;
    }
    printf("%s physical fade: %d field planes, max gray error %.6f, row RMS %.6f, outlier %.6f, black %.6f\n",
           system,fade.checked,fade.maximum_mean_error,fade.maximum_row_rms,
           fade.maximum_outlier,fade.maximum_black);fflush(stdout);
    assert(fade.checked>=6 && fade.black_planes>=2);
    assert(fade.maximum_mean_error<=.03 && fade.maximum_row_rms<=.012);
    assert(fade.maximum_outlier<=.045);
    /* Black planes sit at the lifted studio-swing floor, not crushed to 0. */
    assert(fabs(fade.maximum_black-display_level(0.0))<=.03);
    free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

static void run_quality_contracts(void) {
    run_quality_format(GUI_RF_PAL);run_quality_format(GUI_RF_NTSC);
}

/* Reuse actual FM ordinary lines, without calling private pulse/parser APIs.
 * A long carrier-only interval removes H while preserving source continuity.
 * One isolated H after that loss must not inherit the previous lock's trust. */
static void check_h_reacquisition(gui_rf_decoder_t *decoder, gui_rf_format_t format,
                                  const float *rf, size_t length, test_frames_t *frames) {
    double line_us=format==GUI_RF_PAL ? 64.0 : 63.55555555555556;
    double blank_hz=format==GUI_RF_PAL ? 4100000.0 : 3685714.2857142854;
    size_t blank_count=(size_t)ceil(60.0*line_us*20.0);
    float *blank_rf=malloc(blank_count*sizeof(*blank_rf));assert(blank_rf);
    for (size_t n=0; n<blank_count; ++n)
        blank_rf[n]=(float)(.72*cos(2.0*TEST_PI*blank_hz*n/TEST_RATE));
    uint64_t offset=length;
    process_fixture(decoder,blank_rf,blank_count,offset,frames);offset+=blank_count;
    gui_rf_decode_status_t lost, isolated, recovered;
    gui_rf_decoder_get_status(decoder,&lost);
    assert(!lost.h_locked && !lost.v_locked && !lost.color_locked);

    size_t ordinary=(size_t)floor(50.0*line_us*20.0);
    size_t one_line=(size_t)ceil(line_us*20.0);
    size_t settling=(size_t)ceil(5.0*line_us*20.0);
    assert(ordinary+one_line<length && settling<blank_count);
    process_fixture(decoder,rf+ordinary,one_line,offset,frames);offset+=one_line;
    process_fixture(decoder,blank_rf,settling,offset,frames);offset+=settling;
    gui_rf_decoder_get_status(decoder,&isolated);
    assert(isolated.hsync_count==lost.hsync_count+1);
    assert(!isolated.h_locked && !isolated.v_locked);
    assert(isolated.frames_decoded==lost.frames_decoded);

    size_t train=(size_t)floor(30.0*line_us*20.0);
    assert(ordinary+train<length);
    process_fixture(decoder,rf+ordinary,train,offset,frames);
    gui_rf_decoder_get_status(decoder,&recovered);
    assert(recovered.hsync_count>=isolated.hsync_count+20);
    assert(recovered.h_locked && !recovered.v_locked);
    assert(recovered.frames_decoded==lost.frames_decoded);
    printf("%s H loss discarded trust; isolated H stayed unlocked; regular H reacquired\n",
           format==GUI_RF_PAL ? "PAL" : "NTSC");
    free(blank_rf);
}

/* A lock can learn a carrier offset during unstable playback. Its old sync
 * threshold must not prevent a later healthy recording from ever supplying H.
 * Exercise physical RF only, with continuous sample positions and no reset. */
static void run_stop_resume_format(gui_rf_format_t format, int direction) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(format);assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(frames.rgba);
    size_t length;float *rf=make_rf(format,0.0,true,1.0,-1,&length);
    uint64_t offset=0;
    process_fixture(decoder,rf,length,offset,&frames);offset+=length;
    check_bars(&frames,.08);
    for (int step=1; step<=10; ++step) {
        test_quality_t quality={.pattern=TEST_Q_COLOR_BARS,.fields=6,
            .carrier_offset_hz=direction*step*25000.0};
        size_t shifted_length;
        float *shifted=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,
                                      &quality,&shifted_length);
        process_fixture(decoder,shifted,shifted_length,offset,&frames);
        offset+=shifted_length;free(shifted);
    }
    gui_rf_decode_status_t before;
    gui_rf_decoder_get_status(decoder,&before);
    assert(before.h_locked && before.v_locked);
    float silence[8192]={0};
    for (int cycle=0; cycle<2; ++cycle) {
        /* The second stop follows a successful automatic reacquisition. */
        size_t gap=(size_t)(TEST_RATE*(cycle ? .5 : .1));
        while (gap) {
            size_t chunk=gap<8192u ? gap : 8192u;
            gui_rf_decoder_process(decoder,silence,chunk,offset,receive_frame,&frames);
            offset+=chunk;gap-=chunk;
        }
        gui_rf_decode_status_t lost,recovered;
        gui_rf_decoder_get_status(decoder,&lost);
        assert(!lost.h_locked && !lost.v_locked && !lost.color_locked);
        assert(lost.frames_decoded>=before.frames_decoded);
        assert(lost.fields_accepted>=before.fields_accepted);
        assert(lost.vsync_candidates>=before.vsync_candidates);
        assert(lost.color_losses==before.color_losses+1u);
        int old_frames=frames.frames;
        uint64_t resume_start=offset;
        process_fixture(decoder,rf,length,offset,&frames);offset+=length;
        gui_rf_decoder_get_status(decoder,&recovered);
        printf("%s carrier offset %+d Hz, stop/resume %d: new frames %d, H/V/color %d/%d/%d\n",
               format==GUI_RF_PAL ? "PAL" : "NTSC",direction*250000,cycle+1,
               frames.frames-old_frames,recovered.h_locked,recovered.v_locked,
               recovered.color_locked);fflush(stdout);
        assert(frames.frames>=old_frames+2);
        assert(recovered.h_locked && recovered.v_locked && recovered.color_locked);
        assert(recovered.hsync_count>lost.hsync_count && recovered.vsync_count>lost.vsync_count);
        assert(recovered.fields_accepted>lost.fields_accepted);
        assert(recovered.color_losses==lost.color_losses);
        assert(frames.end_sample>=resume_start && frames.end_sample<=offset);
        check_bars(&frames,.08);
        before=recovered;
    }
    free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

static void run_stop_resume(void) {
    run_stop_resume_format(GUI_RF_PAL,-1);run_stop_resume_format(GUI_RF_PAL,1);
    run_stop_resume_format(GUI_RF_NTSC,-1);run_stop_resume_format(GUI_RF_NTSC,1);
}

static void run_format(gui_rf_format_t format) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(format); assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u); assert(frames.rgba);
    size_t length;
    float *rf=make_rf(format,0.0,true,1.0,-1,&length);
    process_fixture(decoder,rf,length,0,&frames);
    check_bars(&frames,.08);
    /* The same RF at the MISRC 12-bit ADC amplitude must have the same AGC,
     * color gating, and rendered colors as left-aligned 16-bit DdD RF. */
    size_t pixels=(size_t)frames.width*frames.height*4u;
    uint8_t *reference=malloc(pixels); assert(reference);
    memcpy(reference,frames.rgba,pixels);
    check_h_reacquisition(decoder,format,rf,length,&frames);
    for (size_t n=0; n<length; ++n) rf[n]*=1.0f/16.0f;
    /* Process the other tape system between reference and replay. Returning
     * through reset must discard both its filter state and its field map. */
    gui_rf_format_t other=format==GUI_RF_PAL ? GUI_RF_NTSC : GUI_RF_PAL;
    size_t other_length;float *other_rf=make_rf(other,0.0,true,1.0,-1,&other_length);
    gui_rf_decoder_reset(decoder,other);frames.end_sample=0;
    process_fixture(decoder,other_rf,other_length,0,&frames);check_bars(&frames,.08);
    free(other_rf);
    gui_rf_decoder_reset(decoder,format); frames.end_sample=0;
    process_fixture(decoder,rf,length,0,&frames);
    check_bars(&frames,.08);
    int maximum_difference=0; double total_difference=0.0;
    for (size_t n=0; n<pixels; ++n) {
        int difference=abs((int)frames.rgba[n]-(int)reference[n]);
        if (difference>maximum_difference) maximum_difference=difference;
        total_difference+=difference;
    }
    printf("ADC amplitude invariance: max %d, mean %.5f / 255\n",
           maximum_difference,total_difference/pixels);
    assert(maximum_difference<=2 && total_difference/pixels<.1);
    free(reference);
    free(rf);
    /* A discontinuity must discard both partial fields and all DSP history. */
    float silence[4096]={0};
    gui_rf_decoder_process(decoder,silence,4096,UINT64_C(100000003),receive_frame,&frames);
    gui_rf_decode_status_t status;
    gui_rf_decoder_get_status(decoder,&status);
    assert(!status.h_locked && !status.v_locked && !status.color_locked);
    assert(status.frames_decoded==0);
    assert(status.vsync_candidates==0 && status.fields_accepted==0 && status.fields_rejected==0);
    assert(status.color_losses==0 && status.last_reject_reason[0]=='\0');
    int before=frames.frames;
    rf=make_rf(format,.18,true,1.002,-1,&length);
    process_fixture(decoder,rf,length,UINT64_C(100004099),&frames);
    assert(frames.frames>=before+2);
    check_bars(&frames,.10);
    assert(frames.end_sample>UINT64_C(100004099));
    uint64_t continuous_offset=UINT64_C(100004099)+length;
    free(rf);
    rf=make_rf(format,0.0,false,1.0,-1,&length);
    int color_frames=frames.frames;
    process_fixture(decoder,rf,length,continuous_offset,&frames);
    assert(frames.frames>=color_frames+2);
    assert(frames.status.h_locked && frames.status.v_locked && !frames.status.color_locked);
    assert(frames.status.color_losses>0 && frames.status.last_color_loss_reason[0]!='\0');
    assert(frames.status.fields_accepted==(uint64_t)frames.status.fields_decoded);
    assert(frames.status.fields_rejected==frames.status.reject_cadence+
        frames.status.reject_parity+frames.status.reject_line_map);
    for (int bar=0; bar<8; ++bar) {
        double r=plane_mean(&frames,bar,0,0), g=plane_mean(&frames,bar,0,1), b=plane_mean(&frames,bar,0,2);
        assert(fabs(r-g)<.01 && fabs(r-b)<.01);
    }
    free(rf); free(frames.rgba); gui_rf_decoder_destroy(decoder);
}

static void run_damaged_vertical(void) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(GUI_RF_NTSC); assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(frames.rgba);
    size_t length;
    float *rf=make_rf(GUI_RF_NTSC,0.0,true,1.0,2,&length);
    /* Stop after field 2's malformed vertical interval, before field 3 can
     * reacquire. The broad train remains observable as a source candidate. */
    size_t cutoff=(size_t)((525.0+12.0)*63.55555555555556*20.0);
    assert(cutoff<length);
    process_fixture(decoder,rf,cutoff,0,&frames);
    gui_rf_decode_status_t status;
    gui_rf_decoder_get_status(decoder,&status);
    printf("Damaged NTSC vertical: %llu validated boundaries, %llu source candidates\n",
           (unsigned long long)status.vsync_count,(unsigned long long)status.vsync_candidates);
    /* Acquisition starts at field 1: field 0 lacks a settled preceding H. */
    assert(status.vsync_count==1 && status.vsync_candidates==3);
    assert(status.reject_cadence==1 && status.reject_parity==1
           && status.fields_rejected==2);
    assert(status.last_reject_reason[0]!='\0');
    free(rf); free(frames.rgba); gui_rf_decoder_destroy(decoder);
}

static void run_split_vertical(gui_rf_format_t format, test_vertical_t vertical_case,
                               bool should_decode) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(format); assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(frames.rgba);
    size_t length;
    float *rf=make_rf_vertical(format,0.0,true,1.0,-1,vertical_case,&length);
    process_fixture(decoder,rf,length,UINT64_C(6600000000),&frames);
    gui_rf_decode_status_t status;gui_rf_decoder_get_status(decoder,&status);
    printf("%s vertical case %d: %d frames, %llu fields, %llu candidates, %llu rejected (cad %llu/par %llu/map %llu), H %llu\n",
           format==GUI_RF_PAL ? "PAL" : "NTSC",(int)vertical_case,frames.frames,
           (unsigned long long)status.fields_accepted,(unsigned long long)status.vsync_candidates,
           (unsigned long long)status.fields_rejected,(unsigned long long)status.reject_cadence,
           (unsigned long long)status.reject_parity,(unsigned long long)status.reject_line_map,
           (unsigned long long)status.hsync_count);
    fflush(stdout);
    if (should_decode) check_bars(&frames,.10);
    else assert(frames.frames==0 && status.fields_accepted==0 && status.vsync_count==0);
    /* The immutable pre-fix core counts these same physical H pulses. The
     * deferred H tail and intervening H must both be replayed on failure. */
    if (vertical_case==TEST_V_SHORT_INTERRUPTED)
        assert(status.hsync_count==(format==GUI_RF_PAL ? 1841 : 1532));
    free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

static void run_split_vertical_cases(void) {
    for (int format=GUI_RF_PAL; format<=GUI_RF_NTSC; ++format) {
        run_split_vertical((gui_rf_format_t)format,TEST_V_SPLIT_FIRST,true);
        run_split_vertical((gui_rf_format_t)format,TEST_V_SPLIT_SHORT_FIRST,true);
        run_split_vertical((gui_rf_format_t)format,TEST_V_SPLIT_15P5_FIRST,true);
        run_split_vertical((gui_rf_format_t)format,TEST_V_SPLIT_13P5_FIRST,true);
        for (int shape=TEST_V_NEW_EQ; shape<=TEST_V_SPLIT_SECOND_H; ++shape)
            run_split_vertical((gui_rf_format_t)format,(test_vertical_t)shape,false);
        run_split_vertical((gui_rf_format_t)format,TEST_V_SHORT_INTERRUPTED,false);
        for (int shape=TEST_V_SHORT_BELOW_MIN; shape<=TEST_V_SHORT_SECOND_15P5; ++shape)
            run_split_vertical((gui_rf_format_t)format,(test_vertical_t)shape,false);
        run_split_vertical((gui_rf_format_t)format,TEST_V_SHORT_SECOND_13P5,false);
    }
}

static void run_split_gap(gui_rf_format_t format) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(format);assert(decoder);
    test_frames_t frames={0};frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(frames.rgba);size_t length;
    float *rf=make_rf_vertical(format,0.0,true,1.0,-1,TEST_V_SPLIT_SHORT_FIRST,&length);
    bool pal=format==GUI_RF_PAL;double line_us=pal ? 64.0 : 63.55555555555556;
    double broad_us=((pal ? 312.0 : 263.0)+(pal ? 1.0 : .5)+(pal ? 5.0 : 6.0)*.5)*line_us;
    size_t first=(size_t)((broad_us+31.0)*20.0);
    size_t last=(size_t)((broad_us+(pal ? 5.0 : 6.0)*line_us*.5+10.0)*20.0);
    assert(first<last && last<length);uint64_t origin=UINT64_C(6600000000);
    process_fixture(decoder,rf,first,origin,&frames);
    gui_rf_decode_status_t status;gui_rf_decoder_get_status(decoder,&status);
    assert(status.fields_accepted==0);
    /* The discontinuity occurs after both first-slot fragments, before the
     * complete confirming broad pulses. Those pulses cannot reuse its EQ. */
    process_fixture(decoder,rf+first,last-first,origin+first+200000,&frames);
    gui_rf_decoder_get_status(decoder,&status);
    assert(status.fields_accepted==0 && status.vsync_count==0 && frames.frames==0);
    process_fixture(decoder,rf,length,origin+last+200000,&frames);
    check_bars(&frames,.10);
    printf("%s split-pending discontinuity discarded origin and reacquired\n",pal ? "PAL":"NTSC");
    free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

static double row_region_mean(const test_frames_t *f,int row,int x0,int x1,
                              int channel) {
    double total=0;int count=0;
    for (int x=x0; x<x1; ++x) {
        total+=f->rgba[((size_t)row*f->width+x)*4u+channel];count++;
    }
    return count ? total/count/255.0 : 0.0;
}

static double row_region_extreme(const test_frames_t *f,int row,bool maximum) {
    double extreme=maximum ? 0.0 : 1.0;
    for (int x=0; x<f->width; ++x)
        for (int channel=0; channel<3; ++channel) {
            double value=f->rgba[((size_t)row*f->width+x)*4u+channel]/255.0;
            extreme=maximum ? fmax(extreme,value) : fmin(extreme,value);
        }
    return extreme;
}

/* Full-frame contract: every field line is woven into one interlaced frame
 * (PAL 626 / NTSC 526 rows), the VBI renders with the hybrid background
 * levels (blanking as a visible dark band, sync at the floor), the bottom
 * active rows of both parities survive (no crop), and each field's content
 * lands on its own row parity. The generator encodes the exact geometry:
 * the vertical interval is lines 0..7 (PAL) / 0..9 (NTSC), and bar 0 carries
 * per-parity gray levels as a physical field marker. */
static void run_full_frame_format(gui_rf_profile_t profile) {
    const bool pal=profile==GUI_RF_PROFILE_PAL_VHS;
    const bool beta=test_beta_profile(profile);
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(profile);assert(decoder);
    gui_rf_decoder_set_frame_mode(decoder,true);
    test_frames_t frames={.beta_display=beta};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(frames.rgba);
    size_t length;
    float *rf=make_rf_signal_profile(profile,0.0,true,1.0,-1,TEST_V_NORMAL,NULL,&length);
    process_fixture(decoder,rf,length,0,&frames);
    assert(frames.frames>=2);
    assert(frames.status.h_locked && frames.status.v_locked && frames.status.color_locked);
    assert(frames.status.fields_accepted==(uint64_t)frames.status.fields_decoded);
    assert(frames.status.fields_rejected==frames.status.reject_cadence+
        frames.status.reject_parity+frames.status.reject_line_map);
    assert(frames.height==(pal ? 626 : 526) && frames.width==(pal ? 768 : 640));

    /* Rows 0..15 (PAL) / 0..18 (NTSC) are the generator's pure vertical
     * interval: blanking must show the hybrid offset, sync the floor. The
     * minimum bound tolerates the 2.3 us equalizing pulses, which cannot
     * fully settle through the NTSC de-emphasis chain and render around
     * -34 IRE; 4.7 us H sync and 27 us broad pulses do reach the floor. */
    const int vbi_rows=pal ? 16 : 19;
    for (int row=0; row<vbi_rows; ++row) {
        double maximum=row_region_extreme(&frames,row,true);
        double minimum=row_region_extreme(&frames,row,false);
        if (maximum<=.15 || maximum>=.30 || minimum>.08)
            fprintf(stderr,"VBI row %d outside hybrid levels: max %.3f min %.3f\n",
                    row,maximum,minimum);
        assert(maximum>.15 && maximum<.30);
        assert(minimum<=.08);
    }
    /* The tail row no parity can write (PAL 624, NTSC 525) stays cleared. */
    int tail_row=pal ? 624 : 525;
    for (int x=0; x<frames.width*4; ++x)
        assert(frames.rgba[(size_t)tail_row*frames.width*4u+x]==0);

    /* Active-window columns mirror the renderer's positional split. */
    double line_us=pal ? 64.0 : 63.55555555555556;
    double active_start=pal ? 10.5 : 9.45,active_end=pal ? 62.5 : 62.55555555555556;
    const int x0=(int)ceil(active_start*frames.width/line_us-.5);
    const int x1=(int)ceil(active_end*frames.width/line_us-.5);
    /* Bars decode with the same accuracy as Active mode through the window. */
    double largest_error=0.0;
    for (int bar=1; bar<8; ++bar) for (int parity=0; parity<2; ++parity)
        for (int c=0; c<3; ++c) {
            double actual=plane_mean_window(&frames,bar,parity,c,x0,x1);
            double expected=beta ? bars[bar][c] : display_bar_channel(bar,c);
            largest_error=fmax(largest_error,fabs(actual-expected));
            assert(fabs(actual-expected)<=(beta ? .10 : .08));
        }
    /* No bottom crop: the last row each parity writes carries real bars. */
    const int last_row[2]={pal ? 622 : 524,pal ? 625 : 523};
    for (int parity=0; parity<2; ++parity) {
        int yellow=x0+(int)(1.5*(x1-x0)/8.0),black_bar=x0+(int)(7.5*(x1-x0)/8.0);
        assert(row_region_mean(&frames,last_row[parity],yellow-4,yellow+5,0)>.60);
        assert(row_region_mean(&frames,last_row[parity],black_bar-4,black_bar+5,1)<.15);
    }
    /* Adjacent-row registration: bar 0 alternates bright (even rows, parity-0
     * gray .82) and dark (odd rows, parity-1 gray .22); bars 2..6 are shared
     * content, so adjacent rows must agree. A one-row or one-field vertical
     * displacement breaks the bright/dark phase or the row agreement. */
    int marker=x0+(int)(.5*(x1-x0)/8.0);
    double even_bright=0.0,odd_bright=0.0,registration=0.0;int pairs=0;
    for (int row=100; row<298; row+=2) {
        double bright=row_region_mean(&frames,row,marker-4,marker+5,1);
        double dark=row_region_mean(&frames,row+1,marker-4,marker+5,1);
        assert(bright>.60 && dark<.40 && bright-dark>.40);
        even_bright+=bright;odd_bright+=dark;pairs++;
    }
    for (int bar=2; bar<=6; ++bar) {
        int center=x0+(int)((bar+.5)*(x1-x0)/8.0);
        for (int row=100; row<298; row+=2)
            registration=fmax(registration,fabs(
                row_region_mean(&frames,row,center-4,center+5,1)
               -row_region_mean(&frames,row+1,center-4,center+5,1)));
    }
    assert(pairs>90);
    assert(fabs(even_bright/pairs-(beta ? .82 : display_level(.82)))<=.06);
    assert(fabs(odd_bright/pairs-(beta ? .22 : display_level(.22)))<=.06);
    assert(registration<=.08);

    /* Display levels on one woven active row (row 200 = line 100, parity 0):
     * the sync tip at the floor, back porch at the hybrid background level,
     * and the selected profile's active black on the SAME row. */
    const int porch_x0=pal ? 100 : 82,porch_x1=pal ? 120 : 92;
    double sync=row_region_mean(&frames,200,16,pal ? 48 : 44,1);
    double porch=row_region_mean(&frames,200,porch_x0,porch_x1,1);
    int black_bar=x0+(int)(7.5*(x1-x0)/8.0);
    double black=row_region_mean(&frames,200,black_bar-4,black_bar+5,1);
    assert(sync<=.06);
    assert(fabs(porch-(pal ? 38.7543/174.147 : 44.1843/197.680))<=.06);
    assert(fabs(black-(beta ? 0.0 : display_level(0.0)))<=.04);
    printf("%s full frame: %dx%d, %d frames, bars max error %.3f, porch %.3f, sync %.3f, black %.3f, registration %.3f\n",
           gui_rf_profile_name(profile),frames.width,frames.height,frames.frames,
           largest_error,porch,sync,black,registration);
    fflush(stdout);
    /* The same profile returns to its original Active geometry/transfer,
     * including after a decoder reset; no Full-mode field may leak across. */
    gui_rf_decoder_set_frame_mode(decoder,false);
    gui_rf_decoder_reset_profile(decoder,profile);
    frames.frames=0;frames.end_sample=0;
    process_fixture(decoder,rf,length,0,&frames);
    assert(frames.height==(pal ? 576 : 480));
    check_bars(&frames,beta ? .10 : .08);
    free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

static void run_full_frame_contracts(void) {
    /* This QAM fixture models PAL/NTSC VHS and Beta only. Native SECAM's
     * independent FM oracle covers its Active/Full geometry separately. */
    for (int profile=GUI_RF_PROFILE_PAL_VHS;profile<=GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI;++profile)
        run_full_frame_format((gui_rf_profile_t)profile);
}

/* Optional external-media regression for the exact 20 MHz mono s16le excerpt
 * of DanDanYouQing-DHX2.ddd.flac, source seconds 5..7. Its SMPTE bars are an
 * independent polarity oracle: an inverted V can pass a synthetic fixture
 * whose modulator shares that sign error, but turns cyan/magenta/red/green
 * into the wrong hues here. This is a hue/cadence check, not archive parity.
 * SHA256: A054AC393B3CCDD69BE1B3A6E3ED8A32A390DEBD186D40A4FB89C36FA53283D2 */
static void run_real_ntsc_bars(const char *path) {
    FILE *input=fopen(path,"rb"); assert(input);
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(GUI_RF_NTSC); assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(frames.rgba);
    uint8_t packed[65536]; float rf[32768];
    uint64_t position=0; size_t bytes;
    while ((bytes=fread(packed,1,sizeof(packed),input))!=0) {
        assert((bytes&1u)==0);
        size_t count=bytes/2u;
        for (size_t n=0; n<count; ++n) {
            unsigned word=(unsigned)packed[n*2u]|((unsigned)packed[n*2u+1u]<<8);
            int sample=word<32768u ? (int)word : (int)word-65536;
            rf[n]=sample/32768.0f;
        }
        gui_rf_decoder_process(decoder,rf,count,position,receive_frame,&frames);
        position+=count;
    }
    assert(!ferror(input)); fclose(input);
    assert(position==UINT64_C(40000000));
    assert(frames.width==640 && frames.height==480 && frames.frames>=55);
    assert(frames.status.h_locked && frames.status.v_locked && frames.status.color_locked);
    assert(frames.status.fields_accepted==(uint64_t)frames.status.fields_decoded);
    assert(frames.status.fields_rejected==frames.status.reject_cadence+
        frames.status.reject_parity+frames.status.reject_line_map);
    for (int bar=1; bar<7; ++bar) for (int parity=0; parity<2; ++parity) {
        double minimum_on=1.0, maximum_off=0.0;
        for (int c=0; c<3; ++c) {
            double value=plane_mean(&frames,bar,parity,c);
            if (bars[bar][c]>0.5) minimum_on=fmin(minimum_on,value);
            else maximum_off=fmax(maximum_off,value);
        }
        if (minimum_on-maximum_off<=.20)
            fprintf(stderr,"real NTSC bar %d field %d has wrong hue: %.3f vs %.3f\n",
                    bar,parity,minimum_on,maximum_off);
        assert(minimum_on-maximum_off>.20);
    }
    printf("Real NTSC RF bars: %d woven frames, all sync/color locks, correct hue order\n",
           frames.frames);
    free(frames.rgba); gui_rf_decoder_destroy(decoder);
}

/* One-thread steady-state DSP throughput at the 20 MHz decode rate: the full
 * locked pipeline (FM demod, H/V sync, line map, chroma decode, weave) over a
 * looping 16-field fixture. Reports the realtime multiple one worker needs
 * and its share of one core — the measured input for queue-drop budget
 * decisions, never an assumption. */
static void run_bench(gui_rf_format_t format, double seconds) {
    test_quality_t quality={.pattern=TEST_Q_GRAY,.fields=16};
    size_t length;
    float *rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&quality,&length);
    assert(rf && length>0);
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(format); assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(frames.rgba);
    /* Warm-up: reach the locked steady state before measuring. */
    gui_rf_decoder_process(decoder,rf,length,0,receive_frame,&frames);
    gui_rf_decode_status_t before; gui_rf_decoder_get_status(decoder,&before);
    assert(before.v_locked);
    uint64_t loops=(uint64_t)((seconds*20000000.0)/(double)length);
    if (loops<1) loops=1;
    struct timespec wall0,wall1,cpu0,cpu1;
    clock_gettime(CLOCK_MONOTONIC,&wall0);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu0);
    uint64_t position=length;
    for (uint64_t i=0;i<loops;++i) {
        gui_rf_decoder_process(decoder,rf,length,position,receive_frame,&frames);
        position+=length;
    }
    clock_gettime(CLOCK_MONOTONIC,&wall1);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu1);
    double wall=(double)(wall1.tv_sec-wall0.tv_sec)+1e-9*(double)(wall1.tv_nsec-wall0.tv_nsec);
    double cpu=(double)(cpu1.tv_sec-cpu0.tv_sec)+1e-9*(double)(cpu1.tv_nsec-cpu0.tv_nsec);
    gui_rf_decode_status_t after; gui_rf_decoder_get_status(decoder,&after);
    double content=(double)loops*(double)length/20000000.0;
    double fields=(double)(after.fields_decoded-before.fields_decoded);
    printf("%s bench: %.2f s content in %.3f s wall / %.3f s cpu -> %.2fx realtime, %.0f%% of one core, %.2f ms/field, %llu frames\n",
           format==GUI_RF_PAL ? "PAL" : "NTSC",content,wall,cpu,
           wall>0 ? content/wall : 0,wall>0 ? 100.0*cpu/wall : 0,
           fields>0 ? 1000.0*cpu/fields : 0,
           (unsigned long long)frames.frames);
    fflush(stdout);
    free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}


/* Betamax video contracts use raw RF from the physical modulator above. They
 * cover both head hypotheses, profile isolation, color loss and gain scaling;
 * they do not qualify real Beta I/II/III tapes or hardware. */
static void run_beta_contracts(void) {
    assert(!gui_rf_decoder_create_profile((gui_rf_profile_t)-1));
    assert(!gui_rf_decoder_create_profile(GUI_RF_PROFILE_COUNT));
    assert(gui_rf_profile_format(GUI_RF_PROFILE_PAL_VHS)==GUI_RF_PAL);
    assert(gui_rf_profile_format(GUI_RF_PROFILE_NTSC_BETAMAX)==GUI_RF_NTSC);
    float coefficients[5];
    assert(gui_rf_deemph_coefficients(TEST_RATE/4.0,8.0,100000.0,.5,
                                      coefficients,coefficients+1,coefficients+2,coefficients+3,coefficients+4));
    assert(fabs(transfer_magnitude(coefficients,0)-1.0)<2e-5);
    assert(fabs(transfer_magnitude(coefficients,TEST_PI)-pow(10,-8.0/20.0))<2e-5);
    assert(fabs(transfer_magnitude(coefficients,2.0*TEST_PI*100000.0/(TEST_RATE/4.0))
                -pow(10,-8.0/40.0))<2e-5);
    const double bad_parameters[][3]={{-1,100000,.5},{INFINITY,100000,.5},
        {8,0,.5},{8,NAN,.5},{8,100000,0},{8,100000,1e-320}};
    for(size_t n=0;n<sizeof(bad_parameters)/sizeof(bad_parameters[0]);++n) {
        const float sentinel[5]={17,-23,31,-43,59};memcpy(coefficients,sentinel,sizeof(sentinel));
        assert(!gui_rf_deemph_coefficients(TEST_RATE/4.0,bad_parameters[n][0],
            bad_parameters[n][1],bad_parameters[n][2],coefficients,coefficients+1,
            coefficients+2,coefficients+3,coefficients+4));
        assert(memcmp(coefficients,sentinel,sizeof(sentinel))==0);
    }
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(GUI_RF_PROFILE_NTSC_BETAMAX);assert(decoder);
    test_frames_t frames={.beta_display=true};frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(frames.rgba);
    for (int profile=GUI_RF_PROFILE_NTSC_BETAMAX;profile<=GUI_RF_PROFILE_NTSC_BETAMAX_HIFI;++profile) {
        for (int swap=0;swap<2;++swap) {
            test_quality_t signal={.pattern=TEST_Q_COLOR_BARS,.fields=8,.swap_tracks=swap,
                .beta_audio_amplitude=profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI ? .12 : 0.0};
            size_t length;float *rf=make_rf_signal_profile((gui_rf_profile_t)profile,.14,true,1.001,-1,
                                                         TEST_V_NORMAL,&signal,&length);
            gui_rf_decoder_reset_profile(decoder,(gui_rf_profile_t)profile);frames.frames=0;frames.end_sample=0;
            process_fixture(decoder,rf,length,0,&frames);
            printf("%s head order %d: ",gui_rf_profile_name((gui_rf_profile_t)profile),swap);
            check_bars(&frames,.10);
            gui_rf_decode_status_t before_invalid,after_invalid;
            gui_rf_decoder_get_status(decoder,&before_invalid);
            gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_COUNT);
            gui_rf_decoder_get_status(decoder,&after_invalid);
            assert(memcmp(&before_invalid,&after_invalid,sizeof(before_invalid))==0);
            size_t pixels=(size_t)frames.width*frames.height*4u;
            uint8_t *reference=malloc(pixels);assert(reference);memcpy(reference,frames.rgba,pixels);
            for (size_t n=0;n<length;++n) rf[n]*=1.0f/16.0f;
            gui_rf_decoder_reset_profile(decoder,(gui_rf_profile_t)profile);frames.end_sample=0;
            process_fixture(decoder,rf,length,0,&frames);check_bars(&frames,.10);
            double difference=0;for(size_t n=0;n<pixels;++n) difference+=abs((int)frames.rgba[n]-reference[n]);
            assert(difference/pixels<.12);free(reference);free(rf);
            /* A gap must reset filters/partial fields without reverting to VHS. */
            float silence[4096]={0};uint64_t origin=UINT64_C(9000000000);
            gui_rf_decoder_process(decoder,silence,4096,origin,receive_frame,&frames);
            gui_rf_decode_status_t state;gui_rf_decoder_get_status(decoder,&state);
            assert(!state.h_locked && !state.v_locked && !state.color_locked && state.frames_decoded==0);
            assert(strstr(state.reason,"Betamax") || strstr(state.reason,"Beta Hi-Fi") || strstr(state.reason,"RF gap"));
            rf=make_rf_signal_profile((gui_rf_profile_t)profile,0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
            int before=frames.frames;process_fixture(decoder,rf,length,origin+4096,&frames);
            assert(frames.frames>=before+2);check_bars(&frames,.10);
            uint64_t continuation=origin+4096+length;free(rf);
            rf=make_rf_signal_profile((gui_rf_profile_t)profile,0,false,1.0,-1,TEST_V_NORMAL,&signal,&length);
            process_fixture(decoder,rf,length,continuation,&frames);
            assert(frames.status.h_locked && frames.status.v_locked && !frames.status.color_locked);
            for (int bar=1;bar<8;++bar) {
                double red=plane_mean(&frames,bar,0,0),green=plane_mean(&frames,bar,0,1),blue=plane_mean(&frames,bar,0,2);
                assert(fabs(red-green)<.01 && fabs(red-blue)<.01);
            }
            free(rf);
        }
        /* Independent gray levels expose a wrong blank/Hz-per-IRE profile. */
        test_quality_t gray={.pattern=TEST_Q_GRAY,.fields=8,.window_noise_hz=40000};
        size_t length;float *rf=make_rf_signal_profile((gui_rf_profile_t)profile,0,true,1.0,-1,TEST_V_NORMAL,&gray,&length);
        gui_rf_decoder_reset_profile(decoder,(gui_rf_profile_t)profile);frames.frames=0;frames.end_sample=0;
        process_fixture(decoder,rf,length,0,&frames);
        check_quality_gray(&frames,gui_rf_profile_name((gui_rf_profile_t)profile),0,.035,.015,.06);
        free(rf);
        gray.pattern=TEST_Q_MID_BARS;gray.window_noise_hz=0;gray.switch_track_line=247;
        rf=make_rf_signal_profile((gui_rf_profile_t)profile,.12,true,1.001,-1,TEST_V_NORMAL,&gray,&length);
        gui_rf_decoder_reset_profile(decoder,(gui_rf_profile_t)profile);frames.frames=0;frames.end_sample=0;
        process_fixture(decoder,rf,length,0,&frames);
        assert(frames.frames>=2 && frames.status.color_locked);
        check_bar_registration(&frames);free(rf);
    }
    free(frames.rgba);gui_rf_decoder_destroy(decoder);
}


/* Acquisition checks use only physical RF and public lock/frame observations.
 * The ordinary Beta video API has no PCM sink: audio-off routing/zero audio
 * work belongs to the worker integration/real-RF probes, not this DSP fixture. */
typedef struct {
    uint64_t transition_sample,last_unlocked_sample;
    int frames_at_loss;
    bool locked_before_change,lost_after_change,recovered_after_change;
} test_beta_acquire_observation_t;

static void process_beta_acquire_chunks(gui_rf_decoder_t *decoder,const float *rf,
                                        size_t length,uint64_t offset,bool varied,
                                        test_frames_t *frames,
                                        test_beta_acquire_observation_t *observation) {
    static const size_t chunks[]={1u,4093u,8191u,257u};
    size_t position=0,block=0;
    while(position<length) {
        size_t count=varied ? chunks[block++%4u] : 8192u;
        if(count>length-position) count=length-position;
        gui_rf_decoder_process(decoder,rf+position,count,offset+position,receive_frame,frames);
        position+=count;
        if(observation) {
            gui_rf_decode_status_t status;gui_rf_decoder_get_status(decoder,&status);
            uint64_t end=offset+position;
            bool locked=status.h_locked && status.v_locked;
            if(!locked) observation->last_unlocked_sample=end;
            if(end<=observation->transition_sample && locked && frames->frames>0)
                observation->locked_before_change=true;
            if(end>observation->transition_sample && observation->locked_before_change) {
                if(!locked && !observation->lost_after_change) {
                    observation->lost_after_change=true;
                    observation->frames_at_loss=frames->frames;
                }
                if(observation->lost_after_change && locked &&
                   frames->frames>observation->frames_at_loss)
                    observation->recovered_after_change=true;
            }
        }
    }
}

static void check_beta_acquired(gui_rf_decoder_t *decoder,const test_frames_t *frames,
                                 const char *label) {
    gui_rf_decode_status_t status;gui_rf_decoder_get_status(decoder,&status);
    printf("%s: frames=%d H=%d V=%d C=%d rejects=%llu\n",label,frames->frames,
        status.h_locked,status.v_locked,status.color_locked,
        (unsigned long long)status.fields_rejected);
    fflush(stdout);
    assert(frames->frames>=2 && status.h_locked && status.v_locked);
    assert(frames->width>=64 && frames->height>=160);
    double low=1.0,high=0.0;
    for(int parity=0;parity<2;parity++) for(int bar=1;bar<8;bar++) {
        double y=.299*plane_mean(frames,bar,parity,0)
                +.587*plane_mean(frames,bar,parity,1)
                +.114*plane_mean(frames,bar,parity,2);
        low=fmin(low,y);high=fmax(high,y);
    }
    /* Publishing uniformly black/white pixels is not a recovered picture.
     * A broad contrast floor permits the deliberately mismatched FM profile. */
    printf("  active picture Y span %.4f\n",high-low);fflush(stdout);
    assert(high-low>.04);
}

static void beta_invalid_rf(gui_rf_decoder_t *decoder,bool noise,size_t length,
                            uint64_t offset,test_frames_t *frames) {
    float samples[4096];uint32_t random=0x5188e4c3u;
    double phase=.73,step=2.0*TEST_PI*4250000.0/TEST_RATE;
    size_t position=0;
    while(position<length) {
        size_t count=length-position;if(count>4096u) count=4096u;
        for(size_t n=0;n<count;n++) {
            if(noise) {
                random=random*1664525u+1013904223u;
                samples[n]=(float)(.72*((double)(random>>8)/8388608.0-1.0));
            } else {
                samples[n]=(float)(.72*cos(phase));phase+=step;
                if(phase>2.0*TEST_PI) phase-=2.0*TEST_PI;
            }
        }
        gui_rf_decoder_process(decoder,samples,count,offset+position,receive_frame,frames);
        position+=count;
    }
}

static void run_beta_acquire_contracts(void) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(GUI_RF_PROFILE_NTSC_BETAMAX);
    assert(decoder);
    test_frames_t frames={.beta_display=true};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(frames.rgba);
    test_quality_t hifi={.pattern=TEST_Q_COLOR_BARS,.fields=10,.beta_audio_amplitude=.12};
    size_t length;float *rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_BETAMAX_HIFI,.08,true,
        1.0,-1,TEST_V_NORMAL,&hifi,&length);
    for(int varied=0;varied<2;varied++) {
        gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_NTSC_BETAMAX);
        frames.frames=0;frames.end_sample=0;
        test_beta_acquire_observation_t observation={.transition_sample=UINT64_MAX};
        process_beta_acquire_chunks(decoder,rf,length,0,varied!=0,&frames,&observation);
        check_beta_acquired(decoder,&frames,varied ? "HiFi RF / Beta Off irregular blocks" : "HiFi RF / Beta Off 8192 blocks");
        /* A frame pair must keep arriving after acquisition, not just a stale
         * H/V flag or an isolated frame before a later loss. Wrong-profile RGB
         * calibration is intentionally not equated with HiFi-profile quality. */
        assert(length-observation.last_unlocked_sample>=(uint64_t)(TEST_RATE*.035));
    }
    free(rf);

    /* Continuous oscillator phase and sample positions, with an actual change
     * of recorded FM levels after six ordinary-Beta fields. The later sync and
     * blank are HiFi nominal plus 250kHz; neither ADC gain nor decoder state is
     * patched. A gray source keeps the luma RF inside the ordinary passband. */
    test_quality_t changing={.pattern=TEST_Q_GRAY,.fields=16,.fm_change_field=6,
        .fm_blank_after_hz=4450000.0,.fm_sync_after_hz=4050000.0,
        .fm_hz_per_ire_after=10000.0};
    rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_BETAMAX,0,true,1.0,-1,
        TEST_V_NORMAL,&changing,&length);
    gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_NTSC_BETAMAX);
    frames.frames=0;frames.end_sample=0;
    test_beta_acquire_observation_t changed={.transition_sample=changing.field_start_sample[6]};
    process_beta_acquire_chunks(decoder,rf,length,0,true,&frames,&changed);
    check_beta_acquired(decoder,&frames,"Continuous Beta FM level change");
    assert(changed.locked_before_change && changed.lost_after_change && changed.recovered_after_change);
    assert(frames.frames>=changed.frames_at_loss+2);
    assert(length-changed.last_unlocked_sample>=(uint64_t)(TEST_RATE*.035));
    free(rf);

    /* Neither a high-energy unmodulated carrier nor random RF supplies V/EQ
     * cadence. Observe V and frame publication explicitly, not only H lock. */
    for(int noise=0;noise<2;noise++) {
        gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_NTSC_BETAMAX);
        frames.frames=0;frames.end_sample=0;
        beta_invalid_rf(decoder,noise!=0,(size_t)(TEST_RATE*.12),0,&frames);
        gui_rf_decode_status_t status;gui_rf_decoder_get_status(decoder,&status);
        printf("Beta %s negative: frames=%d H=%d V=%d\n",noise ? "random RF" : "carrier-only",
            frames.frames,status.h_locked,status.v_locked);fflush(stdout);
        assert(!status.v_locked && !status.color_locked && frames.frames==0);
        if(!noise) assert(!status.h_locked);
    }

    /* A previously locked decoder can retain believable H pulses while V is
     * missing. New RF must still be discoverable after that stale-H interval. */
    test_quality_t ordinary={.pattern=TEST_Q_GRAY,.fields=6};
    rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_BETAMAX,0,true,1.0,-1,
        TEST_V_NORMAL,&ordinary,&length);
    gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_NTSC_BETAMAX);
    frames.frames=0;frames.end_sample=0;
    process_beta_acquire_chunks(decoder,rf,length,0,true,&frames,NULL);
    check_beta_acquired(decoder,&frames,"Beta before H-only interval");
    uint64_t offset=length;free(rf);
    test_quality_t h_only={.pattern=TEST_Q_GRAY,.fields=8};
    rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_BETAMAX,0,false,1.0,-1,
        TEST_V_H_ONLY,&h_only,&length);
    size_t settling=(size_t)h_only.field_start_sample[2];
    process_beta_acquire_chunks(decoder,rf,settling,offset,true,&frames,NULL);
    int before=frames.frames;
    process_beta_acquire_chunks(decoder,rf+settling,length-settling,offset+settling,true,&frames,NULL);
    gui_rf_decode_status_t missing_v;gui_rf_decoder_get_status(decoder,&missing_v);
    assert(frames.frames==before && !missing_v.v_locked);
    offset+=length;free(rf);
    test_quality_t shifted={.pattern=TEST_Q_GRAY,.fields=10,.carrier_offset_hz=250000.0,
        .beta_audio_amplitude=.12};
    rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_BETAMAX_HIFI,0,true,1.0,-1,
        TEST_V_NORMAL,&shifted,&length);
    frames.frames=0;
    size_t two_fields=(size_t)shifted.field_start_sample[2];
    process_beta_acquire_chunks(decoder,rf,two_fields,offset,true,&frames,NULL);
    /* Old scan-window samples/partial fields must not become a rendered frame. */
    assert(frames.frames==0);
    process_beta_acquire_chunks(decoder,rf+two_fields,length-two_fields,offset+two_fields,true,&frames,NULL);
    check_beta_acquired(decoder,&frames,"HiFi FM offset after stale H-only interval");
    assert(frames.first_end_sample>=offset+two_fields);
    free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

/* Check three distinct nonzero Y/C steps. A small profile-local calibration
 * must hold across the tape clock, line jitter and both original fields,
 * rather than aligning only one favorable synthetic red/blue transition. */
static void check_beta_edges(const test_frames_t *f,const char *name,double clock,double jitter) {
    int largest_separation=0;double largest_centroid=0;
    for(int edge=4;edge<=6;++edge) for(int parity=0;parity<2;++parity) {
        int center=f->width*edge/8,radius=f->width/32;
        double previous_y=0,previous_u=0,previous_v=0,y_gradient=0,c_gradient=0;
        double y_sum=0,c_sum=0,y_moment=0,c_moment=0;int y_edge=0,c_edge=0;
        for(int x=center-radius;x<=center+radius;++x) {
            double rgb[3]={0};int count=0;
            for(int row=120+parity;row<160;row+=2) {
                for(int channel=0;channel<3;++channel)
                    rgb[channel]+=f->rgba[((size_t)row*f->width+x)*4u+channel]/255.0;
                count++;
            }
            for(int channel=0;channel<3;++channel) rgb[channel]/=count;
            double y=.299*rgb[0]+.587*rgb[1]+.114*rgb[2];
            double u=.492*(rgb[2]-y),v=.877*(rgb[0]-y);
            if(x>center-radius) {
                double dy=fabs(y-previous_y),dc=hypot(u-previous_u,v-previous_v);
                if(dy>y_gradient) {y_gradient=dy;y_edge=x;}
                if(dc>c_gradient) {c_gradient=dc;c_edge=x;}
                y_sum+=dy;c_sum+=dc;y_moment+=x*dy;c_moment+=x*dc;
            }
            previous_y=y;previous_u=u;previous_v=v;
        }
        int separation=abs(y_edge-c_edge);
        if(separation>largest_separation) largest_separation=separation;
        assert(y_gradient>.005 && c_gradient>.01 && separation<=5);
        double centroid=fabs(c_moment/c_sum-y_moment/y_sum);
        if(centroid>largest_centroid) largest_centroid=centroid;
    }
    printf("%s midtone registration clock %.3f jitter %.2fus: max edge %dpx, centroid %.3fpx\n",
        name,clock,jitter,largest_separation,largest_centroid);
}

/* These ordinary-SuperBeta fixtures exercise the independent 4.4/5.6MHz
 * modulator. They qualify the reduced-bandwidth preview on synthetic RF only. */
static void run_superbeta_contracts(void) {
    _Static_assert(GUI_RF_PROFILE_PAL_VHS==0 && GUI_RF_PROFILE_NTSC_VHS==1 &&
                   GUI_RF_PROFILE_NTSC_BETAMAX==2 && GUI_RF_PROFILE_NTSC_BETAMAX_HIFI==3,
                   "Existing persisted RF profile IDs must not move");
    assert(!gui_rf_profile_has_hifi((gui_rf_profile_t)-1));
    assert(!gui_rf_profile_has_hifi(GUI_RF_PROFILE_COUNT));
    assert(!gui_rf_profile_has_hifi((gui_rf_profile_t)1000));
    for(int profile=GUI_RF_PROFILE_PAL_VHS;profile<GUI_RF_PROFILE_COUNT;++profile) {
        bool expected=profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI ||
                      profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI || profile==GUI_RF_PROFILE_NTSC_ED_BETA;
        assert(gui_rf_profile_has_hifi((gui_rf_profile_t)profile)==expected);
    }
    assert(gui_rf_profile_format(GUI_RF_PROFILE_NTSC_SUPERBETA)==GUI_RF_NTSC);
    assert(gui_rf_profile_format(GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI)==GUI_RF_NTSC);
    assert(strcmp(gui_rf_profile_name(GUI_RF_PROFILE_NTSC_SUPERBETA),"NTSC SuperBeta")==0);
    assert(strcmp(gui_rf_profile_name(GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI),"NTSC SuperBeta Hi-Fi")==0);
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(GUI_RF_PROFILE_NTSC_SUPERBETA);assert(decoder);
    test_frames_t frames={.beta_display=true};
    size_t capacity=(size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u;
    frames.rgba=malloc(capacity);uint8_t *reference=malloc(capacity);assert(frames.rgba && reference);
    for(int swap=0;swap<2;++swap) {
        test_quality_t signal={.pattern=TEST_Q_COLOR_BARS,.fields=8,.swap_tracks=swap,
            .beta_audio_amplitude=swap ? .12 : 0.0};
        size_t length;float *rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_SUPERBETA,.14,true,
            1.001,-1,TEST_V_NORMAL,&signal,&length);
        int reference_frames=0;uint64_t reference_end=0;
        for(int profile=GUI_RF_PROFILE_NTSC_SUPERBETA;profile<=GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI;++profile) {
            gui_rf_decoder_reset_profile(decoder,(gui_rf_profile_t)profile);frames.frames=0;frames.end_sample=0;
            process_fixture(decoder,rf,length,0,&frames);
            printf("%s head order %d audio RF %.2f: ",gui_rf_profile_name((gui_rf_profile_t)profile),swap,
                signal.beta_audio_amplitude);check_bars(&frames,.10);
            assert(frames.width==640 && frames.height==480);
            size_t pixels=(size_t)frames.width*frames.height*4u;
            if(profile==GUI_RF_PROFILE_NTSC_SUPERBETA) {
                memcpy(reference,frames.rgba,pixels);reference_frames=frames.frames;reference_end=frames.end_sample;
            } else {
                assert(frames.frames==reference_frames && frames.end_sample==reference_end);
                assert(memcmp(reference,frames.rgba,pixels)==0);
            }
            gui_rf_decode_status_t before,after;gui_rf_decoder_get_status(decoder,&before);
            gui_rf_decoder_reset_profile(decoder,(gui_rf_profile_t)-1);gui_rf_decoder_get_status(decoder,&after);
            assert(memcmp(&before,&after,sizeof(before))==0);
        }
        for(size_t n=0;n<length;++n) rf[n]*=1.0f/16.0f;
        gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_NTSC_SUPERBETA);frames.frames=0;frames.end_sample=0;
        process_fixture(decoder,rf,length,0,&frames);check_bars(&frames,.10);
        size_t pixels=(size_t)frames.width*frames.height*4u;
        double difference=0;for(size_t n=0;n<pixels;++n) difference+=abs((int)frames.rgba[n]-reference[n]);
        assert(difference/pixels<.12);free(rf);
    }
    for(int profile=GUI_RF_PROFILE_NTSC_SUPERBETA;profile<=GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI;++profile) {
        test_quality_t gray={.pattern=TEST_Q_GRAY,.fields=8,.window_noise_hz=40000};
        size_t length;float *rf=make_rf_signal_profile((gui_rf_profile_t)profile,0,true,1.0,-1,
            TEST_V_NORMAL,&gray,&length);
        gui_rf_decoder_reset_profile(decoder,(gui_rf_profile_t)profile);frames.frames=0;frames.end_sample=0;
        process_fixture(decoder,rf,length,0,&frames);
        check_quality_gray(&frames,gui_rf_profile_name((gui_rf_profile_t)profile),0,.035,.015,.06);free(rf);
        gray.pattern=TEST_Q_MID_BARS;gray.window_noise_hz=0;gray.switch_track_line=247;
        rf=make_rf_signal_profile((gui_rf_profile_t)profile,.12,true,1.001,-1,TEST_V_NORMAL,&gray,&length);
        gui_rf_decoder_reset_profile(decoder,(gui_rf_profile_t)profile);frames.frames=0;frames.end_sample=0;
        process_fixture(decoder,rf,length,0,&frames);
        assert(frames.frames>=2 && frames.status.color_locked);check_bar_registration(&frames);free(rf);
        uint64_t offset=length;
        rf=make_rf_signal_profile((gui_rf_profile_t)profile,0,false,1.0,-1,TEST_V_NORMAL,&gray,&length);
        process_fixture(decoder,rf,length,offset,&frames);
        gui_rf_decode_status_t status;gui_rf_decoder_get_status(decoder,&status);
        assert(status.h_locked && status.v_locked && !status.color_locked && status.color_losses>0);
        for(int bar=1;bar<8;++bar) for(int parity=0;parity<2;++parity) {
            double r=plane_mean(&frames,bar,parity,0),g=plane_mean(&frames,bar,parity,1),b=plane_mean(&frames,bar,parity,2);
            assert(fabs(r-g)<.01 && fabs(r-b)<.01);
        }
        free(rf);
    }
    const double clocks[]={.999,1.0,1.001};
    for(int clock=0;clock<3;++clock) for(int jitter=0;jitter<2;++jitter) {
        test_quality_t signal={.pattern=TEST_Q_MID_BARS,.fields=6,
            .beta_audio_amplitude=.12,.switch_track_line=247};
        size_t length;float *rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_SUPERBETA,jitter*.12,
            true,clocks[clock],-1,TEST_V_NORMAL,&signal,&length);
        gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_NTSC_SUPERBETA);frames.frames=0;frames.end_sample=0;
        process_fixture(decoder,rf,length,0,&frames);
        assert(frames.frames>=2 && frames.status.h_locked && frames.status.v_locked && frames.status.color_locked);
        check_beta_edges(&frames,"SuperBeta",clocks[clock],jitter*.12);free(rf);
    }
    free(reference);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

/* ED/S-VHS input positions are 20 MSPS; publication stays on the common 20 MSPS
 * clock. Compare every woven RGBA frame across input partitions. */
typedef struct {
    test_frames_t frame;
    uint64_t ends[16], hashes[16];
} test_ed_sequence_t;
static void receive_ed_frame(void *context,const uint8_t *rgba,int width,int height,
                             uint64_t end_sample,const gui_rf_decode_status_t *status) {
    test_ed_sequence_t *sequence=context;
    assert(sequence->frame.frames<16);
    size_t slot=(size_t)sequence->frame.frames;
    uint64_t hash=UINT64_C(14695981039346656037);
    for(size_t n=0;n<(size_t)width*height*4u;n++) { hash^=rgba[n];hash*=UINT64_C(1099511628211); }
    sequence->ends[slot]=end_sample;sequence->hashes[slot]=hash;
    receive_frame(&sequence->frame,rgba,width,height,end_sample,status);
}
static void process_ed_chunks(gui_rf_decoder_t *decoder,const float *rf,size_t length,
                              uint64_t origin,bool varied,test_ed_sequence_t *sequence) {
    static const size_t chunks[]={1u,4093u,32771u,257u};
    size_t position=0,block=0;
    while(position<length) {
        size_t count=varied ? chunks[block++%4u] : 8192u;
        if(count>length-position) count=length-position;
        gui_rf_decoder_process(decoder,rf+position,count,origin+position,receive_ed_frame,sequence);
        position+=count;
    }
}

/* External libm calls prevent constant-folded NaN/signed-zero expectations
 * under the same -ffast-math flags used by production RF DSP. */
static double (*volatile test_phase_reference)(double,double)=atan2;
static float (*volatile test_phase_reference_float)(float,float)=atan2f;
static uint64_t test_phase_bits(double value) {
    uint64_t bits;memcpy(&bits,&value,sizeof(bits));return bits;
}
static double test_phase_from_bits(uint64_t bits) {
    double value;memcpy(&value,&bits,sizeof(value));return value;
}
static bool test_phase_nan(double value) {
    uint64_t bits=test_phase_bits(value);
    return (bits&UINT64_C(0x7ff0000000000000))==UINT64_C(0x7ff0000000000000)
        && (bits&UINT64_C(0x000fffffffffffff))!=0;
}
static void check_ed_phase_finite(double x,double z,bool legacy,
                                  double *maximum,double *maximum_legacy) {
    double actual=gui_rf_preview_phase(x,z),expected=test_phase_reference(x,z);
    assert(gui_rf_deemph_finite(actual) && gui_rf_deemph_finite(expected));
    double error=fabs(actual-expected);
    *maximum=fmax(*maximum,error);
    assert(error<=2e-8 && actual>=-TEST_PI && actual<=TEST_PI);
    /* Some fast-math platform libm atan2 implementations discard -0 on
     * this finite axis. Its mathematical result keeps the input x sign. */
    if((test_phase_bits(x)<<1)==0 && z>0) {
        if(test_phase_bits(actual)!=test_phase_bits(x))
            fprintf(stderr,"phase zero mismatch:x=%.17g z=%.17g actual=%.17g expected=%.17g "
                "bits x=%016llx z=%016llx actual=%016llx expected=%016llx\n",x,z,actual,expected,
                (unsigned long long)test_phase_bits(x),(unsigned long long)test_phase_bits(z),
                (unsigned long long)test_phase_bits(actual),(unsigned long long)test_phase_bits(expected));
        assert(test_phase_bits(actual)==test_phase_bits(x));
    }
    if(legacy) {
        double reference=test_phase_reference_float((float)x,(float)z);
        double difference=fabs(actual-reference);
        *maximum_legacy=fmax(*maximum_legacy,difference);
        assert(difference<=5e-7);
    }
}
static void run_ed_phase_contract(void) {
    double maximum=0,maximum_legacy=0;
    const double scales[]={1e-150,1e-20,1.0,1e20,1e150};
    for(size_t scale=0;scale<sizeof(scales)/sizeof(scales[0]);scale++)
        for(int point=0;point<=50000;point++) {
            double angle=-TEST_PI+2.0*TEST_PI*point/50000.0;
            check_ed_phase_finite(scales[scale]*sin(angle),scales[scale]*cos(angle),
                                    scale>=1 && scale<=3,&maximum,&maximum_legacy);
        }
    const double transition=.4142135623730950488;
    const double ratios[]={0,nextafter(transition,0),transition,nextafter(transition,1),
                            nextafter(1.0,0),1.0,nextafter(1.0,2)};
    for(size_t n=0;n<sizeof(ratios)/sizeof(ratios[0]);n++)
        for(int sx=-1;sx<=1;sx+=2) for(int sz=-1;sz<=1;sz+=2) {
            check_ed_phase_finite(sx*ratios[n],sz,true,&maximum,&maximum_legacy);
            check_ed_phase_finite(sx,sz*ratios[n],true,&maximum,&maximum_legacy);
        }
    /* These finite doubles overflow/underflow a float cast. Compare with
     * atan2(double),not the less accurate legacy conversion to atan2f. */
    const double extremes[][2]={{DBL_MAX,DBL_MAX*.5},{DBL_MAX*.5,DBL_MAX},
        {DBL_MIN,DBL_MIN*2},{DBL_MIN*2,DBL_MIN},{DBL_MAX,DBL_MIN},{DBL_MIN,DBL_MAX}};
    for(size_t n=0;n<sizeof(extremes)/sizeof(extremes[0]);n++)
        for(int sx=-1;sx<=1;sx+=2) for(int sz=-1;sz<=1;sz+=2)
            check_ed_phase_finite(sx*extremes[n][0],sz*extremes[n][1],false,
                                    &maximum,&maximum_legacy);
    /* Volatile bit inputs retain both zero signs and nonfinite values even
     * with finite-math-only assumptions. Fallback must preserve atan2f's
     * actual result,including the sign of zero and the +/-pi branch cut. */
    volatile uint64_t special[]={UINT64_C(0),UINT64_C(0x8000000000000000),
        UINT64_C(0x3ff0000000000000),UINT64_C(0xbff0000000000000),
        UINT64_C(0x7ff0000000000000),UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000001234)};
    for(size_t a=0;a<sizeof(special)/sizeof(special[0]);a++)
        for(size_t b=0;b<sizeof(special)/sizeof(special[0]);b++) {
            double x=test_phase_from_bits(special[a]),z=test_phase_from_bits(special[b]);
            bool fallback=!gui_rf_deemph_finite(x) || !gui_rf_deemph_finite(z)
                            || ((test_phase_bits(x)<<1)==0 && (test_phase_bits(z)<<1)==0);
            if(fallback) {
                double actual=gui_rf_preview_phase(x,z);
                double expected=test_phase_reference_float((float)x,(float)z);
                if(test_phase_nan(expected)) assert(test_phase_nan(actual));
                else assert(test_phase_bits(actual)==test_phase_bits(expected));
            } else check_ed_phase_finite(x,z,true,&maximum,&maximum_legacy);
        }
    /* Adjacent octants must meet at pi/4 even after coefficient rounding.
     * This catches an otherwise tiny but avoidable minimax endpoint jump. */
    const double adjacent=nextafter(1.0,0.0);
    assert(fabs(gui_rf_preview_phase(1.0,1.0)-TEST_PI*.25)<=1e-14);
    for(int sx=-1;sx<=1;sx+=2) for(int sz=-1;sz<=1;sz+=2) {
        double left=gui_rf_preview_phase(sx*adjacent,sz);
        double right=gui_rf_preview_phase(sx,sz*adjacent);
        assert(fabs(left-right)<=1e-14);
    }
    double previous=gui_rf_preview_phase(0.0,1.0);
    for(int point=1;point<=32768;point++) {
        double phase=gui_rf_preview_phase(point/32768.0,1.0);
        assert(phase>=previous && phase<=TEST_PI*.25+1e-14);
        previous=phase;
    }
    printf("ED phase: dense full-circle/magnitude/octant/axes/extreme/fallback checks; "
           "max double %.9g rad,legacy float %.9g rad\n",maximum,maximum_legacy);
}

static void run_ed_physical_contracts(gui_rf_decoder_t *decoder,size_t capacity) {
    test_frames_t frames={.beta_display=true};frames.rgba=malloc(capacity);assert(frames.rgba);
    /* Full-range white exercises ED's9.3MHz endpoint with both physical head
     * orders and simultaneous strong FM Hi-Fi carriers under tape jitter. */
    for(int swap=0;swap<2;swap++) {
        test_quality_t signal={.pattern=TEST_Q_COLOR_BARS,.fields=6,.swap_tracks=swap,
            .beta_audio_amplitude=.12,.switch_track_line=247};
        size_t length;float *rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_ED_BETA,.14,
            true,1.001,-1,TEST_V_NORMAL,&signal,&length);
        gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_NTSC_ED_BETA);
        frames.frames=0;frames.end_sample=0;
        process_fixture(decoder,rf,length,0,&frames);
        printf("ED Beta head order%d strong Hi-Fi RF%.2f: ",swap,signal.beta_audio_amplitude);
        check_bars(&frames,.10);
        assert(frames.width==640 && frames.height==480);
        free(rf);
    }
    /* Reuse the same three nonzero Y/C edges and unchanged5px/gradient
     * thresholds as SuperBeta,for each original field and tape clock. */
    const double clocks[]={.999,1.0,1.001};
    for(int clock=0;clock<3;clock++) for(int jitter=0;jitter<2;jitter++) {
        test_quality_t signal={.pattern=TEST_Q_MID_BARS,.fields=6,
            .beta_audio_amplitude=.12,.switch_track_line=247,.swap_tracks=(clock+jitter)&1};
        size_t length;float *rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_ED_BETA,jitter*.12,
            true,clocks[clock],-1,TEST_V_NORMAL,&signal,&length);
        gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_NTSC_ED_BETA);
        frames.frames=0;frames.end_sample=0;
        process_fixture(decoder,rf,length,0,&frames);
        assert(frames.frames>=2 && frames.width==640 && frames.height==480);
        assert(frames.status.h_locked && frames.status.v_locked && frames.status.color_locked);
        check_beta_edges(&frames,"ED Beta",clocks[clock],jitter*.12);
        free(rf);
    }
    free(frames.rgba);
}

static void run_ed_beta_contracts(void) {
    run_ed_phase_contract();
    _Static_assert(GUI_RF_PROFILE_PAL_VHS==0 && GUI_RF_PROFILE_NTSC_VHS==1 &&
        GUI_RF_PROFILE_NTSC_BETAMAX==2 && GUI_RF_PROFILE_NTSC_BETAMAX_HIFI==3 &&
        GUI_RF_PROFILE_NTSC_SUPERBETA==4 && GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI==5 &&
        GUI_RF_PROFILE_SECAM_VHS==6 && GUI_RF_PROFILE_MESECAM_VHS==7 &&
        GUI_RF_PROFILE_NTSC_ED_BETA==8,
        "ED appends without changing any existing profile ID");
    for(int profile=0;profile<GUI_RF_PROFILE_COUNT;profile++)
        assert(gui_rf_profile_video_rate_hz((gui_rf_profile_t)profile)==
            20000000u);
    assert(gui_rf_profile_format(GUI_RF_PROFILE_NTSC_ED_BETA)==GUI_RF_NTSC);
    assert(gui_rf_profile_has_hifi(GUI_RF_PROFILE_NTSC_ED_BETA));
    assert(!strcmp(gui_rf_profile_name(GUI_RF_PROFILE_NTSC_ED_BETA),"NTSC ED Beta"));
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(GUI_RF_PROFILE_NTSC_ED_BETA);assert(decoder);
    const size_t capacity=(size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u;
    test_frames_t gray={.beta_display=true};gray.rgba=malloc(capacity);assert(gray.rgba);
    test_quality_t levels={.pattern=TEST_Q_LEVEL_BARS,.fields=6};
    size_t length;float *rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_ED_BETA,0,true,1.0,-1,
        TEST_V_NORMAL,&levels,&length);
    assert(length>(size_t)(TEST_ED_RATE*.099) && length<(size_t)(TEST_ED_RATE*.101));
    process_fixture(decoder,rf,length,0,&gray);
    assert(gray.frames>=2 && gray.width==640 && gray.height==480);
    assert(gray.status.h_locked && gray.status.v_locked);
    const double nominal[]={0.0,.025,.10,.25,.50,.65,.85,1.0};
    double error=0.0;
    for(int parity=0;parity<2;parity++) for(int bar=0;bar<8;bar++) for(int channel=0;channel<3;channel++) {
        quality_rows_t rows=quality_rows(&gray,parity,bar,channel,nominal[bar]);
        error=fmax(error,rows.maximum_error);
        assert(fabs(rows.mean-nominal[bar])<.035 && rows.maximum_error<.06 && rows.row_rms<.015);
    }
    printf("ED Beta nominal 0-100IRE gray: max error %.5f\n",error);
    free(rf);free(gray.rgba);
    test_quality_t color={.pattern=TEST_Q_COLOR_BARS,.fields=6,.swap_tracks=1};
    rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_ED_BETA,0,true,1.0,-1,
        TEST_V_NORMAL,&color,&length);
    const uint64_t origin=UINT64_C(10000000003); /* Odd, nonzero 20MSPS input origin. */
    test_ed_sequence_t reference={.frame={.beta_display=true}},partition={.frame={.beta_display=true}};
    reference.frame.rgba=malloc(capacity);partition.frame.rgba=malloc(capacity);
    assert(reference.frame.rgba && partition.frame.rgba);
    gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_NTSC_ED_BETA);
    process_ed_chunks(decoder,rf,length,origin,false,&reference);
    check_bars(&reference.frame,.10);
    assert(reference.frame.width==640 && reference.frame.height==480);
    assert(reference.frame.first_end_sample>=origin && reference.frame.end_sample<=origin+length+4);
    gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_NTSC_ED_BETA);
    process_ed_chunks(decoder,rf,length,origin,true,&partition);
    check_bars(&partition.frame,.10);
    assert(partition.frame.frames==reference.frame.frames);
    for(int frame=0;frame<reference.frame.frames;frame++) {
        assert(partition.ends[frame]==reference.ends[frame]);
        assert(partition.hashes[frame]==reference.hashes[frame]);
    }
    /* A genuine 20MSPS discontinuity discards partial fields and keeps ED.
     * Recovery timestamps remain on the20MSPS clock, never working-5M. */
    float silence[257]={0};uint64_t gap=origin+length+UINT64_C(4000001);
    partition.frame.frames=0;partition.frame.end_sample=0;
    gui_rf_decoder_process(decoder,silence,257,gap,receive_ed_frame,&partition);
    gui_rf_decode_status_t state;gui_rf_decoder_get_status(decoder,&state);
    assert(!state.h_locked && !state.v_locked && !state.color_locked && state.frames_decoded==0);
    assert(partition.frame.frames==0);
    uint64_t resume=gap+257;
    process_ed_chunks(decoder,rf,length,resume,true,&partition);
    check_bars(&partition.frame,.10);
    assert(partition.frame.first_end_sample>=resume+color.field_start_sample[2]);
    assert(partition.frame.end_sample<=resume+length+4);
    printf("ED Beta 20MSPS partition/gap: %d identical frames, common20M end %llu\n",
        reference.frame.frames,(unsigned long long)partition.frame.end_sample);
    free(rf);free(reference.frame.rgba);free(partition.frame.rgba);
    run_ed_physical_contracts(decoder,capacity);gui_rf_decoder_destroy(decoder);
    /* Opposite head order, original field parity and complete Full526 rows. */
    run_full_frame_format(GUI_RF_PROFILE_NTSC_ED_BETA);
}

static void run_superbeta_acquire_contracts(void) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(GUI_RF_PROFILE_NTSC_SUPERBETA);assert(decoder);
    test_frames_t frames={.beta_display=true};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(frames.rgba);
    for(int profile=GUI_RF_PROFILE_NTSC_SUPERBETA;profile<=GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI;++profile) {
        /* Real FM shift above the initial sync slicer, with a shared tape time
         * axis. Acquisition must come from RF, not injected decoder levels. */
        test_quality_t shifted={.pattern=TEST_Q_GRAY,.fields=10,.carrier_offset_hz=250000.0,
            .beta_audio_amplitude=.12};
        size_t length;float *rf=make_rf_signal_profile((gui_rf_profile_t)profile,0,true,1.0,-1,
            TEST_V_NORMAL,&shifted,&length);
        gui_rf_decoder_reset_profile(decoder,(gui_rf_profile_t)profile);frames.frames=0;frames.end_sample=0;
        test_beta_acquire_observation_t observation={.transition_sample=UINT64_MAX};
        process_beta_acquire_chunks(decoder,rf,length,0,true,&frames,&observation);
        check_beta_acquired(decoder,&frames,"SuperBeta +250kHz cold acquisition");
        assert(length-observation.last_unlocked_sample>=(uint64_t)(TEST_RATE*.035));
        uint64_t offset=length;free(rf);
        test_quality_t h_only={.pattern=TEST_Q_GRAY,.fields=8};
        rf=make_rf_signal_profile((gui_rf_profile_t)profile,0,false,1.0,-1,TEST_V_H_ONLY,&h_only,&length);
        size_t settling=(size_t)h_only.field_start_sample[2];
        process_beta_acquire_chunks(decoder,rf,settling,offset,true,&frames,NULL);int before=frames.frames;
        process_beta_acquire_chunks(decoder,rf+settling,length-settling,offset+settling,true,&frames,NULL);
        gui_rf_decode_status_t missing;gui_rf_decoder_get_status(decoder,&missing);
        assert(frames.frames==before && !missing.v_locked);offset+=length;free(rf);
        rf=make_rf_signal_profile((gui_rf_profile_t)profile,0,true,1.0,-1,TEST_V_NORMAL,&shifted,&length);
        frames.frames=0;size_t two_fields=(size_t)shifted.field_start_sample[2];
        process_beta_acquire_chunks(decoder,rf,two_fields,offset,true,&frames,NULL);assert(frames.frames==0);
        process_beta_acquire_chunks(decoder,rf+two_fields,length-two_fields,offset+two_fields,true,&frames,NULL);
        check_beta_acquired(decoder,&frames,"SuperBeta after stale H-only interval");
        assert(frames.first_end_sample>=offset+two_fields);free(rf);
        for(int noise=0;noise<2;++noise) {
            gui_rf_decoder_reset_profile(decoder,(gui_rf_profile_t)profile);frames.frames=0;frames.end_sample=0;
            beta_invalid_rf(decoder,noise!=0,(size_t)(TEST_RATE*.12),0,&frames);
            gui_rf_decode_status_t status;gui_rf_decoder_get_status(decoder,&status);
            assert(!status.v_locked && !status.color_locked && frames.frames==0);
            if(!noise) assert(!status.h_locked);
        }
    }
    free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

static void check_superbeta_level_bars(const test_frames_t *frames,double offset) {
    const double expected[]={0.0,.025,.10,.25,.50,.65,.85,1.0};
    assert(frames->frames>0 && frames->status.h_locked && frames->status.v_locked);
    double largest_error=0,largest_rms=0;
    for(int parity=0;parity<2;++parity) for(int bar=0;bar<8;++bar) {
        double target=clamp_value(expected[bar]+offset,0,1);
        quality_rows_t rows=quality_rows(frames,parity,bar,1,target);
        largest_error=fmax(largest_error,rows.maximum_error);
        largest_rms=fmax(largest_rms,rows.row_rms);
        assert(fabs(rows.mean-target)<.025 && rows.maximum_error<.04 && rows.row_rms<.012);
    }
    printf("SuperBeta calibrated level bars: offset %.5f, max error %.5f, row RMS %.5f\n",
           offset,largest_error,largest_rms);fflush(stdout);
}

/* Recording FM offset/span are independent of ADC amplitude and tape timing.
 * Gray steps from black through white must retain their physical values even
 * when valid sync locks before the older 50ms acquisition fallback can run.
 * A later small, healthy level change deliberately remains visible: this
 * proves that the accepted display mapping is frozen rather than recomputed
 * every field. A genuine H loss must then permit a new recording calibration. */
static void run_superbeta_display_level_contracts(void) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(GUI_RF_PROFILE_NTSC_SUPERBETA);assert(decoder);
    test_frames_t frames={.beta_display=true};
    size_t capacity=(size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u;
    frames.rgba=malloc(capacity);uint8_t *reference=malloc(capacity);assert(frames.rgba && reference);
    test_quality_t signal={.pattern=TEST_Q_LEVEL_BARS,.fields=16,.beta_audio_amplitude=.12,
        .window_noise_hz=40000,.fm_blank_hz=4730000,.fm_sync_hz=4340000,.fm_hz_per_ire=9750,
        .fm_change_field=6,.fm_blank_after_hz=4755000,.fm_sync_after_hz=4365000,
        .fm_hz_per_ire_after=9750};
    size_t length;float *rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_SUPERBETA,0,true,1.0,-1,
        TEST_V_NORMAL,&signal,&length);
    int reference_frames=0;uint64_t reference_end=0;
    for(int profile=GUI_RF_PROFILE_NTSC_SUPERBETA;profile<=GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI;++profile) {
        gui_rf_decoder_reset_profile(decoder,(gui_rf_profile_t)profile);frames.frames=0;frames.end_sample=0;
        size_t first=(size_t)signal.field_start_sample[4];
        process_fixture(decoder,rf,first,0,&frames);check_superbeta_level_bars(&frames,0);
        gui_rf_decode_status_t before,after;gui_rf_decoder_get_status(decoder,&before);
        process_fixture(decoder,rf+first,length-first,first,&frames);
        check_superbeta_level_bars(&frames,25000.0/975000.0);
        gui_rf_decoder_get_status(decoder,&after);
        assert(after.fields_rejected==before.fields_rejected);
        size_t pixels=(size_t)frames.width*frames.height*4u;
        if(profile==GUI_RF_PROFILE_NTSC_SUPERBETA) {
            memcpy(reference,frames.rgba,pixels);reference_frames=frames.frames;reference_end=frames.end_sample;
        } else {
            assert(frames.frames==reference_frames && frames.end_sample==reference_end);
            assert(memcmp(reference,frames.rgba,pixels)==0);
        }
        uint64_t offset=length;size_t outage=(size_t)(TEST_RATE*.06);
        beta_invalid_rf(decoder,false,outage,offset,&frames);offset+=outage;
        gui_rf_decoder_get_status(decoder,&after);assert(!after.h_locked && !after.v_locked);
        test_quality_t restored={.pattern=TEST_Q_LEVEL_BARS,.fields=8,.beta_audio_amplitude=.12,
            .window_noise_hz=40000,.fm_blank_hz=4850000,.fm_sync_hz=4490000,.fm_hz_per_ire=9000};
        size_t restored_length;float *restored_rf=make_rf_signal_profile((gui_rf_profile_t)profile,0,true,
            1.0,-1,TEST_V_NORMAL,&restored,&restored_length);
        frames.frames=0;process_fixture(decoder,restored_rf,restored_length,offset,&frames);
        check_superbeta_level_bars(&frames,0);free(restored_rf);
    }
    free(rf);
    /* The display calibration is stored in the same speed-corrected units as
     * rendered luma. Exercise actual RF/line-clock changes with non-nominal
     * recording endpoints, independently of the edge registration matrix. */
    const double clocks[]={.999,1.001};
    signal.fields=8;signal.fm_change_field=0;signal.window_noise_hz=0;
    for(int clock=0;clock<2;++clock) {
        rf=make_rf_signal_profile(GUI_RF_PROFILE_NTSC_SUPERBETA,.12,true,clocks[clock],-1,
            TEST_V_NORMAL,&signal,&length);
        gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_NTSC_SUPERBETA);
        frames.frames=0;frames.end_sample=0;process_fixture(decoder,rf,length,0,&frames);
        printf("SuperBeta shifted recording clock %.3f: ",clocks[clock]);
        check_superbeta_level_bars(&frames,0);free(rf);
    }
    free(reference);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

/* Compare stable bar interiors, retaining every original woven row. A DC
 * defect can average away in a bar mean while leaving large false-color
 * ripple, so this checks actual RGB and reconstructed Y/U/V pixel errors. */
static void check_svhs_dc_pixels(const test_frames_t *reference,const test_frames_t *actual) {
    assert(reference->width==actual->width && reference->height==actual->height);
    assert(reference->frames==actual->frames && reference->end_sample==actual->end_sample);
    double rgb_square=0.0,yuv_square[3]={0},maximum_rgb=0.0;
    size_t samples=0;
    for(int y=80;y<actual->height-80;y++) for(int bar=0;bar<8;bar++) {
        int centre=(int)((bar+.5)*actual->width/8.0);
        for(int x=centre-8;x<=centre+8;x++) {
            size_t at=((size_t)y*actual->width+x)*4u;
            double delta[3];
            for(int channel=0;channel<3;channel++) {
                delta[channel]=((double)actual->rgba[at+channel]-reference->rgba[at+channel])/255.0;
                rgb_square+=delta[channel]*delta[channel];
                maximum_rgb=fmax(maximum_rgb,fabs(delta[channel]));
            }
            double dy=.299*delta[0]+.587*delta[1]+.114*delta[2];
            double du=(delta[2]-dy)/2.032,dv=(delta[0]-dy)/1.140;
            yuv_square[0]+=dy*dy;yuv_square[1]+=du*du;yuv_square[2]+=dv*dv;
            samples++;
        }
    }
    assert(samples>10000);
    double rms=sqrt(rgb_square/(3.0*samples));
    printf("S-VHS ADC offset stable pixels: RGB RMS %.6f max %.6f, YUV RMS %.6f/%.6f/%.6f\n",
        rms,maximum_rgb,sqrt(yuv_square[0]/samples),sqrt(yuv_square[1]/samples),sqrt(yuv_square[2]/samples));
    assert(rms<=.006 && maximum_rgb<=.03);
    for(int channel=0;channel<3;channel++) assert(sqrt(yuv_square[channel]/samples)<=.005);
}

/* Add ADC bias to physical FM+color-under samples, never decoded pixels.
 * The +/-0.8 offsets are comparable with the 0.72 peak luma RF and much
 * larger than the recorded burst. This independently exposes DC translated
 * into the color band, its AGC amplification, and state lost at block splits. */
static void run_svhs_dc_contracts(void) {
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_PAL_SVHS,GUI_RF_PROFILE_NTSC_SVHS};
    const size_t capacity=(size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u;
    test_ed_sequence_t reference={0},biased={0},partition={0};
    reference.frame.rgba=malloc(capacity);biased.frame.rgba=malloc(capacity);
    partition.frame.rgba=malloc(capacity);
    assert(reference.frame.rgba && biased.frame.rgba && partition.frame.rgba);
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(profiles[0]);assert(decoder);
    for(size_t system=0;system<sizeof(profiles)/sizeof(profiles[0]);system++) {
        for(int gray=0;gray<2;gray++) {
            test_quality_t quality={.pattern=gray ? TEST_Q_GRAY : TEST_Q_COLOR_BARS,
                .fields=8,.swap_tracks=1};
            size_t length;
            float *rf=make_rf_signal_profile(profiles[system],.08,true,1.001,-1,
                TEST_V_NORMAL,&quality,&length);
            float *offset_rf=malloc(length*sizeof(*offset_rf));assert(offset_rf);
            const uint64_t origin=UINT64_C(10000000003);
            gui_rf_decoder_reset_profile(decoder,profiles[system]);
            reference.frame=(test_frames_t){.rgba=reference.frame.rgba};
            process_ed_chunks(decoder,rf,length,origin,false,&reference);
            if(gray) check_quality_gray(&reference.frame,"S-VHS ADC bias reference",0,.035,.015,.06);
            else check_bars(&reference.frame,.10);
            assert(reference.frame.status.color_locked);
            /* The step occurs inside an ordinary line of the fourth field,
             * not at a producer block or field boundary. Publication waits
             * for the following V sync, so eight input fields are needed
             * to compare a later complete frame beyond the causal transient. */
            size_t step=(size_t)quality.field_start_sample[3]+123457u;
            assert(step<quality.field_start_sample[4] && step<length);
            assert(reference.frame.end_sample-origin>=quality.field_start_sample[6]);
            printf("S-VHS ADC recovery window: step at %llu, last frame end %llu, stable boundary %llu\n",
                (unsigned long long)step,(unsigned long long)(reference.frame.end_sample-origin),
                (unsigned long long)quality.field_start_sample[6]);
            for(int bias=0;bias<3;bias++) {
                for(size_t n=0;n<length;n++) {
                    double dc=bias==0 ? .8 : bias==1 ? -.8 : n<step ? -.8 : .8;
                    offset_rf[n]=(float)(rf[n]+dc);
                }
                gui_rf_decoder_reset_profile(decoder,profiles[system]);
                gui_rf_decode_status_t initial;gui_rf_decoder_get_status(decoder,&initial);
                assert(!initial.h_locked && !initial.v_locked && !initial.color_locked && initial.frames_decoded==0);
                biased.frame=(test_frames_t){.rgba=biased.frame.rgba};
                process_ed_chunks(decoder,offset_rf,length,origin,bias!=0,&biased);
                printf("%s %s ADC bias %s: ",gui_rf_profile_name(profiles[system]),
                    gray ? "gray" : "bars",bias==0 ? "+0.8" : bias==1 ? "-0.8" : "-0.8 to +0.8 step");
                if(gray) {
                    check_quality_gray(&biased.frame,"S-VHS biased gray",0,.035,.015,.06);
                    assert(biased.frame.status.color_locked);
                    for(int parity=0;parity<2;parity++) {
                        double red=plane_mean(&biased.frame,4,parity,0);
                        assert(fabs(red-plane_mean(&biased.frame,4,parity,1))<.01);
                        assert(fabs(red-plane_mean(&biased.frame,4,parity,2))<.01);
                    }
                } else check_bars(&biased.frame,.10);
                check_svhs_dc_pixels(&reference.frame,&biased.frame);
                if(!gray && bias==2) {
                    gui_rf_decoder_reset_profile(decoder,profiles[system]);
                    partition.frame=(test_frames_t){.rgba=partition.frame.rgba};
                    process_ed_chunks(decoder,offset_rf,length,origin,false,&partition);
                    assert(partition.frame.frames==biased.frame.frames);
                    for(int frame=0;frame<biased.frame.frames;frame++) {
                        assert(partition.ends[frame]==biased.ends[frame]);
                        assert(partition.hashes[frame]==biased.hashes[frame]);
                    }
                }
            }
            /* A fresh reset after biased input must also recover the clean
             * waveform without inheriting the previous ADC offset. */
            gui_rf_decoder_reset_profile(decoder,profiles[system]);
            partition.frame=(test_frames_t){.rgba=partition.frame.rgba};
            process_ed_chunks(decoder,rf,length,origin,true,&partition);
            assert(partition.frame.frames==reference.frame.frames);
            for(int frame=0;frame<reference.frame.frames;frame++) {
                assert(partition.ends[frame]==reference.ends[frame]);
                assert(partition.hashes[frame]==reference.hashes[frame]);
            }
            free(offset_rf);free(rf);
        }
    }
    gui_rf_decoder_destroy(decoder);free(reference.frame.rgba);free(biased.frame.rgba);free(partition.frame.rgba);
}

/* The same physical oracle changes only the tape's FM modulation. The
 * independent VHS color-under recording law must still recover both head
 * orders and weave adjacent fields, with identical output across partitions. */
static void run_svhs_contracts(void) {
    _Static_assert(GUI_RF_PROFILE_NTSC_ED_BETA==8 && GUI_RF_PROFILE_PAL_SVHS==9 &&
        GUI_RF_PROFILE_NTSC_SVHS==10 && GUI_RF_PROFILE_COUNT==11,
        "S-VHS appends without changing existing persisted profile IDs");
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_PAL_SVHS,GUI_RF_PROFILE_NTSC_SVHS};
    const size_t capacity=(size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u;
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(profiles[0]);assert(decoder);
    test_ed_sequence_t reference={0},partition={0};
    reference.frame.rgba=malloc(capacity);partition.frame.rgba=malloc(capacity);
    assert(reference.frame.rgba && partition.frame.rgba);
    for(size_t system=0;system<sizeof(profiles)/sizeof(profiles[0]);system++) {
        gui_rf_profile_t profile=profiles[system];
        bool pal=profile==GUI_RF_PROFILE_PAL_SVHS;
        const char *name=pal ? "PAL S-VHS" : "NTSC S-VHS";
        assert(gui_rf_profile_format(profile)==(pal ? GUI_RF_PAL : GUI_RF_NTSC));
        assert(gui_rf_profile_video_rate_hz(profile)==20000000u);
        assert(!gui_rf_profile_has_hifi(profile));
        assert(strcmp(gui_rf_profile_name(profile),name)==0);
        test_quality_t color={.pattern=TEST_Q_COLOR_BARS,.fields=6,.swap_tracks=1};
        size_t length;float *rf=make_rf_signal_profile(profile,.08,true,1.001,-1,
            TEST_V_NORMAL,&color,&length);
        const uint64_t origin=UINT64_C(10000000003);
        gui_rf_decoder_reset_profile(decoder,profile);
        reference.frame.frames=0;reference.frame.end_sample=0;
        process_ed_chunks(decoder,rf,length,origin,false,&reference);
        printf("%s physical high-carrier RF: ",name);check_bars(&reference.frame,.10);
        assert(reference.frame.width==(pal ? 768 : 640) && reference.frame.height==(pal ? 576 : 480));
        assert(reference.frame.first_end_sample>=origin && reference.frame.end_sample<=origin+length+4);
        gui_rf_decoder_reset_profile(decoder,profile);
        partition.frame.frames=0;partition.frame.end_sample=0;
        process_ed_chunks(decoder,rf,length,origin,true,&partition);
        assert(partition.frame.frames==reference.frame.frames);
        for(int frame=0;frame<reference.frame.frames;frame++) {
            assert(partition.ends[frame]==reference.ends[frame]);
            assert(partition.hashes[frame]==reference.hashes[frame]);
        }
        /* A real sample-clock jump must discard partially assembled fields
         * while retaining S-VHS modulation for the recovered source. */
        float silence[257]={0};uint64_t gap=origin+length+UINT64_C(4000001);
        partition.frame.frames=0;partition.frame.end_sample=0;
        gui_rf_decoder_process(decoder,silence,257,gap,receive_ed_frame,&partition);
        gui_rf_decode_status_t state;gui_rf_decoder_get_status(decoder,&state);
        assert(!state.h_locked && !state.v_locked && !state.color_locked && state.frames_decoded==0);
        assert(partition.frame.frames==0);
        uint64_t resume=gap+257;
        process_ed_chunks(decoder,rf,length,resume,true,&partition);
        check_bars(&partition.frame,.10);
        assert(partition.frame.first_end_sample>=resume+color.field_start_sample[2]);
        assert(partition.frame.end_sample<=resume+length+4);
        printf("%s partition/gap: %d identical frames before gap, 20MSPS end %llu\n",
            name,reference.frame.frames,(unsigned long long)partition.frame.end_sample);
        free(rf);
        color.swap_tracks=0;
        rf=make_rf_signal_profile(profile,0,true,1.0,-1,TEST_V_NORMAL,&color,&length);
        gui_rf_decoder_reset_profile(decoder,profile);
        partition.frame.frames=0;partition.frame.end_sample=0;
        process_fixture(decoder,rf,length,0,&partition.frame);
        printf("%s opposite head order: ",name);check_bars(&partition.frame,.10);free(rf);

        /* Physical gray levels expose a wrong 5.4/7.0MHz calibration even
         * when timing alone locks. No burst must also remove color cleanly. */
        test_quality_t gray={.pattern=TEST_Q_GRAY,.fields=8,.window_noise_hz=60000.0};
        rf=make_rf_signal_profile(profile,0,false,1.0,-1,TEST_V_NORMAL,&gray,&length);
        gui_rf_decoder_reset_profile(decoder,profile);
        partition.frame.frames=0;partition.frame.end_sample=0;
        process_fixture(decoder,rf,length,0,&partition.frame);
        check_quality_gray(&partition.frame,name,0,.035,.015,.06);
        assert(!partition.frame.status.color_locked);
        for(int parity=0;parity<2;parity++) {
            double red=plane_mean(&partition.frame,4,parity,0);
            assert(fabs(red-plane_mean(&partition.frame,4,parity,1))<.01);
            assert(fabs(red-plane_mean(&partition.frame,4,parity,2))<.01);
        }
        free(rf);
        /* Switching back must restore VHS RF calibration, not carry the
         * preceding S-VHS filter coefficients or partial frame state. */
        gui_rf_profile_t vhs=pal ? GUI_RF_PROFILE_PAL_VHS : GUI_RF_PROFILE_NTSC_VHS;
        gray.window_noise_hz=0.0;
        rf=make_rf_signal_profile(vhs,0,true,1.0,-1,TEST_V_NORMAL,&gray,&length);
        gui_rf_decoder_reset_profile(decoder,vhs);
        gui_rf_decoder_get_status(decoder,&state);
        assert(!state.h_locked && !state.v_locked && !state.color_locked && state.frames_decoded==0);
        partition.frame.frames=0;partition.frame.end_sample=0;
        process_fixture(decoder,rf,length,0,&partition.frame);
        check_quality_gray(&partition.frame,pal ? "PAL VHS after S-VHS" : "NTSC VHS after S-VHS",
            0,.035,.015,.06);
        assert(partition.frame.status.color_locked);free(rf);
    }
    free(reference.frame.rgba);free(partition.frame.rgba);gui_rf_decoder_destroy(decoder);
    run_svhs_dc_contracts();
}

/* Physical equal-Y color oracle. RGB is intentionally kept away from both
 * display clipping rails, so chroma errors cannot hide in clipped channels. */
static void svhs_expected_color(int parity,bool second,double rgb[3],double yuv[3]) {
    double u,v;quality_uv_target(parity,second,&u,&v);
    rgb[0]=display_level(.5)+1.140*v;
    rgb[1]=display_level(.5)-.395*u-.581*v;
    rgb[2]=display_level(.5)+2.032*u;
    for(int c=0;c<3;c++) assert(rgb[c]>.05 && rgb[c]<.95);
    yuv[0]=.299*rgb[0]+.587*rgb[1]+.114*rgb[2];
    yuv[1]=(rgb[2]-yuv[0])/2.032;yuv[2]=(rgb[0]-yuv[0])/1.140;
}

static void svhs_pixel_yuv(const test_frames_t *frame,int x,int y,double out[3]) {
    const uint8_t *rgb=frame->rgba+((size_t)y*frame->width+x)*4u;
    out[0]=(.299*rgb[0]+.587*rgb[1]+.114*rgb[2])/255.0;
    out[1]=(rgb[2]/255.0-out[0])/2.032;
    out[2]=(rgb[0]/255.0-out[0])/1.140;
}

static double svhs_crossing(const double *trace,int length,double target,int low,int high) {
    if(low<0) low=0;
    if(high>=length-1) high=length-2;
    for(int x=low;x<=high;x++) if(trace[x]<=target && trace[x+1]>target)
        return x+(target-trace[x])/(trace[x+1]-trace[x]);
    return -1.0;
}

static double svhs_color_metrics(const test_frames_t *frame,bool horizontal,const char *label,int recovery_line) {
    assert(frame->frames>=3 && frame->status.h_locked && frame->status.v_locked && frame->status.color_locked);
    int field_height=frame->height/2;
    double row_square[2][2][2]={{{0}}},row_sum[2][2][2]={{{0}}},rgb_square=0.0;
    size_t row_counts[2][2]={{0}},rows=0,pixels=0;
    for(int parity=0;parity<2;parity++) {
        double sum[2][3]={{0}},expected_sum[2][3]={{0}};
        size_t counts[2]={0};
        for(int fy=40;fy<field_height-40;fy++) {
            double boundary=quality_uv_boundary(parity)*field_height;
            if(horizontal && fabs(fy-boundary)<8.0) continue;
            if(recovery_line>=0 && abs(fy-recovery_line)<=4) continue;
            double row_error[2][2]={{0}};size_t row_pixels[2]={0};
            for(int region=0;region<2;region++) {
                int centre=(int)((region ? .75 : .25)*frame->width);
                bool second=horizontal ? fy>=(int)boundary : region!=0;
                double expected_rgb[3],expected_yuv[3];
                svhs_expected_color(parity,second,expected_rgb,expected_yuv);
                for(int x=centre-8;x<=centre+8;x++) {
                    int y=fy*2+parity;double actual[3];svhs_pixel_yuv(frame,x,y,actual);
                    int slot=second ? 1 : 0;
                    for(int c=0;c<3;c++) { sum[slot][c]+=actual[c];expected_sum[slot][c]+=expected_yuv[c]; }
                    counts[slot]++;
                    const uint8_t *rgb=frame->rgba+((size_t)y*frame->width+x)*4u;
                    for(int c=0;c<3;c++) {
                        double value=rgb[c]/255.0,error=value-expected_rgb[c];
                        assert(value>.015 && value<.985);
                        rgb_square+=error*error;
                    }
                    for(int c=0;c<2;c++) row_error[slot][c]+=actual[c+1]-expected_yuv[c+1];
                    row_pixels[slot]++;pixels++;
                }
            }
            assert(row_pixels[0]+row_pixels[1]==34);
            for(int region=0;region<2;region++) if(row_pixels[region]) {
                for(int c=0;c<2;c++) {
                    double error=row_error[region][c]/row_pixels[region];
                    row_sum[parity][region][c]+=error;
                    row_square[parity][region][c]+=error*error;
                }
                row_counts[parity][region]++;
            }
            rows++;
        }
        for(int region=0;region<2;region++) {
            assert(counts[region]>1000);
            for(int c=0;c<3;c++) {
                double error=fabs((sum[region][c]-expected_sum[region][c])/counts[region]);
                if(error>(c==0 ? .025 : .045))
                    fprintf(stderr,"%s field%d region%d %c bias %.6f\n",label,parity,region,"YUV"[c],error);
                assert(error<=(c==0 ? .025 : .045));
            }
        }
        /* Project onto the prescribed UV transition, never a decoder-derived
         * reference trace. Horizontal traces retain each original field. */
        double left_rgb[3],left[3],right_rgb[3],right[3];
        svhs_expected_color(parity,false,left_rgb,left);svhs_expected_color(parity,true,right_rgb,right);
        double du=right[1]-left[1],dv=right[2]-left[2],norm=du*du+dv*dv;
        int count=horizontal ? field_height : frame->width;
        double *trace=calloc((size_t)count,sizeof(*trace));assert(trace);
        for(int at=0;at<count;at++) {
            double uv[2]={0};int samples=0;
            if(horizontal) {
                for(int x=frame->width/4;x<frame->width*3/4;x++) {
                    double pixel[3];svhs_pixel_yuv(frame,x,at*2+parity,pixel);
                    uv[0]+=pixel[1];uv[1]+=pixel[2];samples++;
                }
            } else {
                for(int fy=40;fy<field_height-40;fy++) {
                    if(recovery_line>=0 && abs(fy-recovery_line)<=4) continue;
                    double pixel[3];svhs_pixel_yuv(frame,at,fy*2+parity,pixel);
                    uv[0]+=pixel[1];uv[1]+=pixel[2];samples++;
                }
            }
            trace[at]=((uv[0]/samples-left[1])*du+(uv[1]/samples-left[2])*dv)/norm;
        }
        double expected=quality_uv_boundary(parity)*count;
        int radius=horizontal ? 12 : 64;
        double x10=svhs_crossing(trace,count,.10,(int)expected-radius,(int)expected+radius);
        double x50=svhs_crossing(trace,count,.50,(int)expected-radius,(int)expected+radius);
        double x90=svhs_crossing(trace,count,.90,(int)expected-radius,(int)expected+radius);
        printf("%s field%d %s edge: expected %.3f centre %.3f width10-90 %.3f\n",
            label,parity,horizontal ? "horizontal/field-lines" : "vertical/pixels",expected,x50,x90-x10);
        fflush(stdout);
        assert(x10>=0 && x50>=0 && x90>=x10);
        assert(fabs(x50-expected)<=(horizontal ? 3.0 : 32.0));
        assert(x90-x10<=(horizontal ? 4.0 : 64.0));
        free(trace);
    }
    assert(rows>=280 && pixels==rows*34);
    double variance=0.0;size_t variance_samples=0;
    for(int parity=0;parity<2;parity++) for(int region=0;region<2;region++) {
        size_t count=row_counts[parity][region];assert(count>50);
        for(int c=0;c<2;c++) {
            double sum=row_sum[parity][region][c];
            variance+=fmax(0.0,row_square[parity][region][c]-sum*sum/count);
            variance_samples+=count;
        }
    }
    double stripe=sqrt(variance/variance_samples),rgb_rms=sqrt(rgb_square/(3.0*pixels));
    printf("%s physical RGB RMS %.6f; row UV error std %.6f (%zu stable rows)\n",label,rgb_rms,stripe,rows);
    fflush(stdout);
    assert(rgb_rms<.12);
    /* Levels, distinct original fields and edge gates prevent meeting the
     * stripe limit by desaturating, clipping or smoothing real color edges. */
    return stripe;
}

/* Picture UV is prescribed and unchanged by the transport phase jump.
 * Probe just outside the explicit recovery window as well as far plateaus;
 * averaging a whole frame must not hide local hue drag near a boundary. */
static double svhs_phase_recovery_hue(const test_frames_t *frame,int recovery_line,const char *label) {
    const int offsets[]={-12,-8,-5,5,8,12,-40,40};
    double maximum=0.0,plateau=0.0;
    for(int parity=0;parity<2;parity++) for(int region=0;region<2;region++) {
        double expected_rgb[3],expected[3];svhs_expected_color(parity,region!=0,expected_rgb,expected);
        int centre=(int)((region ? .75 : .25)*frame->width);
        for(size_t n=0;n<sizeof(offsets)/sizeof(offsets[0]);n++) {
            int fy=recovery_line+offsets[n];assert(fy>=0 && fy<frame->height/2);
            double u=0,v=0;
            for(int x=centre-8;x<=centre+8;x++) {
                double pixel[3];svhs_pixel_yuv(frame,x,fy*2+parity,pixel);u+=pixel[1];v+=pixel[2];
            }
            double degrees=fabs(atan2(v*expected[1]-u*expected[2],u*expected[1]+v*expected[2]))*180.0/TEST_PI;
            if(abs(offsets[n])<=12) maximum=fmax(maximum,degrees);
            else plateau=fmax(plateau,degrees);
        }
    }
    printf("%s known-UV hue error: recovery +/-5/8/12 lines max %.3fdeg; far plateau max %.3fdeg\n",label,maximum,plateau);
    fflush(stdout);return maximum;
}

static void check_svhs_no_burst(const test_frames_t *frame,const char *label) {
    assert(frame->frames>=3 && frame->status.h_locked && frame->status.v_locked && !frame->status.color_locked);
    for(int parity=0;parity<2;parity++) {
        double red=plane_mean(frame,2,parity,0),green=plane_mean(frame,2,parity,1),
            blue=plane_mean(frame,2,parity,2);
        assert(fabs(red-green)<.01 && fabs(red-blue)<.01 && fabs(red-display_level(.5))<.035);
    }
    printf("%s: active color RF without burst stays locked gray (%d original woven frames)\n",label,frame->frames);
}

/* Few still-valid bursts are weak and phase-corrupt while picture chroma
 * is unchanged. Their hue is unknowable from that one bad reference, but
 * rejecting every healthy neighbor must not create huge chroma AGC gain. */
static void run_svhs_burst_outlier_contracts(void) {
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_PAL_SVHS,GUI_RF_PROFILE_NTSC_SVHS};
    const int bad_lines[]={101,151,201};
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(profiles[0]);assert(decoder);
    test_ed_sequence_t sequence={0};
    sequence.frame.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(sequence.frame.rgba);
    for(size_t system=0;system<sizeof(profiles)/sizeof(profiles[0]);system++) {
        bool pal=profiles[system]==GUI_RF_PROFILE_PAL_SVHS;
        test_quality_t quality={.pattern=TEST_Q_UV_VERTICAL,.fields=8,.swap_tracks=1,
            .sparse_bad_bursts=true,.bad_burst_gain=.14,.bad_burst_phase=TEST_PI/3.0};
        size_t length;float *rf=make_rf_signal_profile(profiles[system],0,true,1.0,-1,
            TEST_V_NORMAL,&quality,&length);
        gui_rf_decoder_reset_profile(decoder,profiles[system]);
        sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
        const uint64_t origin=UINT64_C(13000000003);
        process_ed_chunks(decoder,rf,length,origin,true,&sequence);
        assert(sequence.frame.frames>=3 && sequence.frame.end_sample-origin>=quality.field_start_sample[6]);
        assert(sequence.frame.status.h_locked && sequence.frame.status.v_locked && sequence.frame.status.color_locked);
        double maximum_ratio=0.0;size_t clipped=0,pixels=0;
        for(int parity=0;parity<2;parity++) for(int region=0;region<2;region++) {
            double expected_rgb[3],expected[3];svhs_expected_color(parity,region!=0,expected_rgb,expected);
            double target_amplitude=hypot(expected[1],expected[2]);assert(target_amplitude>.1);
            int centre=(int)((region ? .75 : .25)*sequence.frame.width);
            for(size_t line=0;line<sizeof(bad_lines)/sizeof(bad_lines[0]);line++) for(int dy=-3;dy<=3;dy++) {
                int fy=bad_lines[line]-(pal ? 22 : 20)+dy;
                assert(fy>=0 && fy<sequence.frame.height/2);
                for(int x=centre-8;x<=centre+8;x++) {
                    int y=fy*2+parity;double actual[3];svhs_pixel_yuv(&sequence.frame,x,y,actual);
                    maximum_ratio=fmax(maximum_ratio,hypot(actual[1],actual[2])/target_amplitude);
                    const uint8_t *rgb=sequence.frame.rgba+((size_t)y*sequence.frame.width+x)*4u;
                    for(int channel=0;channel<3;channel++) if(rgb[channel]<=2 || rgb[channel]>=253) clipped++;
                    pixels++;
                }
            }
        }
        printf("%s sparse bad bursts: source lines101/151/201, gain14%% phase+60deg, activeUV unchanged; peakUV/known target %.4fx, clipped channels %zu/%zu pixels\n",
            gui_rf_profile_name(profiles[system]),maximum_ratio,clipped,pixels);
        fflush(stdout);
        assert(pixels>1000 && clipped==0 && maximum_ratio<=1.8);
        free(rf);
    }
    free(sequence.frame.rgba);gui_rf_decoder_destroy(decoder);
}

/* Strong but isolated bad burst references contradict the physical PAL
 * swinging sequence. Picture UV never changes, so both the source row and
 * its paired neighbors must retain the prescribed direction and amplitude.
 * Source lines of both parities and both physical head orders are exercised. */
static void run_svhs_pal_cadence_contracts(void) {
    const gui_rf_profile_t profile=GUI_RF_PROFILE_PAL_SVHS;
    const int bad_lines[]={100,151,202};
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(profile);assert(decoder);
    test_ed_sequence_t sequence={0};
    sequence.frame.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(sequence.frame.rgba);
    for(int heads=0;heads<2;heads++) {
        test_quality_t quality={.pattern=TEST_Q_UV_VERTICAL,.fields=8,.swap_tracks=heads,
            .sparse_bad_bursts=true,.bad_burst_first_line=100,.bad_burst_line_step=51,
            .bad_burst_gain=1.0,.bad_burst_phase=TEST_PI};
        size_t length;float *rf=make_rf_signal_profile(profile,0,true,1.0,-1,
            TEST_V_NORMAL,&quality,&length);
        gui_rf_decoder_reset_profile(decoder,profile);
        sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
        const uint64_t origin=UINT64_C(14000000003);
        process_ed_chunks(decoder,rf,length,origin,true,&sequence);
        assert(sequence.frame.frames>=3 && sequence.frame.end_sample-origin>=quality.field_start_sample[6]);
        assert(sequence.frame.status.h_locked && sequence.frame.status.v_locked && sequence.frame.status.color_locked);
        double maximum_hue=0.0,minimum_ratio=10.0,maximum_ratio=0.0;size_t rows=0;
        for(int parity=0;parity<2;parity++) for(int region=0;region<2;region++) {
            double expected_rgb[3],expected[3];svhs_expected_color(parity,region!=0,expected_rgb,expected);
            double amplitude=hypot(expected[1],expected[2]);assert(amplitude>.1);
            int centre=(int)((region ? .75 : .25)*sequence.frame.width);
            for(size_t line=0;line<sizeof(bad_lines)/sizeof(bad_lines[0]);line++) for(int dy=-3;dy<=3;dy++) {
                int fy=bad_lines[line]-22+dy;assert(fy>=0 && fy<sequence.frame.height/2);
                double u=0.0,v=0.0;
                for(int x=centre-8;x<=centre+8;x++) {
                    double actual[3];svhs_pixel_yuv(&sequence.frame,x,fy*2+parity,actual);
                    u+=actual[1];v+=actual[2];
                }
                u/=17.0;v/=17.0;
                double hue=fabs(atan2(v*expected[1]-u*expected[2],u*expected[1]+v*expected[2]))*180.0/TEST_PI;
                double ratio=hypot(u,v)/amplitude;
                maximum_hue=fmax(maximum_hue,hue);
                minimum_ratio=fmin(minimum_ratio,ratio);maximum_ratio=fmax(maximum_ratio,ratio);rows++;
            }
        }
        printf("PAL S-VHS head order%d strong contradictory bursts: source lines100/151/202, gain100%% phase+180deg, activeUV unchanged; source/paired +/-3 rows hue max %.3fdeg, known-UV amplitude %.4f..%.4fx (%zu row/region probes)\n",
            heads,maximum_hue,minimum_ratio,maximum_ratio,rows);
        fflush(stdout);
        assert(rows==84 && maximum_hue<=5.0 && minimum_ratio>=.65 && maximum_ratio<=1.35);
        free(rf);
    }
    free(sequence.frame.rgba);gui_rf_decoder_destroy(decoder);
}

/* Project onto the independently prescribed (dU,dV)=(1,.7) direction.
 * Averages remain within each original field; equal-Y RGB stays in gamut. */
static double svhs_detail_trace(const test_frames_t *frame,int parity,int column,int fy,double *y_error) {
    int centre=(int)((column+.5)*frame->width/5.0);
    double u=0.0,v=0.0;
    for(int x=centre-7;x<=centre+7;x++) {
        double actual[3];svhs_pixel_yuv(frame,x,fy*2+parity,actual);
        u+=actual[1];v+=actual[2];
        *y_error=fmax(*y_error,fabs(actual[0]-display_level(.5)));
        const uint8_t *rgb=frame->rgba+((size_t)(fy*2+parity)*frame->width+x)*4u;
        for(int c=0;c<3;c++) assert(rgb[c]>5 && rgb[c]<250);
    }
    u=u/15.0-(parity ? -.04 : .05);v=v/15.0-(parity ? .05 : .04);
    return (u+.7*v)/1.49;
}

static double svhs_detail_average(const double *trace,int first,int last) {
    double sum=0.0;for(int fy=first;fy<last;fy++) sum+=trace[fy];
    return sum/(last-first);
}

static void run_svhs_detail_contracts(void) {
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_PAL_SVHS,GUI_RF_PROFILE_NTSC_SVHS};
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(profiles[0]);assert(decoder);
    test_ed_sequence_t sequence={0};
    sequence.frame.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(sequence.frame.rgba);
    for(size_t system=0;system<sizeof(profiles)/sizeof(profiles[0]);system++) for(int heads=0;heads<2;heads++)
      for(int detail=0;detail<(heads==0 ? 3 : 1);detail++) {
        double amplitude=detail ? .008 : .025,phase=detail==2 ? TEST_PI*.5 : 0.0;
        test_quality_t quality={.pattern=TEST_Q_UV_DETAIL,.fields=8,.swap_tracks=heads,
            .detail_amplitude=amplitude,.detail_phase=phase};
        size_t length;float *rf=make_rf_signal_profile(profiles[system],0,true,1.0,-1,
            TEST_V_NORMAL,&quality,&length);
        gui_rf_decoder_reset_profile(decoder,profiles[system]);
        sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
        const uint64_t origin=UINT64_C(15000000003);
        process_ed_chunks(decoder,rf,length,origin,true,&sequence);
        assert(sequence.frame.frames>=3 && sequence.frame.end_sample-origin>=quality.field_start_sample[6]);
        assert(sequence.frame.status.h_locked && sequence.frame.status.v_locked && sequence.frame.status.color_locked);
        for(int parity=0;parity<2;parity++) {
            double trace[5][GUI_RF_FRAME_MAX_HEIGHT/2]={{0}},y_error=0.0;
            for(int column=0;column<5;column++) for(int fy=32;fy<208;fy++)
                trace[column][fy]=svhs_detail_trace(&sequence.frame,parity,column,fy,&y_error);
            double contrast[2],width[2];
            for(int edge=0;edge<2;edge++) {
                double low=svhs_detail_average(trace[edge],56,64);
                double high=svhs_detail_average(trace[edge],96,104);
                contrast[edge]=(high-low)/(edge==0 ? .16 : .025);
                double normalized[GUI_RF_FRAME_MAX_HEIGHT/2]={0};
                assert(high>low);
                for(int fy=32;fy<208;fy++) normalized[fy]=(trace[edge][fy]-low)/(high-low);
                double x10=svhs_crossing(normalized,208,.10,70,90);
                double x90=svhs_crossing(normalized,208,.90,70,90);
                width[edge]=x90-x10;
                assert(x10>=0 && x90>=x10);
            }
            double modulation[2];
            for(int wave=0;wave<2;wave++) {
                double sine=0.0,cosine=0.0,period=wave==0 ? 16.0 : 8.0;
                for(int fy=40;fy<200;fy++) {
                    double phase=2.0*TEST_PI*fy/period;
                    sine+=trace[wave+2][fy]*sin(phase);cosine+=trace[wave+2][fy]*cos(phase);
                }
                modulation[wave]=2.0*hypot(sine,cosine)/(160.0*amplitude);
            }
            double glyph=10.0;
            for(int cap=0;cap<3;cap++) {
                int start=64+16*cap;
                double stroke=svhs_detail_average(trace[4],start,start+4);
                double background=svhs_detail_average(trace[4],start+8,start+12);
                glyph=fmin(glyph,(stroke-background)/.025);
            }
            printf("%s head%d field%d equal-Y detail amp%.3f phase%.0fdeg: strong/weak contrast %.4f/%.4fx widths %.3f/%.3f source-lines; 16/8-line C modulation %.4f/%.4fx; four-line glyph contrast %.4fx; max displayY error %.6f\n",
                gui_rf_profile_name(profiles[system]),heads,parity,amplitude,phase*180.0/TEST_PI,contrast[0],contrast[1],width[0],width[1],
                modulation[0],modulation[1],glyph,y_error);
            fflush(stdout);
            /* New preservation floors were fixed from the independently
             * characterized v2 physical chart before testing any NR variant:
             * approximately10% additional detail loss is the maximum allowed.
             * The absolute amplitudes still refer to known input UV, not an
             * image generated by the candidate. Existing gates stay intact. */
            bool pal=profiles[system]==GUI_RF_PROFILE_PAL_SVHS;
            assert(contrast[0]>=.98 && contrast[1]>=.95 && contrast[0]<=1.30 && contrast[1]<=1.30);
            assert(width[0]<=1.0 && width[1]<=4.65);
            assert(modulation[0]>=(detail ? (pal ? .825 : .795) : (pal ? .84 : .795)) && modulation[0]<=1.25);
            assert(modulation[1]>=(detail ? (pal ? .415 : .385) : (pal ? .45 : .432)) && modulation[1]<=1.25);
            assert(glyph>=(pal ? .75 : .71) && glyph<=1.35 && y_error<.012);
        }
        free(rf);
    }
    free(sequence.frame.rgba);gui_rf_decoder_destroy(decoder);
}

/* Noise-on test oracle: RF IQ noise is independent of the known picture.
 * Pixel and row error statistics are kept separate for each original field.
 * Detail modulation is reported even when it is weaker than the noise. */
static void run_svhs_cnr_noise_contracts(void) {
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_PAL_SVHS,GUI_RF_PROFILE_NTSC_SVHS};
    bool strong_edges_ok=true,noise_reduced_ok=true;
    /* Corresponding frozen-v2 same-seed edge widths plus10%, fixed without
     * changing any pre-existing clean-edge threshold. */
    const double width_bound[2][2]={{4.28,2.281},{.94,.82}};
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(profiles[0]);assert(decoder);
    test_ed_sequence_t sequence={0};
    sequence.frame.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(sequence.frame.rgba);
    for(size_t system=0;system<sizeof(profiles)/sizeof(profiles[0]);system++) {
        test_quality_t quality={.pattern=TEST_Q_UV_DETAIL,.fields=8,.swap_tracks=1,.detail_amplitude=.008,
            .detail_phase=TEST_PI*.5,.chroma_noise_amplitude=.30,.chroma_noise_seed=UINT32_C(0x73676873)};
        size_t length;float *rf=make_rf_signal_profile(profiles[system],0,true,1.0,-1,TEST_V_NORMAL,&quality,&length);
        gui_rf_decoder_reset_profile(decoder,profiles[system]);sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
        const uint64_t origin=UINT64_C(16000000003);
        process_ed_chunks(decoder,rf,length,origin,true,&sequence);
        assert(sequence.frame.frames>=3 && sequence.frame.end_sample-origin>=quality.field_start_sample[6]);
        assert(sequence.frame.status.h_locked && sequence.frame.status.v_locked && sequence.frame.status.color_locked);
        printf("%s physical C-band noise: seed%08x whiteIQ amplitude%.3f, 3x800kHz LP before carrier; all-time RF noise RMS%.6f (FM amplitude.72 unchanged)\n",
            gui_rf_profile_name(profiles[system]),(unsigned)quality.chroma_noise_seed,quality.chroma_noise_amplitude,quality.generated_c_noise_rms);
        assert(quality.generated_c_noise_rms>.001 && quality.generated_c_noise_rms<.05);
        for(int parity=0;parity<2;parity++) {
            for(int column=0;column<2;column++) {
                double sum[2]={0},square[2]={0},row_sum[2]={0},row_square[2]={0};size_t pixels=0,rows=0;
                int centre=(int)((column+.5)*sequence.frame.width/5.0);
                double expected_u,expected_v;
                quality_detail_uv(parity,(column+.5)/5.0,150,.008,quality.detail_phase,&expected_u,&expected_v);
                for(int fy=120;fy<200;fy++) {
                    double row[2]={0};int count=0;
                    for(int x=centre-24;x<=centre+24;x++) {
                        double actual[3];svhs_pixel_yuv(&sequence.frame,x,fy*2+parity,actual);
                        double error[2]={actual[1]-expected_u,actual[2]-expected_v};
                        for(int c=0;c<2;c++) {sum[c]+=error[c];square[c]+=error[c]*error[c];row[c]+=error[c];}
                        pixels++;count++;
                    }
                    for(int c=0;c<2;c++) {row[c]/=count;row_sum[c]+=row[c];row_square[c]+=row[c]*row[c];}rows++;
                }
                double variance=0.0,row_variance=0.0;
                for(int c=0;c<2;c++) {
                    variance+=fmax(0.0,square[c]/pixels-pow(sum[c]/pixels,2));
                    row_variance+=fmax(0.0,row_square[c]/rows-pow(row_sum[c]/rows,2));
                    assert(fabs(sum[c]/pixels)<.045);
                }
                if(sqrt(variance*.5)>.012 || sqrt(row_variance*.5)>.007) noise_reduced_ok=false;
                printf("%s noisy field%d flat%d: knownUV bias %.6f/%.6f; pixelUV error std %.6f; rowUV error std %.6f\n",
                    gui_rf_profile_name(profiles[system]),parity,column,sum[0]/pixels,sum[1]/pixels,sqrt(variance*.5),sqrt(row_variance*.5));
            }
            double modulation[2],ignored_y=0.0;
            for(int wave=0;wave<2;wave++) {
                double sine=0.0,cosine=0.0,period=wave==0 ? 16.0 : 8.0;
                for(int fy=40;fy<200;fy++) {
                    double value=svhs_detail_trace(&sequence.frame,parity,wave+2,fy,&ignored_y);
                    double phase=2.0*TEST_PI*fy/period;
                    sine+=value*sin(phase);cosine+=value*cos(phase);
                }
                modulation[wave]=2.0*hypot(sine,cosine)/(160*.008);
            }
            double trace[GUI_RF_FRAME_MAX_HEIGHT/2]={0};
            for(int fy=32;fy<208;fy++) trace[fy]=svhs_detail_trace(&sequence.frame,parity,0,fy,&ignored_y);
            double low=svhs_detail_average(trace,48,64),high=svhs_detail_average(trace,96,112);
            double contrast=(high-low)/.16;assert(contrast>=.95 && contrast<=1.35);
            for(int fy=32;fy<208;fy++) trace[fy]=(trace[fy]-low)/(high-low);
            double x10=svhs_crossing(trace,208,.1,75,85),x50=svhs_crossing(trace,208,.5,75,85),
                x90=svhs_crossing(trace,208,.9,75,85);
            printf("%s noisy field%d weakC amp.008: 16/8H modulation %.6f/%.6fx; strong edge contrast %.4fx centre%.3f width%.3fsource-lines; displayY error%.6f\n",
                gui_rf_profile_name(profiles[system]),parity,modulation[0],modulation[1],contrast,x50,x90-x10,ignored_y);
            fflush(stdout);
            if(!(x10>=0 && x50>=0 && x90>=x10 && fabs(x50-80)<=2.0 && x90-x10<=width_bound[system][parity])) strong_edges_ok=false;
        }
        /* Identical noise but opposite prescribed weak texture allows a
         * finite-difference signal response. A single noisy DFT also contains
         * the random realization at that frequency and is not a clean transfer
         * measurement. This pairing does not assume candidate linearity. */
        test_frames_t positive=sequence.frame;
        size_t bytes=(size_t)positive.width*positive.height*4u;
        positive.rgba=malloc(bytes);assert(positive.rgba);memcpy(positive.rgba,sequence.frame.rgba,bytes);
        free(rf);quality.detail_phase+=TEST_PI;
        rf=make_rf_signal_profile(profiles[system],0,true,1.0,-1,TEST_V_NORMAL,&quality,&length);
        gui_rf_decoder_reset_profile(decoder,profiles[system]);sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
        process_ed_chunks(decoder,rf,length,origin,true,&sequence);
        assert(sequence.frame.frames==positive.frames && sequence.frame.end_sample==positive.end_sample);
        for(int parity=0;parity<2;parity++) {
            double response[2],signed_response[2],quadrature[2],ignored_y=0.0;
            for(int wave=0;wave<2;wave++) {
                double sine=0.0,cosine=0.0,period=wave==0 ? 16.0 : 8.0;
                for(int fy=40;fy<200;fy++) {
                    double value=.5*(svhs_detail_trace(&positive,parity,wave+2,fy,&ignored_y)-
                        svhs_detail_trace(&sequence.frame,parity,wave+2,fy,&ignored_y));
                    double phase=2.0*TEST_PI*fy/period+TEST_PI*.5;
                    sine+=value*sin(phase);cosine+=value*cos(phase);
                }
                response[wave]=2.0*hypot(sine,cosine)/(160*.008);
                signed_response[wave]=2.0*sine/(160*.008);quadrature[wave]=2.0*cosine/(160*.008);
            }
            printf("%s same-noise opposite-texture field%d: finite-difference 16/8H weakC response %.6f/%.6fx; signed %.6f/%.6f quadrature %.6f/%.6f (inputUV amplitude.008)\n",
                gui_rf_profile_name(profiles[system]),parity,response[0],response[1],signed_response[0],signed_response[1],quadrature[0],quadrature[1]);
            fflush(stdout);
            /* This same-seed physical response must not reverse either
             * prescribed weak chroma frequency. It catches the rectangular
             * nine-pair kernel's negative16H lobe; it is not a guarantee for
             * every possible texture, noise realization or tape. */
            assert(signed_response[0]>=0.0 && signed_response[1]>=0.0);
        }
        fflush(stdout);free(positive.rgba);free(rf);
    }
    free(sequence.frame.rgba);gui_rf_decoder_destroy(decoder);
    assert(strong_edges_ok && noise_reduced_ok);
}

static void run_svhs_gray_identity_contracts(void) {
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_PAL_SVHS,GUI_RF_PROFILE_NTSC_SVHS};
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(profiles[0]);assert(decoder);
    test_ed_sequence_t sequence={0};sequence.frame.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(sequence.frame.rgba);
    for(size_t system=0;system<sizeof(profiles)/sizeof(profiles[0]);system++) {
        test_quality_t quality={.pattern=TEST_Q_GRAY,.fields=8,.swap_tracks=1};
        size_t length;float *rf=make_rf_signal_profile(profiles[system],0,true,1.0,-1,TEST_V_NORMAL,&quality,&length);
        gui_rf_decoder_reset_profile(decoder,profiles[system]);sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
        process_ed_chunks(decoder,rf,length,0,true,&sequence);assert(sequence.frame.frames>=3);
        for(int n=0;n<sequence.frame.frames;n++) printf("%s clean gray identity frame%d end%llu hash%016llx\n",
            gui_rf_profile_name(profiles[system]),n,(unsigned long long)sequence.ends[n],(unsigned long long)sequence.hashes[n]);
        free(rf);
    }
    free(sequence.frame.rgba);gui_rf_decoder_destroy(decoder);
}

/* Physical front-end oracle: three simultaneous nonzero physical Y/C
 * transitions. Existing midtone bars are reused; no expected pixel trace is
 * generated from a candidate filter. Keep the original fields separate. */
static void svhs_frontend_midbar_metrics(const test_frames_t *frame,const char *name,int heads,double clock,double jitter) {
    for(int edge=4;edge<=6;edge++) for(int parity=0;parity<2;parity++) {
        int centre=frame->width*edge/8,radius=frame->width/32;
        double old[3]={0},peak_y=0.0,peak_c=0.0,sum_y=0.0,sum_c=0.0,moment_y=0.0,moment_c=0.0;
        int y_edge=0,c_edge=0;
        for(int x=centre-radius;x<=centre+radius;x++) {
            double value[3]={0};int count=0;
            for(int fy=60;fy<80;fy++) {
                double pixel[3];svhs_pixel_yuv(frame,x,fy*2+parity,pixel);
                for(int c=0;c<3;c++) value[c]+=pixel[c];
                count++;
            }
            for(int c=0;c<3;c++) value[c]/=count;
            if(x>centre-radius) {
                double dy=fabs(value[0]-old[0]),dc=hypot(value[1]-old[1],value[2]-old[2]);
                if(dy>peak_y) {peak_y=dy;y_edge=x;}
                if(dc>peak_c) {peak_c=dc;c_edge=x;}
                sum_y+=dy;sum_c+=dc;moment_y+=x*dy;moment_c+=x*dc;
            }
            memcpy(old,value,sizeof(old));
        }
        double centroid=moment_c/sum_c-moment_y/sum_y;
        printf("FRONTREG %s head%d clock%.3f jitter%.2f field%d edge%d Y%d C%d signed%d centroid%.6f steps%.6f/%.6f\n",
            name,heads,clock,jitter,parity,edge,y_edge,c_edge,c_edge-y_edge,centroid,peak_y,peak_c);
        fflush(stdout);
        /* Reuse the existing physical7px/5px registration limit after
         * frozen4cb S-VHS characterization passed it. The centroid remains
         * diagnostic: one5MSPS work sample spans about2.5-3output pixels,
         * so an uncalibrated extra1output-pixel limit is not a contract. */
        bool pal=frame->height==576;
        assert(peak_y>1e-4 && peak_c>1e-4 && abs(c_edge-y_edge)<=(pal ? 7 : 5));

    }
}

static void svhs_frontend_solve_three(double gram[3][3],double rhs[3],double out[3]) {
    double a[3][4];
    for(int r=0;r<3;r++) {for(int c=0;c<3;c++) a[r][c]=gram[r][c];a[r][3]=rhs[r];}
    for(int c=0;c<3;c++) {
        int pivot=c;for(int r=c+1;r<3;r++) if(fabs(a[r][c])>fabs(a[pivot][c])) pivot=r;
        assert(fabs(a[pivot][c])>1e-10);
        for(int j=c;j<4;j++) {double v=a[c][j];a[c][j]=a[pivot][j];a[pivot][j]=v;}
        double divisor=a[c][c];for(int j=c;j<4;j++) a[c][j]/=divisor;
        for(int r=0;r<3;r++) if(r!=c) {double scale=a[r][c];for(int j=c;j<4;j++) a[r][j]-=scale*a[c][j];}
    }
    for(int c=0;c<3;c++) out[c]=a[c][3];
}

static void svhs_frontend_horizontal_metrics(const test_frames_t *frame,bool pal,const char *name,int heads,double frequency) {
    /* Independent physical active intervals from the RF generator above,
     * not queried from the decoder or candidate filter. */
    const double duration=pal ? 52.0 : 53.10555555555556;
    for(int parity=0;parity<2;parity++) {
        double gram[3][3]={{0}},rhs[2][3]={{0}},fit[2][3];
        double ysum=0.0;size_t count=0;
        for(int x=64;x<frame->width-64;x++) {
            double phase=2.0*TEST_PI*frequency*duration*1e-6*(x+.5)/frame->width+(parity ? -.71 : .37);
            double basis[3]={1.0,sin(phase),cos(phase)},uv[2]={0};int rows=0;
            for(int fy=40;fy<frame->height/2-40;fy++) {
                double pixel[3];svhs_pixel_yuv(frame,x,fy*2+parity,pixel);
                const uint8_t *rgb=frame->rgba+((size_t)(fy*2+parity)*frame->width+x)*4u;
                for(int c=0;c<3;c++) assert(rgb[c]>5 && rgb[c]<250);
                uv[0]+=pixel[1];uv[1]+=pixel[2];ysum+=pixel[0];count++;rows++;
            }
            for(int c=0;c<2;c++) {
                uv[c]/=rows;
                for(int j=0;j<3;j++) rhs[c][j]+=basis[j]*uv[c];
            }
            for(int j=0;j<3;j++) for(int k=0;k<3;k++) gram[j][k]+=basis[j]*basis[k];
        }
        for(int c=0;c<2;c++) svhs_frontend_solve_three(gram,rhs[c],fit[c]);
        double gain[2],degrees[2];
        for(int c=0;c<2;c++) {gain[c]=hypot(fit[c][1],fit[c][2])/(c==0 ? .04 : .028);degrees[c]=atan2(fit[c][2],fit[c][1])*180.0/TEST_PI;}
        printf("FRONTSINE %s head%d frequency%.0f field%d gainU%.6f gainV%.6f phaseU%.6f phaseV%.6f meanU%.6f meanV%.6f meanY%.6f\n",
            name,heads,frequency,parity,gain[0],gain[1],degrees[0],degrees[1],fit[0][0],fit[1][0],ysum/count);
        fflush(stdout);
        /* Frozen before candidate measurements: retain at least90percent
         * of the smallest4cb modulation, with no arbitrary gain boost.
         * Known physical zero-phase error may improve; it must not exceed
         * the frozen4cb worst absolute error plus approximately2degrees. */
        const double floor_gain[2][2]={{1.022,1.004},{.973,.938}};
        const double phase_bound[2][2]={{15.6,48.0},{10.5,33.1}};
        int system=pal ? 0 : 1,band=frequency>200000.0 ? 1 : 0;
        assert(gain[0]>=floor_gain[system][band] && gain[1]>=floor_gain[system][band]);
        assert(gain[0]<=1.30 && gain[1]<=1.30);
        assert(fabs(degrees[0])<=phase_bound[system][band] && fabs(degrees[1])<=phase_bound[system][band]);
        assert(fabs(fit[0][0]-(parity ? -.04 : .05))<.025 && fabs(fit[1][0]-(parity ? .05 : .04))<.025);
        assert(fabs(ysum/count-display_level(.5))<.015);
    }
}

static void run_svhs_frontend_contracts(void) {
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_PAL_SVHS,GUI_RF_PROFILE_NTSC_SVHS};
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(profiles[0]);assert(decoder);
    test_ed_sequence_t sequence={0};sequence.frame.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(sequence.frame.rgba);
    for(size_t system=0;system<2;system++) for(int heads=0;heads<2;heads++) {
        bool pal=system==0;
        for(int warped=0;warped<(heads ? 2 : 1);warped++) {
            double clock=warped ? 1.001 : 1.0,jitter=warped ? .08 : 0;
            test_quality_t quality={.pattern=TEST_Q_MID_BARS,.fields=8,.swap_tracks=heads};
            size_t length;float *rf=make_rf_signal_profile(profiles[system],jitter,true,clock,-1,TEST_V_NORMAL,&quality,&length);
            gui_rf_decoder_reset_profile(decoder,profiles[system]);sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
            process_ed_chunks(decoder,rf,length,UINT64_C(16000000003),true,&sequence);
            assert(sequence.frame.frames>=3 && sequence.frame.status.h_locked && sequence.frame.status.v_locked && sequence.frame.status.color_locked);
            svhs_frontend_midbar_metrics(&sequence.frame,gui_rf_profile_name(profiles[system]),heads,clock,jitter);free(rf);
        }
        const double frequencies[2]={100000.0,300000.0};
        for(int f=0;f<2;f++) {
            test_quality_t quality={.pattern=TEST_Q_UV_HORIZONTAL_SINE,.fields=8,.swap_tracks=heads,
                .c_sine_hz=frequencies[f],.c_sine_amplitude=.04};
            size_t length;float *rf=make_rf_signal_profile(profiles[system],0,true,1.0,-1,TEST_V_NORMAL,&quality,&length);
            gui_rf_decoder_reset_profile(decoder,profiles[system]);sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
            process_ed_chunks(decoder,rf,length,UINT64_C(17000000003),true,&sequence);
            assert(sequence.frame.frames>=3 && sequence.frame.status.h_locked && sequence.frame.status.v_locked && sequence.frame.status.color_locked);
            svhs_frontend_horizontal_metrics(&sequence.frame,pal,gui_rf_profile_name(profiles[system]),heads,frequencies[f]);free(rf);
        }
    }
    free(sequence.frame.rgba);gui_rf_decoder_destroy(decoder);
}

static void run_svhs_color_contracts(void) {
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_PAL_SVHS,GUI_RF_PROFILE_NTSC_SVHS};
    const size_t capacity=(size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u;
    gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(profiles[0]);assert(decoder);
    test_ed_sequence_t sequence={0},partition={0};
    sequence.frame.rgba=malloc(capacity);partition.frame.rgba=malloc(capacity);
    assert(sequence.frame.rgba && partition.frame.rgba);
    for(size_t system=0;system<sizeof(profiles)/sizeof(profiles[0]);system++) {
        uint64_t continuation=0;
        for(int horizontal=0;horizontal<2;horizontal++) for(int disturbed=0;disturbed<2;disturbed++) {
            test_quality_t quality={.pattern=horizontal ? TEST_Q_UV_HORIZONTAL : TEST_Q_UV_VERTICAL,
                .fields=8,.swap_tracks=1,.burst_noise_seed=UINT32_C(0x13579bdf),
                .burst_gain_jitter=disturbed ? .28 : 0.0,
                .burst_phase_jitter=disturbed ? 18.0*TEST_PI/180.0 : 0.0};
            size_t length;float *rf=make_rf_signal_profile(profiles[system],0,true,1.0,-1,
                TEST_V_NORMAL,&quality,&length);
            gui_rf_decoder_reset_profile(decoder,profiles[system]);
            sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
            const uint64_t origin=UINT64_C(11000000003);
            process_ed_chunks(decoder,rf,length,origin,false,&sequence);
            continuation=origin+length;
            assert(sequence.frame.end_sample-origin>=quality.field_start_sample[6]);
            char label[160];snprintf(label,sizeof(label),"%s equal-Y %s %s burst",
                gui_rf_profile_name(profiles[system]),horizontal ? "horizontal bands" : "vertical edges",
                disturbed ? "jittered" : "nominal");
            printf("%s: burst-only gain +/-%.1f%%, phase +/-%.1fdeg, seed %08x; active YUV unchanged\n",
                label,100.0*quality.burst_gain_jitter,quality.burst_phase_jitter*180.0/TEST_PI,
                (unsigned)quality.burst_noise_seed);
            double stripe=svhs_color_metrics(&sequence.frame,horizontal!=0,label,-1);
            assert(stripe<=(disturbed ? .010 : .0045));
            if(disturbed && !horizontal) {
                gui_rf_decoder_reset_profile(decoder,profiles[system]);
                partition.frame=(test_frames_t){.rgba=partition.frame.rgba};
                process_ed_chunks(decoder,rf,length,origin,true,&partition);
                assert(partition.frame.frames==sequence.frame.frames);
                for(int frame=0;frame<sequence.frame.frames;frame++) {
                    assert(partition.ends[frame]==sequence.ends[frame]);
                    assert(partition.hashes[frame]==sequence.hashes[frame]);
                }
            }
            free(rf);
        }
        const double phase_jumps[]={60.0,30.0};
        for(size_t phase_case=0;phase_case<sizeof(phase_jumps)/sizeof(phase_jumps[0]);phase_case++) {
            /* A true channel phase jump rotates burst AND active chroma. Known
             * picture UV stays fixed; a long stabilizer must not drag the old
             * channel phase across the boundary. Only four physical source lines
             * on each side are excluded for the two-line decoder's transient. */
            bool pal=profiles[system]==GUI_RF_PROFILE_PAL_SVHS;
            int recovery_line=(pal ? 288 : 240)/2;
            test_quality_t jump={.pattern=TEST_Q_UV_VERTICAL,.fields=8,.swap_tracks=1,
                .chroma_jump_line=(pal ? 22 : 20)+recovery_line,.chroma_jump_radians=phase_jumps[phase_case]*TEST_PI/180.0};
            size_t jump_length;float *jump_rf=make_rf_signal_profile(profiles[system],0,true,1.0,-1,
                TEST_V_NORMAL,&jump,&jump_length);
            gui_rf_decoder_reset_profile(decoder,profiles[system]);
            sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
            const uint64_t jump_origin=UINT64_C(12000000003);
            process_ed_chunks(decoder,jump_rf,jump_length,jump_origin,false,&sequence);
            continuation=jump_origin+jump_length;
            assert(sequence.frame.end_sample-jump_origin>=jump.field_start_sample[6]);
            char jump_label[120];snprintf(jump_label,sizeof(jump_label),"%s true %.0fdeg channel phase jump",
                gui_rf_profile_name(profiles[system]),phase_jumps[phase_case]);
            printf("%s: source line %d; unchanged active UV; +/-4-line recovery window\n",jump_label,jump.chroma_jump_line);
            double jump_stripe=svhs_color_metrics(&sequence.frame,false,jump_label,recovery_line);
            double recovery_hue=svhs_phase_recovery_hue(&sequence.frame,recovery_line,jump_label);
            assert(recovery_hue<=5.0);
            assert(jump_stripe<=.0045);
            gui_rf_decoder_reset_profile(decoder,profiles[system]);
            partition.frame=(test_frames_t){.rgba=partition.frame.rgba};
            process_ed_chunks(decoder,jump_rf,jump_length,jump_origin,true,&partition);
            assert(partition.frame.frames==sequence.frame.frames);
            for(int frame=0;frame<sequence.frame.frames;frame++) {
                assert(partition.ends[frame]==sequence.ends[frame]);
                assert(partition.hashes[frame]==sequence.hashes[frame]);
            }
            free(jump_rf);
        }
        /* Active chroma without burst must not be mistaken for a valid phase
         * reference, including after a previously locked phase-jump interval. */
        test_quality_t no_burst={.pattern=TEST_Q_UV_VERTICAL,.fields=8,.swap_tracks=1,.suppress_burst=true};
        size_t length;float *rf=make_rf_signal_profile(profiles[system],0,true,1.0,-1,
            TEST_V_NORMAL,&no_burst,&length);
        /* Exact continuity first: a locked decoder must age out its old
         * reference instead of keeping color forever from line history. */
        sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
        process_ed_chunks(decoder,rf,length,continuation,true,&sequence);
        check_svhs_no_burst(&sequence.frame,"S-VHS continuous burst loss");
        /* An explicit new segment independently tests reset of histories. */
        gui_rf_decoder_reset_profile(decoder,profiles[system]);
        sequence.frame=(test_frames_t){.rgba=sequence.frame.rgba};
        process_ed_chunks(decoder,rf,length,continuation+length,true,&sequence);
        check_svhs_no_burst(&sequence.frame,"S-VHS reset without burst");
        free(rf);
    }
    free(sequence.frame.rgba);free(partition.frame.rgba);gui_rf_decoder_destroy(decoder);
    run_svhs_burst_outlier_contracts();
    run_svhs_pal_cadence_contracts();
    run_svhs_detail_contracts();
    run_svhs_cnr_noise_contracts();
    run_svhs_gray_identity_contracts();
    run_svhs_frontend_contracts();
}

static void svhs_stripe_probe(const test_frames_t *frame, int parity, int row,
                              int half_width, double uv[2]) {
    uv[0] = uv[1] = 0;
    double scale = 1.0/(2*half_width+1);
    for (int x=frame->width/2-half_width; x<=frame->width/2+half_width; ++x) {
        double value[3];
        svhs_pixel_yuv(frame,x,2*row+parity,value);
        uv[0] += scale*value[1];
        uv[1] += scale*value[2];
    }
}

static double svhs_legal_short_projection(const test_frames_t *frame, int parity, int row) {
    const double truth_u = -.492*svhs_legal_y;
    const double truth_v = .877*(svhs_legal_y/(.299+.587)-svhs_legal_y);
    const double norm = truth_u*truth_u + truth_v*truth_v;
    const int x0 = frame->width*494/1000, x1 = frame->width*506/1000;
    double sum = 0.0;
    for (int x=x0; x<x1; ++x) {
        double yuv[3];
        svhs_pixel_yuv(frame,x,2*row+parity,yuv);
        sum += (truth_u*yuv[1]+truth_v*yuv[2])/norm;
    }
    return sum/(x1-x0);
}

static void run_svhs_stripe_contract(void) {
    const gui_rf_profile_t profile = GUI_RF_PROFILE_PAL_SVHS;
    const uint64_t origin = UINT64_C(18000000003);
    gui_rf_decoder_t *decoder = gui_rf_decoder_create_profile(profile);
    test_ed_sequence_t sequence = {0};
    sequence.frame.rgba = malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(decoder && sequence.frame.rgba);
    double background[2][3][2];
    double healthy[2][9][2];
    bool passes = true;
    for (test_stripe_scene_t scene=STRIPE_BACKGROUND; scene<=STRIPE_LEGAL_SHORT; ++scene) {
        test_quality_t quality = {.pattern=TEST_Q_UV_STRIPES, .fields=8, .stripe_scene=scene,
            .chroma_noise_amplitude=.30, .chroma_noise_seed=UINT32_C(0x73676873)};
        size_t length;
        float *rf = make_rf_signal_profile(profile,0,true,1.0,-1,TEST_V_NORMAL,&quality,&length);
        gui_rf_decoder_reset_profile(decoder,profile);
        sequence.frame = (test_frames_t){.rgba=sequence.frame.rgba};
        process_ed_chunks(decoder,rf,length,origin,true,&sequence);
        free(rf);
        assert(sequence.frame.frames>=3 &&
               sequence.frame.end_sample-origin>=quality.field_start_sample[6]);
        assert(sequence.frame.status.h_locked && sequence.frame.status.v_locked &&
               sequence.frame.status.color_locked);
        for (int parity=0; parity<2; ++parity) {
            if (scene==STRIPE_LEGAL_SHORT) {
                int row = svhs_legal_short_row;
                double reference = 0.0;
                for (int offset=10; offset<=14; ++offset)
                    reference += .1*(svhs_legal_short_projection(&sequence.frame,parity,row-offset) +
                                     svhs_legal_short_projection(&sequence.frame,parity,row+offset));
                double gain = svhs_legal_short_projection(&sequence.frame,parity,row)-reference;
                printf("PAL S-VHS field%d legal short 1H gain %.5f\n",parity,gain);
                passes = passes && gain>=.40;
                continue;
            }
            for (size_t s=0; s<sizeof(svhs_stripes)/sizeof(svhs_stripes[0]); ++s) {
                double minimum = 1.0;
                for (int row=0; row<svhs_stripes[s].span; ++row) {
                    double uv[2];
                    svhs_stripe_probe(&sequence.frame,parity,svhs_stripes[s].row+row,12,uv);
                    double projection = (uv[0]+.7*uv[1])/1.49;
                    if (scene==STRIPE_BACKGROUND) {
                        background[parity][s][row] = projection;
                        continue;
                    }
                    double gain = (projection-background[parity][s][row])*
                                  svhs_stripe_polarity(parity)/svhs_stripes[s].amplitude;
                    minimum = fmin(minimum,gain);
                }
                if (scene!=STRIPE_BACKGROUND) {
                    printf("PAL S-VHS field%d scene%d row%d span%d physical stripe gain %.5f\n",
                           parity,scene,svhs_stripes[s].row,svhs_stripes[s].span,minimum);
                    passes = passes && minimum>=svhs_stripes[s].min_gain;
                }
            }
            if (scene==STRIPE_BACKGROUND) continue;
            double peak = 0.0;
            for (int row=-4; row<=4; ++row) {
                double uv[2];
                svhs_stripe_probe(&sequence.frame,parity,svhs_pulse_row+parity+row,4,uv);
                double *reference = healthy[parity][row+4];
                if (scene==STRIPE_CHART) {
                    memcpy(reference,uv,sizeof(uv));
                    continue;
                }
                peak = fmax(peak,hypot(uv[0]-reference[0],uv[1]-reference[1])/
                                 hypot(svhs_pulse_u,.7*svhs_pulse_u));
            }
            if (scene==STRIPE_CORRUPTED) {
                printf("PAL S-VHS field%d short RF pulse, normalized C peak %.5f\n",parity,peak);
                passes = passes && peak<=.25;
            }
        }
    }
    fflush(stdout);
    free(sequence.frame.rgba);
    gui_rf_decoder_destroy(decoder);
    assert(passes);
}

int main(int argc, char **argv) {
    if (argc==2 && strcmp(argv[1],"--svhs-stripe-only")==0) {
        run_svhs_stripe_contract();
        return 0;
    }
    if(argc==2 && strcmp(argv[1],"--svhs-frontend-only")==0) {run_svhs_frontend_contracts();puts("PAL/NTSC S-VHS physical front-end contracts passed");return 0;}
    if (argc==2 && strcmp(argv[1],"--svhs-cnr-noise-only")==0) {
        run_svhs_cnr_noise_contracts();puts("PAL/NTSC S-VHS physical C-band noise contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--svhs-gray-identity-only")==0) {
        run_svhs_gray_identity_contracts();return 0;
    }
    if (argc==2 && strcmp(argv[1],"--svhs-detail-only")==0) {
        run_svhs_detail_contracts();puts("PAL/NTSC S-VHS physical chroma detail contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--svhs-pal-cadence-only")==0) {
        run_svhs_pal_cadence_contracts();puts("PAL S-VHS physical burst cadence contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--svhs-burst-outlier-only")==0) {
        run_svhs_burst_outlier_contracts();puts("PAL/NTSC S-VHS sparse burst amplitude contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--svhs-color-only")==0) {
        run_svhs_color_contracts();puts("PAL/NTSC S-VHS physical burst/edge/weave contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--svhs-only")==0) {
        run_svhs_contracts();puts("PAL/NTSC S-VHS 20MSPS physical RF contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--superbeta-levels-only")==0) {
        run_superbeta_display_level_contracts();puts("NTSC SuperBeta cached display level contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--superbeta-acquire-only")==0) {
        run_superbeta_acquire_contracts();puts("NTSC SuperBeta adaptive acquisition contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--superbeta-only")==0) {
        run_superbeta_contracts();puts("NTSC SuperBeta RF video contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--beta-acquire-only")==0) {
        run_beta_acquire_contracts();puts("NTSC Betamax adaptive acquisition contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--beta-only")==0) {
        run_beta_contracts();puts("NTSC Betamax RF video contracts passed");return 0;
    }
    if ((argc==2||argc==3) && strcmp(argv[1],"--bench")==0) {
        double seconds=argc==3 ? atof(argv[2]) : 5.0;
        if (!(seconds>=0.5)) seconds=5.0;
        run_bench(GUI_RF_PAL,seconds);
        run_bench(GUI_RF_NTSC,seconds);
        puts("DSP steady-state throughput bench complete");
        return 0;
    }
    if (argc==2 && strcmp(argv[1],"--stop-resume-only")==0) {
        run_stop_resume();puts("Continuous RF stop/resume contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--registration-only")==0) {
        run_registration_contract();puts("Physical RF luma/chroma registration contract passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--quality-only")==0) {
        run_quality_contracts();puts("Physical RF gray, row stability, frequency and fade contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--deemphasis-only")==0) {
        run_deemphasis_contract();puts("NTSC physical de-emphasis transfer contract passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-vertical-only")==0) {
        run_split_vertical_cases();puts("Split vertical positive/negative contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-first-only")==0) {
        run_split_vertical(GUI_RF_NTSC,TEST_V_SPLIT_FIRST,true);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-short-only")==0) {
        run_split_vertical(GUI_RF_NTSC,TEST_V_SPLIT_SHORT_FIRST,true);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-gap-negative-only")==0) {
        run_split_vertical(GUI_RF_NTSC,TEST_V_BAD_SPACING,false);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-h-replay-only")==0) {
        run_split_vertical(GUI_RF_PAL,TEST_V_SHORT_INTERRUPTED,false);
        run_split_vertical(GUI_RF_NTSC,TEST_V_SHORT_INTERRUPTED,false);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-15p5-only")==0) {
        run_split_vertical(GUI_RF_PAL,TEST_V_SPLIT_15P5_FIRST,true);
        run_split_vertical(GUI_RF_NTSC,TEST_V_SPLIT_15P5_FIRST,true);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-boundary-ntsc-only")==0) {
        run_split_vertical(GUI_RF_NTSC,TEST_V_SPLIT_15P5_FIRST,true);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-13p5-only")==0) {
        run_split_vertical(GUI_RF_PAL,TEST_V_SPLIT_13P5_FIRST,true);
        run_split_vertical(GUI_RF_NTSC,TEST_V_SPLIT_13P5_FIRST,true);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--ed-beta-only")==0) {
        run_ed_beta_contracts();puts("ED Beta 20MSPS physical RF contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--full-frame-only")==0) {
        run_full_frame_contracts();puts("Full-frame geometry and level contracts passed");return 0;
    }
    if (argc!=1 && (argc!=3 || strcmp(argv[1],"--real-ntsc-bars")!=0)) {
        fprintf(stderr,"usage: %s [--real-ntsc-bars excerpt.s16le] [--full-frame-only] [--ed-beta-only] [--svhs-only] [--svhs-color-only] [--svhs-burst-outlier-only] [--svhs-pal-cadence-only] [--svhs-detail-only] [--svhs-cnr-noise-only] [--svhs-gray-identity-only] [--svhs-frontend-only]\n",argv[0]);
        return 2;
    }
    run_deemphasis_contract();
    run_beta_contracts();
    run_beta_acquire_contracts();
    run_superbeta_contracts();
    run_superbeta_acquire_contracts();
    run_superbeta_display_level_contracts();
    run_ed_beta_contracts();
    run_svhs_contracts();
    run_svhs_color_contracts();
    run_registration_contract();
    run_quality_contracts();
    run_stop_resume();
    run_format(GUI_RF_PAL);
    run_format(GUI_RF_NTSC);
    run_full_frame_contracts();
    run_damaged_vertical();
    run_split_vertical_cases();
    run_split_gap(GUI_RF_PAL);
    run_split_gap(GUI_RF_NTSC);
    if (argc==3) run_real_ntsc_bars(argv[2]);
    puts("RF preview PAL/NTSC color, TBC, weave and gap contracts passed");
    return 0;
}
