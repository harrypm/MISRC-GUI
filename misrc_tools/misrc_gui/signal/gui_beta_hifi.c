/* NTSC Beta HiFi live monitor.
 *
 * Carrier definitions, 20 -> 5 -> 0.5 MSPS FIR designs, +/-112.5 kHz RF
 * validity, -24 dB dropout threshold, 48 kHz scaling, and the complete frozen
 * sample-16 EQ/stereo-linked dynamics come from the current local
 * vhs-decode-dotnet BetaHiFiRfDecoder.cs / BetaHiFiAudioProcessor.cs,
 * integration beta-hifi-integration-20261020. AudioProcessor SHA256:
 * 03f63e24d693181b2522747f6e609106ef2d0d23efbd7dc8b167628d4a1c9c71.
 * That empirical compensation is
 * not an exact reconstruction of a Sony noise-reduction circuit.
 *
 * Streaming adaptations are deliberately causal: RF FIRs retain their group
 * delay, power and audio IIRs retain state, the line notch runs twice forward
 * (the same steady-state magnitude as offline filtfilt, different phase), and
 * calibration uses bounded recent power observations. No whole-file mean or
 * normalization is used. Short losses use bounded lookahead interpolation
 * only between valid same-channel anchors; long losses and unlocked input
 * are muted. This is not bit-identical to the offline context-based decoder.
 */
#include "gui_beta_hifi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if (defined(__i386__) || defined(__x86_64__)) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(GUI_BETA_HIFI_DISABLE_AVX2)
#include <immintrin.h>
#define BETA_RUNTIME_AVX2 1
#else
#define BETA_RUNTIME_AVX2 0
#endif

#define BETA_PI 3.14159265358979323846
#define BETA_RF_TAPS 81
#define BETA_IQ_TAPS 639
#define BETA_AUDIO_TAPS 2501
#define BETA_AUDIO_HISTORY 224
#define BETA_CAL_POINTS 300
#define BETA_REPAIR_DELAY 96
#define BETA_PCM_BATCH 256
#define BETA_IQ_RATE 500000.0
#define BETA_FIELD_IQ (500000.0 * 1001.0 / 60000.0)

typedef struct { double b0, b1, b2, a1, a2, z1, z2; } beta_biquad_t;
typedef struct { beta_biquad_t l, r; } beta_stereo_filter_t;
typedef void (*beta_iq_pair_t)(const float *,const float *,const float *,
                               double *,double *);

typedef struct {
    beta_stereo_filter_t eq, notch[2], high[2], fallback[2], top[2];
    beta_stereo_filter_t low, top_low, upper;
    double wide_power, high_power, fallback_power, top_power;
    double wide_sum, high_sum, fallback_sum, top_sum;
    double gain, shelf, top_gain, start_gain, start_shelf, start_top;
    int tick;
} beta_audio_t;

typedef struct {
    float real[BETA_IQ_TAPS * 2], imag[BETA_IQ_TAPS * 2];
    double osc_re, osc_im, step_re, step_im;
    double prev_re, prev_im, power, reference;
    beta_biquad_t power_filter;
    bool previous;
} beta_carrier_t;

typedef struct {
    double previous_good;
    double pending[BETA_REPAIR_DELAY];
    bool invalid[BETA_REPAIR_DELAY], previous_valid;
    int bad_run;
} beta_repair_t;

struct gui_beta_hifi_decoder {
    gui_beta_hifi_status_t status;
    float rf_taps[BETA_RF_TAPS], iq_taps[BETA_IQ_TAPS];
    float rf_history[BETA_RF_TAPS * 2];
    float resample_taps[BETA_AUDIO_TAPS];
    double audio_history[2][BETA_AUDIO_HISTORY];
    beta_carrier_t carrier[4];
    beta_biquad_t audio_low[2][3], audio_high[2];
    beta_repair_t repair[2];
    beta_audio_t audio;
    double calibration[BETA_CAL_POINTS][5];
    double midpoint, input_power, gate;
    float pcm[BETA_PCM_BATCH * 2];
    size_t pcm_count;
    uint64_t expected_sample, iq_count, resample_count, resample_next;
    uint64_t last_switch, last_good_switch, calibration_iq_start;
    int rf_pos, rf_phase, iq_pos, iq_phase, cal_phase, cal_count, cal_next;
    int candidate_run, candidate_head, head;
    int bad_count;
    bool have_position, have_head;
    gui_beta_hifi_iq_kernel_t requested_kernel,active_kernel;
    beta_iq_pair_t iq_pair;
};

static double beta_clamp(double x, double low, double high) {
    return x < low ? low : x > high ? high : x;
}

/* Production builds enable fast-math; arithmetic isfinite() checks can then
 * disappear. Inspect IEEE bits so malformed RF cannot enter filter history. */
static bool beta_finite(double value) {
    uint64_t bits;
    _Static_assert(sizeof(bits)==sizeof(value),"IEEE binary64 required");
    memcpy(&bits,&value,sizeof(bits));
    return (bits&UINT64_C(0x7ff0000000000000))!=UINT64_C(0x7ff0000000000000);
}

static bool beta_finite_float(float value) {
    uint32_t bits;
    _Static_assert(sizeof(bits)==sizeof(value),"IEEE binary32 required");
    memcpy(&bits,&value,sizeof(bits));
    return (bits&UINT32_C(0x7f800000))!=UINT32_C(0x7f800000);
}

static double beta_filter(beta_biquad_t *f, double x) {
    double y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static beta_biquad_t beta_coeff(double b0, double b1, double b2,
                                double a1, double a2) {
    beta_biquad_t result = {b0,b1,b2,a1,a2,0,0};
    return result;
}

static beta_stereo_filter_t beta_stereo(double b0, double b1, double b2,
                                        double a1, double a2) {
    beta_stereo_filter_t result;
    result.l = result.r = beta_coeff(b0,b1,b2,a1,a2);
    return result;
}

static void beta_stereo_process(beta_stereo_filter_t *f, double *l, double *r) {
    *l = beta_filter(&f->l,*l);
    *r = beta_filter(&f->r,*r);
}

static beta_biquad_t beta_lowpass(double rate, double frequency, double q) {
    double k = tan(BETA_PI * frequency / rate);
    double norm = 1.0 / (1.0 + k / q + k * k);
    return beta_coeff(k*k*norm,2*k*k*norm,k*k*norm,
                      2*(k*k-1)*norm,(1-k/q+k*k)*norm);
}

static beta_biquad_t beta_highpass(double rate, double frequency, double q) {
    double k = tan(BETA_PI * frequency / rate);
    double norm = 1.0 / (1.0 + k / q + k * k);
    return beta_coeff(norm,-2*norm,norm,2*(k*k-1)*norm,(1-k/q+k*k)*norm);
}

static beta_biquad_t beta_first_lowpass(double rate, double frequency) {
    double k = tan(BETA_PI * frequency / rate);
    return beta_coeff(k/(1+k),k/(1+k),0,(k-1)/(k+1),0);
}

static double beta_i0(double x) {
    double sum=1,term=1,squared=x*x/4;
    for(int k=1;k<100;k++) {
        term *= squared/(k*k);
        sum += term;
        if(term < sum*1e-17) break;
    }
    return sum;
}

static void beta_fir_design(float *taps, int count, double cutoff, double beta,
                            double gain) {
    int half=count/2;
    double denominator=beta_i0(beta),sum=0;
    for(int i=0;i<count;i++) {
        double distance=i-half,argument=cutoff*distance;
        double sinc=argument==0 ? 1 : sin(BETA_PI*argument)/(BETA_PI*argument);
        double position=distance/half;
        double window=beta_i0(beta*sqrt(fmax(0,1-position*position)))/denominator;
        taps[i]=(float)(cutoff*sinc*window);
        sum += taps[i];
    }
    for(int i=0;i<count;i++) taps[i]=(float)(taps[i]*gain/sum);
}

/* Independent vector accumulators avoid reassociation/fast-math flags and
 * keep the 639-tap four-carrier front end practical on the x86 GUI target. */
static float beta_dot(const float *a,const float *b,int count) {
    int i=0;
    float sum=0;
#if defined(__SSE2__)
    __m128 acc0=_mm_setzero_ps(),acc1=_mm_setzero_ps();
    for(;i+8<=count;i+=8) {
        acc0=_mm_add_ps(acc0,_mm_mul_ps(_mm_loadu_ps(a+i),_mm_loadu_ps(b+i)));
        acc1=_mm_add_ps(acc1,_mm_mul_ps(_mm_loadu_ps(a+i+4),_mm_loadu_ps(b+i+4)));
    }
    float lane[4];
    _mm_storeu_ps(lane,_mm_add_ps(acc0,acc1));
    sum=lane[0]+lane[1]+lane[2]+lane[3];
#endif
    for(;i<count;i++) sum += a[i]*b[i];
    return sum;
}

static void beta_dot_iq_scalar(const float *real,const float *imaginary,
                                const float *taps,double *re,double *im) {
    const int count=BETA_IQ_TAPS;
    float r=0,q=0;
    for(int i=0;i<count;i++) r+=real[i]*taps[i];
    for(int i=0;i<count;i++) q+=imaginary[i]*taps[i];
    *re=r;*im=q;
}

#if defined(__SSE2__)
/* One carrier's real/imaginary FIRs share each coefficient load. Each output
 * keeps the original dot's independent accumulators and reduction expression. */
static void beta_dot_iq_sse2(const float *real,const float *imaginary,
                              const float *taps,double *re,double *im) {
    const int count=BETA_IQ_TAPS;
    int i=0;
    __m128 re0=_mm_setzero_ps(),re1=_mm_setzero_ps();
    __m128 im0=_mm_setzero_ps(),im1=_mm_setzero_ps();
    for(;i+8<=count;i+=8) {
        __m128 taps0=_mm_loadu_ps(taps+i),taps1=_mm_loadu_ps(taps+i+4);
        re0=_mm_add_ps(re0,_mm_mul_ps(_mm_loadu_ps(real+i),taps0));
        re1=_mm_add_ps(re1,_mm_mul_ps(_mm_loadu_ps(real+i+4),taps1));
        im0=_mm_add_ps(im0,_mm_mul_ps(_mm_loadu_ps(imaginary+i),taps0));
        im1=_mm_add_ps(im1,_mm_mul_ps(_mm_loadu_ps(imaginary+i+4),taps1));
    }
    float lane[4];
    _mm_storeu_ps(lane,_mm_add_ps(re0,re1));
    float r=lane[0]+lane[1]+lane[2]+lane[3];
    for(int tail=i;tail<count;tail++) r+=real[tail]*taps[tail];
    *re=r;
    _mm_storeu_ps(lane,_mm_add_ps(im0,im1));
    float q=lane[0]+lane[1]+lane[2]+lane[3];
    for(;i<count;i++) q+=imaginary[i]*taps[i];
    *im=q;
}
#endif

#if BETA_RUNTIME_AVX2
/* Each eight-lane accumulator retains the old SSE2 acc0 in its low half
 * and acc1 in its high half. No FMA or wider horizontal reduction is used.
 * The target attribute isolates AVX2 instructions from the baseline binary. */
__attribute__((target("avx2,no-fma")))
static void beta_dot_iq_avx2(const float *real,const float *imaginary,
                             const float *taps,double *re,double *im) {
    const int count=BETA_IQ_TAPS;
    int i=0;
    __m256 r_acc=_mm256_setzero_ps(),q_acc=_mm256_setzero_ps();
    for(;i+8<=count;i+=8) {
        __m256 coefficients=_mm256_loadu_ps(taps+i);
        r_acc=_mm256_add_ps(r_acc,_mm256_mul_ps(_mm256_loadu_ps(real+i),coefficients));
        q_acc=_mm256_add_ps(q_acc,_mm256_mul_ps(_mm256_loadu_ps(imaginary+i),coefficients));
    }
    float lane[4];
    _mm_storeu_ps(lane,_mm_add_ps(_mm256_castps256_ps128(r_acc),_mm256_extractf128_ps(r_acc,1)));
    float r=lane[0]+lane[1]+lane[2]+lane[3];
    for(int tail=i;tail<count;tail++) r+=real[tail]*taps[tail];
    *re=r;
    _mm_storeu_ps(lane,_mm_add_ps(_mm256_castps256_ps128(q_acc),_mm256_extractf128_ps(q_acc,1)));
    float q=lane[0]+lane[1]+lane[2]+lane[3];
    for(;i<count;i++) q+=imaginary[i]*taps[i];
    *im=q;
}
#endif

static bool beta_select_iq_kernel(gui_beta_hifi_decoder_t *d,
                                   gui_beta_hifi_iq_kernel_t requested) {
    gui_beta_hifi_iq_kernel_t selected=requested;
    bool avx2=false;
#if BETA_RUNTIME_AVX2
    __builtin_cpu_init();
    avx2=__builtin_cpu_supports("avx2")!=0;
#endif
    if(selected==GUI_BETA_HIFI_IQ_AUTO) {
        if(avx2) selected=GUI_BETA_HIFI_IQ_AVX2;
#if defined(__SSE2__)
        else selected=GUI_BETA_HIFI_IQ_SSE2;
#else
        else selected=GUI_BETA_HIFI_IQ_SCALAR;
#endif
    }
    beta_iq_pair_t pair=NULL;
    if(selected==GUI_BETA_HIFI_IQ_SCALAR) pair=beta_dot_iq_scalar;
#if defined(__SSE2__)
    else if(selected==GUI_BETA_HIFI_IQ_SSE2) pair=beta_dot_iq_sse2;
#endif
#if BETA_RUNTIME_AVX2
    else if(selected==GUI_BETA_HIFI_IQ_AVX2 && avx2) pair=beta_dot_iq_avx2;
#endif
    if(!pair) return false;
    d->requested_kernel=requested;d->active_kernel=selected;d->iq_pair=pair;
    return true;
}
static void beta_audio_reset(beta_audio_t *a) {
    memset(a,0,sizeof(*a));
    a->eq=beta_stereo(.09835496353257453,.0722833497364509,0,-.9157105270297415,0);
    a->notch[0]=a->notch[1]=beta_stereo(.9970665887563688,.9360736023894386,
                            .9970665887563688,.9360736023894386,.9941331775127376);
    a->high[0]=beta_stereo(.1866943331163784,.3733886662327568,.1866943331163784,
                          -.16292179235145482,.32719796013797375);
    a->high[1]=beta_stereo(1,-2,1,-1.482760782667161,.6409433532780014);
    a->fallback[0]=beta_stereo(.4136239424029401,.8272478848058802,.4136239424029401,
                              .45893731156438367,.21104050922719947);
    a->fallback[1]=beta_stereo(1,-2,1,-1.9814884366082797,.9816596948720536);
    a->top[0]=beta_stereo(.07223087532575317,-.14446175065150635,.07223087532575317,
                         -.28296446826868,.626020355471084);
    a->top[1]=beta_stereo(1,2,1,.49771451813574963,.6360053478162506);
    a->low=beta_stereo(.061511768503621556,.061511768503621556,0,-.8769764629927568,0);
    a->top_low=beta_stereo(.4341737512063021,.4341737512063021,0,-.13165249758739583,0);
    a->upper=beta_stereo(.7158873405487501,-.7984346476182613,.286893473882612,
                        -1.2667540248817235,.47110019169482453);
    a->wide_power=pow(10,-22.4264/10);
    a->high_power=pow(10,(-22.4264-18)/10);
    a->fallback_power=pow(10,(-22.4264-18+10)/10);
    a->top_power=pow(10,-55.54927441601673/10);
    a->gain=a->start_gain=-2.530979500375161;
}

static double beta_envelope(double previous,double target,double attack,double release) {
    double a=exp(-1/(target>previous ? attack : release));
    return a*previous+(1-a)*target;
}

static double beta_db(double x) { return 10*log10(fmax(x,1e-30)); }

static void beta_audio_process(beta_audio_t *a,double *left,double *right) {
    beta_stereo_process(&a->eq,left,right);
    beta_stereo_process(&a->notch[0],left,right);
    beta_stereo_process(&a->notch[1],left,right);
    double hl=*left,hr=*right,fl=*left,fr=*right,tl=*left,tr=*right;
    for(int k=0;k<2;k++) {
        beta_stereo_process(&a->high[k],&hl,&hr);
        beta_stereo_process(&a->fallback[k],&fl,&fr);
        beta_stereo_process(&a->top[k],&tl,&tr);
    }
    double hp=(hl*hl+hr*hr)/2,fp=(fl*fl+fr*fr)/2,tp=(tl*tl+tr*tr)/2;
    a->wide_sum += fp+63.09573444801933*hp;
    a->high_sum += hp; a->fallback_sum += fp; a->top_sum += tp;
    double ll=*left,lr=*right;
    beta_stereo_process(&a->low,&ll,&lr);
    double fraction=(a->tick+1.0)/48;
    double gain=pow(10,(a->start_gain+fraction*(a->gain-a->start_gain))/20);
    double shelf=pow(10,(a->start_shelf+fraction*(a->shelf-a->start_shelf))/20);
    double top=pow(10,(a->start_top+fraction*(a->top_gain-a->start_top))/20);
    *left=(*left+(shelf-1)*(*left-ll))*gain;
    *right=(*right+(shelf-1)*(*right-lr))*gain;
    beta_stereo_process(&a->upper,left,right);
    ll=*left;lr=*right;
    beta_stereo_process(&a->top_low,&ll,&lr);
    *left += (top-1)*(*left-ll); *right += (top-1)*(*right-lr);
    if(++a->tick==48) {
        a->wide_power=beta_envelope(a->wide_power,a->wide_sum/48,5,20);
        a->high_power=beta_envelope(a->high_power,a->high_sum/48,5,35);
        a->fallback_power=beta_envelope(a->fallback_power,a->fallback_sum/48,5,35);
        a->top_power=beta_envelope(a->top_power,a->top_sum/48,5,35);
        double level=beta_db(a->wide_power),balance=beta_db(a->high_power)-beta_db(a->fallback_power)+10;
        double wide=-2.530979500375161+beta_clamp((level+22.4264)*.6434244433339258,-12,6);
        double sh=beta_clamp(2.0820134934911185+.04346560836625418*(level+22.4264)
                             +.18803176557402168*balance,-6,3);
        double tg=beta_clamp(.26618843764941263*(beta_db(a->top_power)+55.54927441601673),-1.5,1.5);
        a->start_gain=a->gain;a->start_shelf=a->shelf;a->start_top=a->top_gain;
        a->gain=beta_envelope(a->gain,wide,3,50);
        a->shelf=beta_envelope(a->shelf,sh,3,50);
        a->top_gain=beta_envelope(a->top_gain,tg,3,50);
        a->wide_sum=a->high_sum=a->fallback_sum=a->top_sum=0;a->tick=0;
    }
}

static void beta_reason(gui_beta_hifi_decoder_t *d,const char *reason) {
    snprintf(d->status.reason,sizeof(d->status.reason),"%s",reason);
}

static void beta_unlock(gui_beta_hifi_decoder_t *d,const char *reason) {
    if(d->status.locked) {
        beta_audio_reset(&d->audio);
        d->cal_count=d->cal_next=0;
        d->calibration_iq_start=d->iq_count;
    }
    d->status.locked=false;
    d->gate=0;
    beta_reason(d,reason);
}

static double beta_quantile(double *values,int count,double fraction) {
    if(count<1) return 0;
    /* Bounded 300-point insertion sort needs no implementation-dependent
     * qsort scratch allocation on the processing thread. */
    for(int i=1;i<count;i++) {
        double value=values[i];int j=i;
        while(j>0 && values[j-1]>value) {values[j]=values[j-1];j--;}
        values[j]=value;
    }
    double pos=(count-1)*fraction;
    int lo=(int)pos,hi=lo+1<count ? lo+1 : lo;
    return values[lo]+(values[hi]-values[lo])*(pos-lo);
}

/* A rolling acquisition window can straddle a programme/head-level change
 * even while the live head detector still follows regular, usable RF. An
 * invalid refresh must retain the last verified calibration; the independent
 * live cadence and active-carrier loss guards decide when a lock is lost. */
static void beta_calibrate(gui_beta_hifi_decoder_t *d) {
    if(d->cal_count<250 || d->iq_count-d->calibration_iq_start<125000) return;
    double values[BETA_CAL_POINTS],p10,p90;
    for(int i=0;i<d->cal_count;i++) values[i]=d->calibration[i][4];
    p10=beta_quantile(values,d->cal_count,.1);
    p90=beta_quantile(values,d->cal_count,.9);
    if(p90-p10<=10) {
        if(!d->status.locked)
            beta_unlock(d,"Waiting for alternating NTSC Beta HiFi carriers");
        return;
    }
    double midpoint=(p10+p90)/2,references[4];
    /* Learn the capture's response imbalance before identifying heads. Both
     * physical head states can lie on the same side of zero dominance.
     * Reclassify this ordered 1 ms window with the newly measured midpoint;
     * using the provisional live detector here would deadlock calibration. */
    int oldest=(d->cal_next+BETA_CAL_POINTS-d->cal_count)%BETA_CAL_POINTS;
    int previous_head=0,last_edge=-1,window_switches=0,regular_switches=0;
    for(int i=0;i<d->cal_count;i++) {
        double centered=d->calibration[(oldest+i)%BETA_CAL_POINTS][4]-midpoint;
        int current=centered>=3 ? 1 : centered<=-3 ? -1 : 0;
        if(!current) continue;
        if(previous_head && current!=previous_head) {
            if(last_edge>=0 && fabs((i-last_edge)*500.0/BETA_FIELD_IQ-1)<=.2)
                regular_switches++;
            last_edge=i;window_switches++;
        }
        previous_head=current;
    }
    if(window_switches<8 || regular_switches*10<(window_switches-1)*7) {
        if(!d->status.locked)
            beta_unlock(d,"Waiting for regular NTSC Beta HiFi head cadence");
        return;
    }
    for(int branch=0;branch<4;branch++) {
        int active=0;
        bool group_a=(branch&1)==0;
        for(int i=0;i<d->cal_count;i++) {
            double dom=d->calibration[i][4]-midpoint;
            if((group_a && dom>3) || (!group_a && dom<-3))
                values[active++]=d->calibration[i][branch];
        }
        references[branch]=beta_quantile(values,active,.9);
        if(active<40 || references[branch]<fmax(1e-20,d->input_power*1e-7)) {
            if(!d->status.locked)
                beta_unlock(d,"Missing NTSC Beta HiFi carrier");
            return;
        }
        /* Do not require this carrier to be stronger than its other-head
         * residual. Actual Beta captures can have much weaker head B RF,
         * while B-frequency residuals on head A are stronger than head B's
         * programme. The reference .NET decoder calibrates each active
         * carrier independently after joint dominance/cadence classification. */
    }
    /* Commit the candidate as a whole only after every acquisition check. */
    d->midpoint=midpoint;
    for(int i=0;i<4;i++) d->carrier[i].reference=references[i];
    if(!d->status.locked) {
        d->bad_count=0;
        beta_audio_reset(&d->audio);
        d->head=previous_head;
        d->have_head=true;
        d->candidate_head=previous_head;d->candidate_run=0;
        /* iq_count already includes this observation; switch timestamps and
         * the same-sample cadence check use its zero-based IQ index. */
        d->last_switch=d->last_good_switch=d->iq_count-1
                             -(uint64_t)(d->cal_count-1-last_edge)*500;
    }
    d->status.locked=true;
    beta_reason(d,"NTSC Beta HiFi locked");
}

static void beta_emit(gui_beta_hifi_decoder_t *d,uint64_t end_sample,
                       gui_beta_hifi_pcm_callback_t callback,void *context) {
    if(d->pcm_count && callback) callback(context,d->pcm,d->pcm_count,end_sample,&d->status);
    d->pcm_count=0;
}

static void beta_resample(gui_beta_hifi_decoder_t *d,double left,double right,
                           uint64_t end_sample,gui_beta_hifi_pcm_callback_t callback,
                           void *context) {
    uint64_t index=d->resample_count++;
    d->audio_history[0][index%BETA_AUDIO_HISTORY]=left;
    d->audio_history[1][index%BETA_AUDIO_HISTORY]=right;
    while(d->resample_next/12<=index) {
        uint64_t center=d->resample_next;
        double output[2]={0,0};
        uint64_t newest=center/12;
        for(int k=0;k<BETA_AUDIO_HISTORY;k++) {
            if((uint64_t)k>newest) break;
            uint64_t source=newest-(uint64_t)k;
            uint64_t coefficient=center-source*12;
            if(coefficient>=BETA_AUDIO_TAPS) break;
            for(int ch=0;ch<2;ch++)
                output[ch] += d->audio_history[ch][source%BETA_AUDIO_HISTORY]*d->resample_taps[coefficient];
        }
        beta_audio_process(&d->audio,&output[0],&output[1]);
        if(!beta_finite(output[0]) || !beta_finite(output[1])) {
            output[0]=output[1]=0;
            beta_unlock(d,"Invalid Beta HiFi audio muted");
        }
        if(d->status.locked) d->gate=fmin(1,d->gate+1.0/480);
        else d->gate=0;
        for(int ch=0;ch<2;ch++) {
            double value=output[ch]*d->gate;
            d->pcm[d->pcm_count*2+(size_t)ch]=(float)beta_clamp(value,-.98,.98);
        }
        d->pcm_count++; d->status.pcm_frames++;
        if(d->pcm_count==BETA_PCM_BATCH) beta_emit(d,end_sample,callback,context);
        d->resample_next+=125;
    }
}

/* 192 us lookahead retains both valid anchors of the upstream <=80 us
 * repair case. It never substitutes the other audio channel. A longer loss
 * is silent; importantly, a contaminated endpoint is never an anchor. */
static double beta_repair_sample(gui_beta_hifi_decoder_t *d,int channel,
                                  double value,bool valid,uint64_t index) {
    beta_repair_t *r=&d->repair[channel];
    size_t slot=(size_t)(index%BETA_REPAIR_DELAY);
    r->pending[slot]=valid ? value : 0;
    r->invalid[slot]=!valid;
    if(!valid) {
        if(r->bad_run==0) d->status.dropouts++;
        if(r->bad_run<BETA_REPAIR_DELAY) r->bad_run++;
    } else {
        if(r->bad_run>0 && r->bad_run<=40 && r->previous_valid) {
            for(int i=0;i<r->bad_run;i++) {
                uint64_t at=index-(uint64_t)r->bad_run+(uint64_t)i;
                double weight=(i+1.0)/(r->bad_run+1);
                r->pending[at%BETA_REPAIR_DELAY]=r->previous_good*(1-weight)+value*weight;
                r->invalid[at%BETA_REPAIR_DELAY]=false;
            }
        }
        r->bad_run=0;r->previous_good=value;r->previous_valid=true;
    }
    if(r->bad_run>40) r->previous_valid=false;
    if(index<BETA_REPAIR_DELAY-1) return 0;
    size_t emit=(size_t)((index+1)%BETA_REPAIR_DELAY);
    return r->invalid[emit] ? 0 : r->pending[emit];
}

static void beta_iq(gui_beta_hifi_decoder_t *d,double real[4],double imaginary[4],
                     double power[4],
                     uint64_t end_sample,gui_beta_hifi_pcm_callback_t callback,
                     void *context) {
    uint64_t index=d->iq_count++;
    double dominance=5/log(10.0)*(log(fmax(power[0],1e-30))+log(fmax(power[2],1e-30))
                         -log(fmax(power[1],1e-30))-log(fmax(power[3],1e-30)));
    double centered=dominance-d->midpoint;
    int candidate=centered>=3 ? 1 : centered<=-3 ? -1 : 0;
    if(candidate && candidate==d->candidate_head) {
        /* Only the qualification threshold matters; bound long single-head RF. */
        if(d->candidate_run<75) d->candidate_run++;
    }
    else {d->candidate_head=candidate;d->candidate_run=candidate ? 1 : 0;}
    bool switched=false;
    if(d->candidate_run>=75 && (!d->have_head || candidate!=d->head)) {
        if(!d->have_head || index-d->last_switch>(uint64_t)(.5*BETA_FIELD_IQ)) {
            if(d->have_head) {
                double period=(double)(index-d->last_switch);
                d->status.head_switches++;
                if(fabs(period/BETA_FIELD_IQ-1)<=.2) {
                    d->last_good_switch=index;
                }
                switched=true;
            }
            d->last_switch=index;d->head=candidate;d->have_head=true;
        }
    }
    if(++d->cal_phase==500) {
        d->cal_phase=0;
        for(int branch=0;branch<4;branch++) d->calibration[d->cal_next][branch]=power[branch];
        d->calibration[d->cal_next][4]=dominance;
        d->cal_next=(d->cal_next+1)%BETA_CAL_POINTS;
        if(d->cal_count<BETA_CAL_POINTS) d->cal_count++;
        if(d->cal_count>=250 && d->cal_next%50==0) beta_calibrate(d);
    }
    if(d->status.locked && index-d->last_good_switch>25000)
        beta_unlock(d,"Beta HiFi head cadence lost; muted");
    /* Power tracking and calibration can select a different head on this very
     * sample. Demodulate its two carriers only after that decision, retaining
     * all four previous IQ samples so a newly selected head has no phase gap.
     * The inactive pair's atan2 results were never consumed by the audio path. */
    double frequency[4]={0,0,0,0};
    for(int channel=0;channel<2;channel++) {
        int branch=2*channel+(d->head>0 ? 0 : 1);
        beta_carrier_t *c=&d->carrier[branch];
        double re=real[branch],im=imaginary[branch];
        frequency[branch]=c->previous ? atan2(im*c->prev_re-re*c->prev_im,
                              re*c->prev_re+im*c->prev_im)*(BETA_IQ_RATE/(2*BETA_PI)) : 0;
    }
    for(int branch=0;branch<4;branch++) {
        beta_carrier_t *c=&d->carrier[branch];
        c->prev_re=real[branch];c->prev_im=imaginary[branch];c->previous=true;
    }
    double audio[2];bool pair_valid=true;
    for(int channel=0;channel<2;channel++) {
        int branch=2*channel+(d->head>0 ? 0 : 1);
        bool valid=d->status.locked && d->have_head && !switched
                    && beta_finite(frequency[branch]) && fabs(frequency[branch])<=112500
                    && power[branch]>d->carrier[branch].reference*.003981071705534973;
        if(!valid) pair_valid=false;
        audio[channel]=beta_repair_sample(d,channel,frequency[branch],valid,index);
        for(int k=0;k<3;k++) audio[channel]=beta_filter(&d->audio_low[channel][k],audio[channel]);
        audio[channel]=beta_filter(&d->audio_high[channel],audio[channel])*(.5/75000);
    }
    if(!pair_valid) {if(d->bad_count<10000) d->bad_count++;}
    else d->bad_count=0;
    if(d->status.locked && d->bad_count>=5000)
        beta_unlock(d,"Beta HiFi carrier RF lost; muted");
    beta_resample(d,audio[0],audio[1],end_sample,callback,context);
}

static void beta_reduced_sample(gui_beta_hifi_decoder_t *d,float sample,
                                 uint64_t end_sample,gui_beta_hifi_pcm_callback_t callback,
                                 void *context) {
    int pos=d->iq_pos;
    for(int branch=0;branch<4;branch++) {
        beta_carrier_t *c=&d->carrier[branch];
        float re=(float)(sample*c->osc_re),im=(float)(sample*c->osc_im);
        c->real[pos]=c->real[pos+BETA_IQ_TAPS]=re;
        c->imag[pos]=c->imag[pos+BETA_IQ_TAPS]=im;
        double next_re=c->osc_re*c->step_re-c->osc_im*c->step_im;
        c->osc_im=c->osc_im*c->step_re+c->osc_re*c->step_im;
        c->osc_re=next_re;
    }
    d->iq_pos=(pos+1)%BETA_IQ_TAPS;
    if(++d->iq_phase!=10) return;
    d->iq_phase=0;
    double real[4],imaginary[4],power[4];
    for(int branch=0;branch<4;branch++) {
        beta_carrier_t *c=&d->carrier[branch];
        double re,im;
        d->iq_pair(c->real+d->iq_pos,c->imag+d->iq_pos,d->iq_taps,&re,&im);
        real[branch]=re;imaginary[branch]=im;
        c->power=fmax(1e-30,beta_filter(&c->power_filter,re*re+im*im));
        power[branch]=c->power;
        /* Prevent oscillator drift without introducing phase discontinuities. */
        if((d->iq_count&4095)==0) {
            double scale=1/sqrt(c->osc_re*c->osc_re+c->osc_im*c->osc_im);
            c->osc_re*=scale;c->osc_im*=scale;
        }
    }
    beta_iq(d,real,imaginary,power,end_sample,callback,context);
}

gui_beta_hifi_decoder_t *gui_beta_hifi_create(void) {
    gui_beta_hifi_decoder_t *d=calloc(1,sizeof(*d));
    if(d) gui_beta_hifi_reset(d);
    return d;
}

void gui_beta_hifi_destroy(gui_beta_hifi_decoder_t *decoder) {free(decoder);}

void gui_beta_hifi_reset(gui_beta_hifi_decoder_t *d) {
    if(!d) return;
    uint64_t resets=d->status.resets+1;
    gui_beta_hifi_iq_kernel_t requested=d->requested_kernel;
    memset(d,0,sizeof(*d));
    (void)beta_select_iq_kernel(d,requested);
    d->status.resets=resets;
    beta_reason(d,"Calibrating NTSC Beta HiFi; muted");
    beta_fir_design(d->rf_taps,BETA_RF_TAPS,.25,5,1);
    beta_fir_design(d->iq_taps,BETA_IQ_TAPS,110000.0/2500000.0,8.6,1);
    beta_fir_design(d->resample_taps,BETA_AUDIO_TAPS,1.0/125,5,12);
    const double carrier_multiples[4]={87.75,97.25,106.75,116.25};
    for(int branch=0;branch<4;branch++) {
        beta_carrier_t *c=&d->carrier[branch];
        double phase=-2*BETA_PI*carrier_multiples[branch]*(15750000.0/1001)/5000000;
        c->osc_re=1;c->step_re=cos(phase);c->step_im=sin(phase);
        /* Causal power tracking must have a nonnegative step response. The
         * offline Butterworth filtfilt can use future samples; applying one
         * Butterworth pass causally rings below zero at high head imbalance
         * and fabricates enormous logarithmic dominance spikes. */
        c->power_filter=beta_lowpass(BETA_IQ_RATE,1500,.5);
    }
    for(int channel=0;channel<2;channel++) {
        /* Causal Butterworth-5 at the reference's 20 kHz cutoff. */
        d->audio_low[channel][0]=beta_first_lowpass(BETA_IQ_RATE,20000);
        d->audio_low[channel][1]=beta_lowpass(BETA_IQ_RATE,20000,.6180339887498948);
        d->audio_low[channel][2]=beta_lowpass(BETA_IQ_RATE,20000,1.618033988749895);
        d->audio_high[channel]=beta_highpass(BETA_IQ_RATE,15,.7071067811865475);
    }
    beta_audio_reset(&d->audio);
}

void gui_beta_hifi_process(gui_beta_hifi_decoder_t *d,const float *samples,
                           size_t count,uint64_t first_sample,
                           gui_beta_hifi_pcm_callback_t callback,void *context) {
    if(!d || !samples || !count) return;
    if(d->have_position && first_sample!=d->expected_sample) gui_beta_hifi_reset(d);
    d->have_position=true;
    for(size_t i=0;i<count;i++) {
        float sample=samples[i];
        if(!beta_finite_float(sample)) {sample=0;beta_unlock(d,"Invalid RF sample; muted");}
        int pos=d->rf_pos;
        d->rf_history[pos]=d->rf_history[pos+BETA_RF_TAPS]=sample;
        d->rf_pos=(pos+1)%BETA_RF_TAPS;
        d->status.input_samples++;
        if(++d->rf_phase==4) {
            d->rf_phase=0;
            float reduced=beta_dot(d->rf_history+d->rf_pos,d->rf_taps,BETA_RF_TAPS);
            d->input_power += .00002*((double)reduced*reduced-d->input_power);
            beta_reduced_sample(d,reduced,first_sample+i+1,callback,context);
        }
    }
    d->expected_sample=first_sample+count;
    beta_emit(d,d->expected_sample,callback,context);
}

void gui_beta_hifi_get_status(const gui_beta_hifi_decoder_t *d,
                              gui_beta_hifi_status_t *status) {
    if(status) {
        if(d) *status=d->status;
        else memset(status,0,sizeof(*status));
    }
}

bool gui_beta_hifi_set_iq_kernel(gui_beta_hifi_decoder_t *d,
                                gui_beta_hifi_iq_kernel_t kernel) {
    if(!d || d->status.input_samples) return false;
    return beta_select_iq_kernel(d,kernel);
}

gui_beta_hifi_iq_kernel_t gui_beta_hifi_get_iq_kernel(
    const gui_beta_hifi_decoder_t *d) {
    return d ? d->active_kernel : GUI_BETA_HIFI_IQ_AUTO;
}
