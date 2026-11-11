/* Private head qualification state; no RF fixture or audio device required. */
#include <math.h>
#include <stdint.h>

/* Count the actual production phase evaluations without adding a production
 * hook. The legacy oracle below calls libm directly after this macro ends. */
static uint64_t phase_evaluations;
static double counted_atan2(double y,double x) {
    phase_evaluations++;
    return atan2(y,x);
}
#define atan2 counted_atan2
#include "../misrc_gui/signal/gui_beta_hifi.c"
#undef atan2
#include <assert.h>
#include <limits.h>

#ifdef NDEBUG
#error "gui_beta_hifi_state_test requires enabled assertions; remove NDEBUG"
#endif

static void observe(gui_beta_hifi_decoder_t *d,int candidate) {
    double real[4]={1,1,1,1},imaginary[4]={0,0,0,0},power[4]={1,1,1,1};
    for(int branch=0;branch<4;branch++) {
        if(candidate && (branch&1)==(candidate>0 ? 0 : 1)) power[branch]=100;
    }
    beta_iq(d,real,imaginary,power,d->iq_count*40,NULL,NULL);
}

static void test_near_integer_limit(void) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();
    assert(d);
    d->head=d->candidate_head=1;d->have_head=true;
    d->candidate_run=INT_MAX-1;
    /* Volatile reads observe the stored counter even under optimized builds. */
    const volatile int *run=&d->candidate_run;
    for(int i=0;i<4;i++) {
        int previous=*run;
        observe(d,1);
        assert(*run>=previous && *run>=75);
        assert(d->head==1 && d->have_head && !d->status.locked);
        assert(d->status.head_switches==0);
    }
    gui_beta_hifi_destroy(d);
}

static void test_acquisition_and_fresh_candidate(void) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();
    assert(d);
    for(int i=0;i<74;i++) {
        observe(d,1);
        assert(!d->have_head && d->candidate_run==i+1);
    }
    observe(d,0);
    assert(!d->have_head && d->candidate_head==0 && d->candidate_run==0);
    for(int i=0;i<74;i++) {
        observe(d,1);
        assert(!d->have_head && d->candidate_run==i+1);
    }
    observe(d,1);
    assert(d->have_head && d->head==1 && d->candidate_run==75);
    uint64_t first_switch=d->last_switch;
    for(int i=0;i<10000;i++) {
        observe(d,1);
        assert(d->head==1 && d->candidate_run==75);
    }
    assert(d->last_switch==first_switch && d->status.head_switches==0);
    for(int i=0;i<74;i++) {
        observe(d,-1);
        assert(d->head==1 && d->candidate_run==i+1);
    }
    observe(d,-1);
    assert(d->head==-1 && d->candidate_run==75);
    assert(d->status.head_switches==1 && !d->status.locked);
    gui_beta_hifi_destroy(d);
}

static void test_half_field_holdoff(void) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();
    assert(d);
    for(int i=0;i<75;i++) observe(d,1);
    uint64_t first_switch=d->last_switch;
    uint64_t holdoff=(uint64_t)(.5*BETA_FIELD_IQ);
    for(int i=0;i<75;i++) observe(d,-1);
    assert(d->candidate_run==75 && d->head==1);
    while(d->iq_count<=first_switch+holdoff) {
        observe(d,-1);
        assert(d->head==1 && d->status.head_switches==0);
    }
    observe(d,-1);
    assert(d->head==-1 && d->status.head_switches==1);
    assert(d->last_switch==first_switch+holdoff+1);
    gui_beta_hifi_destroy(d);
}

static void test_unlock_and_head_recovery(void) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();
    assert(d);
    for(int i=0;i<75;i++) observe(d,1);
    /* Seed only the calibrated lock; exercise the real cadence-loss path. */
    d->status.locked=true;
    d->last_good_switch=d->last_switch;
    uint64_t last_good=d->last_good_switch;
    while(d->iq_count<=last_good+25000) {
        observe(d,1);
        assert(d->status.locked);
    }
    observe(d,1);
    assert(!d->status.locked && d->gate==0);
    assert(d->head==1 && d->candidate_run==75);
    observe(d,0);
    assert(d->candidate_head==0 && d->candidate_run==0);
    for(int i=0;i<74;i++) {
        observe(d,-1);
        assert(d->head==1 && !d->status.locked);
    }
    observe(d,-1);
    assert(d->head==-1 && d->candidate_run==75 && !d->status.locked);
    gui_beta_hifi_destroy(d);
}

/* A regular field fixture whose final edge is at a chosen sample. The 8342
 * IQ-sample interval approximates one NTSC field; it does not derive its edge
 * timestamp or frequency validity from the implementation under test. */
static int calibration_head(uint64_t sample,uint64_t edge) {
    uint64_t earlier_fields=sample<edge ? (edge-sample+8341)/8342 : 0;
    return (earlier_fields&1) ? -1 : 1;
}

static void feed_calibration_window(gui_beta_hifi_decoder_t *d,uint64_t count,
                                     uint64_t edge_age) {
    assert(count>edge_age);
    uint64_t edge=count-1-edge_age;
    for(uint64_t n=0;n<count;n++) observe(d,calibration_head(n,edge));
}

static void require_observed_edge(const gui_beta_hifi_decoder_t *d,uint64_t edge,
                                   const char *stage) {
    if(!d->status.locked || d->last_good_switch!=edge || d->last_switch!=edge)
        fprintf(stderr,"%s: locked=%d IQ=%llu edge=%llu good=%llu switch=%llu "
                       "cal=%d/%d bad=%d reason=%s\n",stage,d->status.locked,
                (unsigned long long)d->iq_count,(unsigned long long)edge,
                (unsigned long long)d->last_good_switch,(unsigned long long)d->last_switch,
                d->cal_count,d->cal_next,d->bad_count,d->status.reason);
    assert(d->status.locked);
    assert(d->last_good_switch==edge && d->last_switch==edge);
    assert(d->head==1 && d->have_head);
}

static void test_calibrated_edge_acquisition_and_relock(void) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();assert(d);
    feed_calibration_window(d,125000,0);
    require_observed_edge(d,124999,"cold newest edge");
    assert(d->cal_count==250 && d->cal_next==250);
    /* Continued calibrated lock must preserve the last observed edge. Exactly
     * 50000 us is allowed; only the following IQ sample is real cadence loss.
     * The rolling 300-point calibration also runs while this head is held. */
    while(d->iq_count<=124999+25000) {
        observe(d,1);
        require_observed_edge(d,124999,"cadence boundary hold");
    }
    observe(d,1);
    assert(!d->status.locked && d->gate==0);
    assert(strcmp(d->status.reason,"Beta HiFi head cadence lost; muted")==0);
    assert(d->calibration_iq_start==150001 && d->cal_count==0 && d->cal_next==0);
    assert(d->cal_phase==1);
    /* With a nonzero sampling phase the 250th point is one sample short of
     * the fresh 125000-IQ minimum. The 300th point is the first valid attempt. */
    uint64_t edge=149998;
    for(uint64_t n=0;n<=edge;n++) {
        observe(d,calibration_head(n,edge));
        if(n<edge) assert(!d->status.locked);
    }
    require_observed_edge(d,299999,"newest edge after true cadence loss");
    assert(d->cal_count==300 && d->cal_next==0);
    uint64_t resets=d->status.resets;
    gui_beta_hifi_reset(d);
    assert(d->status.resets==resets+1 && d->iq_count==0 && !d->status.locked);
    feed_calibration_window(d,125000,0);
    require_observed_edge(d,124999,"newest edge after explicit reset");
    gui_beta_hifi_reset(d);
    feed_calibration_window(d,125000,500);
    require_observed_edge(d,124499,"one observation old edge");
    puts("calibration edge acquisition/relock/reset: newest and prior timestamps, "
         "25000-IQ boundary preserved");
    gui_beta_hifi_destroy(d);
}

static void test_calibration_switch_cold(void) {
    const uint64_t ages[]={73,74,75};
    for(size_t at=0;at<sizeof(ages)/sizeof(ages[0]);at++) {
        gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();assert(d);
        uint64_t edge=124999-ages[at];
        for(uint64_t n=0;n<124999;n++) observe(d,calibration_head(n,edge));
        assert(!d->status.locked && d->bad_count==10000);
        uint64_t switches=d->status.head_switches;
        observe(d,1);
        require_observed_edge(d,124999,"cold switch calibration");
        bool switched_now=ages[at]==74;
        assert(d->status.head_switches==switches+(switched_now ? 1u : 0u));
        assert(d->bad_count==(switched_now ? 1 : 0));
        assert(d->cal_count==250 && d->cal_next==250);
        observe(d,1);
        assert(d->status.locked && d->bad_count==0);
        printf("cold calibration edge age %llu: switch mask=%d, fresh invalid run=%d then0\n",
               (unsigned long long)ages[at],switched_now,switched_now ? 1 : 0);
        gui_beta_hifi_destroy(d);
    }
}

static void test_calibration_switch_relock(void) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();assert(d);
    feed_calibration_window(d,125000,500);
    require_observed_edge(d,124499,"relock fixture initial acquisition");
    while(d->status.locked) observe(d,1);
    assert(strcmp(d->status.reason,"Beta HiFi head cadence lost; muted")==0);
    assert(d->gate==0 && d->cal_count==0 && d->cal_next==0);
    assert(d->calibration_iq_start==d->iq_count && d->cal_phase==1);
    uint64_t count=150000-(uint64_t)d->cal_phase;
    uint64_t edge=count-1-74;
    for(uint64_t n=0;n<count-1;n++) observe(d,calibration_head(n,edge));
    assert(!d->status.locked && d->bad_count==10000);
    uint64_t switches=d->status.head_switches;
    uint64_t observed_edge=d->iq_count;
    observe(d,1);
    require_observed_edge(d,observed_edge,"relock switch calibration");
    assert(d->status.head_switches==switches+1);
    assert(d->bad_count==1 && d->cal_count==300 && d->cal_next==0);
    observe(d,1);
    assert(d->status.locked && d->bad_count==0);
    puts("relock after true cadence loss: same-sample switch invalid run1 then0");
    gui_beta_hifi_destroy(d);
}

static void observe_carrier_loss(gui_beta_hifi_decoder_t *d) {
    double real[4]={0,0,0,0},imaginary[4]={0,0,0,0};
    double power[4]={1e-30,1e-30,1e-30,1e-30};
    beta_iq(d,real,imaginary,power,d->iq_count*40,NULL,NULL);
}

static void test_locked_carrier_loss_threshold(void) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();assert(d);
    feed_calibration_window(d,125000,500);
    assert(d->status.locked && d->bad_count==0);
    for(int n=1;n<5000;n++) {
        observe_carrier_loss(d);
        assert(d->status.locked && d->bad_count==n);
    }
    /* A successful refresh while already locked must not erase the genuine
     * consecutive-invalid run. There is still ample alternating recent RF
     * history to accept calibration, and cadence has not timed out. */
    beta_calibrate(d);
    assert(d->status.locked && d->bad_count==4999);
    observe_carrier_loss(d);
    assert(!d->status.locked && d->bad_count==5000 && d->gate==0);
    assert(strcmp(d->status.reason,"Beta HiFi carrier RF lost; muted")==0);
    puts("locked carrier loss:4999 invalid IQ retained across calibration,5000 mutes");
    assert(d->calibration_iq_start==d->iq_count && d->cal_count==0 && d->cal_next==0);
    uint64_t count=d->cal_phase ? 150000-(uint64_t)d->cal_phase : 125000;
    uint64_t edge=count-1-74;
    for(uint64_t n=0;n<count-1;n++) observe(d,calibration_head(n,edge));
    assert(!d->status.locked && d->bad_count==10000);
    uint64_t switches=d->status.head_switches;
    uint64_t observed_edge=d->iq_count;
    observe(d,1);
    require_observed_edge(d,observed_edge,"carrier-loss switch calibration");
    assert(d->status.head_switches==switches+1 && d->bad_count==1);
    observe(d,1);
    assert(d->status.locked && d->bad_count==0);
    puts("relock after true carrier loss: same-sample switch invalid run1 then0");
    gui_beta_hifi_destroy(d);
}


typedef enum {
    CAL_WINDOW_VALID,
    CAL_WINDOW_LOW_SPAN,
    CAL_WINDOW_SEVEN_SWITCHES,
    CAL_WINDOW_MISSING_REFERENCE,
    CAL_WINDOW_FEW_ACTIVE_POINTS
} calibration_window_t;

/* Direct 300-point state fixtures isolate refresh acceptance from RF filters.
 * Their 17 ms edges approximate the NTSC field period independently. Each bad
 * candidate proposes a new midpoint and four new powers, so partial writes or
 * unwanted resets cannot pass a cache-preservation assertion. */
static void set_refresh_window(gui_beta_hifi_decoder_t *d,
                                calibration_window_t kind,double midpoint,
                                double active_power) {
    d->iq_count=250000;d->cal_count=BETA_CAL_POINTS;d->cal_next=50;
    d->calibration_iq_start=0;d->input_power=1;
    for(int i=0;i<BETA_CAL_POINTS;i++) {
        int head=((i/17)&1) ? -1 : 1;
        if(kind==CAL_WINDOW_SEVEN_SWITCHES) {
            /* Eight 17-point runs give exactly seven regular switches;
             * both active groups still have 68 points and valid powers. */
            head=i<164 ? 0 : ((((i-164)/17)&1) ? -1 : 1);
        } else if(kind==CAL_WINDOW_FEW_ACTIVE_POINTS) {
            /* Ten regular edges, but only 35 observations of head B. Both
             * dominance tails still occupy more than ten percent. */
            if(i>=187 || (head<0 && i%17>=7)) head=0;
        }
        int slot=(50+i)%BETA_CAL_POINTS;
        double half_span=kind==CAL_WINDOW_LOW_SPAN ? 4.75 : 20;
        d->calibration[slot][4]=midpoint+head*half_span;
        for(int branch=0;branch<4;branch++) {
            bool active=head && ((branch&1)==(head>0 ? 0 : 1));
            double power=active ? active_power+branch*10 : 1;
            if(kind==CAL_WINDOW_MISSING_REFERENCE && branch==3) power=1e-30;
            d->calibration[slot][branch]=power;
        }
    }
}

static void test_invalid_refresh_preserves_verified_calibration(void) {
    const calibration_window_t invalid[]={CAL_WINDOW_LOW_SPAN,
        CAL_WINDOW_SEVEN_SWITCHES,CAL_WINDOW_MISSING_REFERENCE,
        CAL_WINDOW_FEW_ACTIVE_POINTS};
    const char *reasons[]={"Waiting for alternating NTSC Beta HiFi carriers",
        "Waiting for regular NTSC Beta HiFi head cadence",
        "Missing NTSC Beta HiFi carrier","Missing NTSC Beta HiFi carrier"};
    for(size_t at=0;at<sizeof(invalid)/sizeof(invalid[0]);at++) {
        gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();assert(d);
        set_refresh_window(d,CAL_WINDOW_VALID,4,100);
        beta_calibrate(d);assert(d->status.locked && d->midpoint==4);
        for(int branch=0;branch<4;branch++)
            assert(d->carrier[branch].reference==100+branch*10);
        d->gate=.625;d->audio.gain=2.5;d->bad_count=4999;
        d->last_switch=d->last_good_switch=d->iq_count-1000;
        set_refresh_window(d,invalid[at],17,1000);
        unsigned char verified[sizeof(*d)];memcpy(verified,d,sizeof(*d));
        beta_calibrate(d);
        /* Includes gate, status/reason, recent-loss counters, head times,
         * audio filters, circular-window cursors and every cached reference. */
        assert(memcmp(d,verified,sizeof(*d))==0);

        gui_beta_hifi_decoder_t *cold=gui_beta_hifi_create();assert(cold);
        set_refresh_window(cold,invalid[at],17,1000);
        beta_calibrate(cold);
        assert(!cold->status.locked && cold->gate==0 && cold->midpoint==0);
        assert(strcmp(cold->status.reason,reasons[at])==0);
        for(int branch=0;branch<4;branch++) assert(cold->carrier[branch].reference==0);
        assert(cold->cal_count==BETA_CAL_POINTS && cold->cal_next==50);
        gui_beta_hifi_destroy(cold);

        /* A rejected refresh cannot reset the 10 ms real carrier-loss run. */
        observe_carrier_loss(d);
        assert(!d->status.locked && d->bad_count==5000 && d->gate==0);
        assert(strcmp(d->status.reason,"Beta HiFi carrier RF lost; muted")==0);
        gui_beta_hifi_destroy(d);
    }
    puts("invalid refresh:9.5dB,seven switches,missing/undersampled carriers "
         "retain verified state; identical cold windows reject; real loss mutes");
}

static void test_valid_refresh_commits_complete_candidate(void) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();assert(d);
    set_refresh_window(d,CAL_WINDOW_VALID,4,100);beta_calibrate(d);
    assert(d->status.locked);
    d->gate=.625;d->audio.gain=2.5;d->bad_count=4999;
    d->last_switch=d->last_good_switch=d->iq_count-1000;
    set_refresh_window(d,CAL_WINDOW_VALID,17,1000);
    beta_calibrate(d);
    assert(d->status.locked && d->midpoint==17);
    for(int branch=0;branch<4;branch++)
        assert(d->carrier[branch].reference==1000+branch*10);
    assert(d->gate==.625 && d->audio.gain==2.5 && d->bad_count==4999);
    assert(d->last_switch==249000 && d->last_good_switch==249000);
    assert(d->cal_count==BETA_CAL_POINTS && d->cal_next==50);
    puts("valid refresh commits midpoint/all references without resetting live loss state");
    gui_beta_hifi_destroy(d);
}

static void test_wrapped_calibration_edge_timestamp(void) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();assert(d);
    d->iq_count=200000;d->cal_count=BETA_CAL_POINTS;d->cal_next=50;
    /* This direct calibration fixture isolates circular-window ordering:
     * chronological oldest point is slot50, newest is slot49. */
    for(int i=0;i<BETA_CAL_POINTS;i++) {
        int slot=(50+i)%BETA_CAL_POINTS;
        int head=calibration_head((uint64_t)i*500,(BETA_CAL_POINTS-1)*500);
        for(int branch=0;branch<4;branch++)
            d->calibration[slot][branch]=((branch&1)==(head>0 ? 0 : 1)) ? 100 : 1;
        d->calibration[slot][4]=head>0 ? 20 : -20;
    }
    beta_calibrate(d);
    require_observed_edge(d,199999,"wrapped newest edge");
    observe(d,1);
    require_observed_edge(d,199999,"wrapped edge next IQ");
    puts("wrapped 300-point calibration: current edge timestamps remain in the past");
    gui_beta_hifi_destroy(d);
}

/* Frozen SuperBeta preview2 all-four phase consumer. Keep this independent
 * while selected-pair evaluation is compared with the prior state ordering. */
static void legacy_beta_iq(gui_beta_hifi_decoder_t *d,double frequency[4],double power[4],
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



typedef struct {
    float stereo[BETA_PCM_BATCH*2];
    size_t frames,total_frames;
    uint64_t end_sample,events,nonzero;
    gui_beta_hifi_status_t status;
    bool pending;
} phase_capture_t;

static void capture_selected_pair(void *context,const float *stereo,size_t frames,
                                   uint64_t end_sample,
                                   const gui_beta_hifi_status_t *status) {
    phase_capture_t *c=context;
    assert(!c->pending && frames<=BETA_PCM_BATCH);
    memcpy(c->stereo,stereo,frames*2*sizeof(float));
    c->frames=frames;c->end_sample=end_sample;c->status=*status;c->pending=true;
    c->events++;c->total_frames+=frames;
    for(size_t i=0;i<frames*2;i++) if(stereo[i]!=0) c->nonzero++;
}

static void compare_all_four_pair(void *context,const float *stereo,size_t frames,
                                  uint64_t end_sample,
                                  const gui_beta_hifi_status_t *status) {
    phase_capture_t *c=context;
    assert(c->pending && c->frames==frames && c->end_sample==end_sample);
    assert(memcmp(c->stereo,stereo,frames*2*sizeof(float))==0);
    assert(memcmp(&c->status,status,sizeof(*status))==0);
    c->pending=false;
}

/* This is an IQ-level phase/state regression, not another RF reception test.
 * Independently generated rotating IQ vectors have distinct channel/head
 * frequencies, alternating powers, a one-channel short dropout, whole-RF
 * loss, cadence loss, and reacquisition. The frozen old routine calculates
 * every carrier before it makes the head/calibration decision. Comparing it
 * catches a stale head, missing inactive phase history, or delayed first IQ.
 */
static void test_selected_phase_matches_all_four_oracle(void) {
    gui_beta_hifi_decoder_t *selected=gui_beta_hifi_create();
    gui_beta_hifi_decoder_t *all_four=gui_beta_hifi_create();
    assert(selected && all_four);
    phase_capture_t capture={0};
    double phase[4]={.2,.7,1.3,2.1};
    bool saw_lock=false,saw_loss=false,saw_recovery=false,saw_calibration_head=false;
    uint64_t calls=0,samples=0;
    phase_evaluations=0;
    for(int epoch=0;epoch<2;epoch++) {
        if(epoch) {
            gui_beta_hifi_reset(selected);gui_beta_hifi_reset(all_four);
            assert(memcmp(selected,all_four,sizeof(*selected))==0);
        }
        /* Both uncalibrated dominance states are positive when head B is
         * attenuated. Calibration must learn the midpoint and immediately
         * select B, before the provisional detector has ever selected it. */
        for(uint64_t n=0;n<480000;n++) {
            bool head_a=(((n+1000)*60000/(UINT64_C(500000)*1001))&1)==0;
            bool rf_loss=n>=160000 && n<210000;
            bool cadence_loss=n>=395000 && n<435000;
            if(cadence_loss) head_a=true;
            double real[4],imaginary[4],power[4],frequency[4];
            for(int branch=0;branch<4;branch++) {
                double tone=branch<2 ? 700 : 1900;
                double deviation=(branch&1) ? -23000 : 17000;
                phase[branch]+=2*BETA_PI*(deviation+9000*sin(2*BETA_PI*tone*n/BETA_IQ_RATE))
                                        /BETA_IQ_RATE;
                double amplitude=head_a==((branch&1)==0) ? 1 : .01;
                if(branch&1) amplitude/=150;
                if(rf_loss || (branch==2 && n>=150010 && n<150030)) amplitude=0;
                real[branch]=amplitude*cos(phase[branch]);
                imaginary[branch]=amplitude*sin(phase[branch]);
                power[branch]=fmax(1e-30,amplitude*amplitude);
                beta_carrier_t *old=&all_four->carrier[branch];
                frequency[branch]=old->previous ? atan2(
                    imaginary[branch]*old->prev_re-real[branch]*old->prev_im,
                    real[branch]*old->prev_re+imaginary[branch]*old->prev_im)
                    *(BETA_IQ_RATE/(2*BETA_PI)) : 0;
                old->prev_re=real[branch];old->prev_im=imaginary[branch];old->previous=true;
            }
            bool was_locked=selected->status.locked;
            int old_head=selected->head;
            uint64_t before=phase_evaluations;
            beta_iq(selected,real,imaginary,power,(n+1)*40,capture_selected_pair,&capture);
            legacy_beta_iq(all_four,frequency,power,(n+1)*40,compare_all_four_pair,&capture);
            assert(!capture.pending);
            assert(phase_evaluations-before==(n ? 2u : 0u));
            if(selected->status.locked) {
                if(saw_loss) saw_recovery=true;
                saw_lock=true;
            } else if(was_locked) saw_loss=true;
            if(!was_locked && selected->status.locked && selected->head!=old_head)
                saw_calibration_head=true;
            /* Compare every stored history/filter/control bit at transitions
             * and periodic checkpoints, as well as every emitted PCM batch. */
            if(n%997==0 || was_locked!=selected->status.locked || old_head!=selected->head)
                assert(memcmp(selected,all_four,sizeof(*selected))==0);
            samples++;
        }
        beta_emit(selected,480000*40,capture_selected_pair,&capture);
        beta_emit(all_four,480000*40,compare_all_four_pair,&capture);
        assert(!capture.pending && memcmp(selected,all_four,sizeof(*selected))==0);
        calls+=480000-1;
    }
    if(!(saw_lock && saw_loss && saw_recovery && saw_calibration_head))
        fprintf(stderr,"oracle coverage: lock=%d loss=%d recovery=%d calibration-head=%d\n",
                saw_lock,saw_loss,saw_recovery,saw_calibration_head);
    assert(saw_lock && saw_loss && saw_recovery && saw_calibration_head);
    assert(capture.nonzero>10000 && capture.total_frames>90000);
    assert(phase_evaluations==2*calls);
    printf("selected-pair vs frozen all-four oracle: %llu IQ samples, %zu PCM frames, "
           "%llu bit-identical PCM/status batches; %llu atan2 calls (half legacy)\n",
           (unsigned long long)samples,capture.total_frames,
           (unsigned long long)capture.events,(unsigned long long)phase_evaluations);
    gui_beta_hifi_destroy(selected);gui_beta_hifi_destroy(all_four);
}

int main(int argc,char **argv) {
    if(argc==2) {
        if(strcmp(argv[1],"--calibration-switch-cold")==0) test_calibration_switch_cold();
        else if(strcmp(argv[1],"--calibration-switch-relock")==0) test_calibration_switch_relock();
        else if(strcmp(argv[1],"--locked-carrier-loss")==0) test_locked_carrier_loss_threshold();
        else if(strcmp(argv[1],"--calibration-refresh")==0) {
            test_invalid_refresh_preserves_verified_calibration();
            test_valid_refresh_commits_complete_candidate();
        }
        else return 2;
        puts("gui_beta_hifi_state_test: scoped OK");
        return 0;
    }
    if(argc!=1) return 2;
    test_near_integer_limit();
    test_acquisition_and_fresh_candidate();
    test_half_field_holdoff();
    test_unlock_and_head_recovery();
    test_calibrated_edge_acquisition_and_relock();
    test_calibration_switch_cold();
    test_calibration_switch_relock();
    test_locked_carrier_loss_threshold();
    test_invalid_refresh_preserves_verified_calibration();
    test_valid_refresh_commits_complete_candidate();
    test_wrapped_calibration_edge_timestamp();
    test_selected_phase_matches_all_four_oracle();
    puts("gui_beta_hifi_state_test: OK");
    return 0;
}
