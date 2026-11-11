/* Physical MESECAM RF contracts, not hardware/media qualification.
 * Independent studio SECAM FM (ITU-R BT.470-6 Table2) passes through a
 * causal record LF network and limiter, then an independent PAL converter
 * oscillator selects the difference band. No decoder coefficient, carrier
 * correction helper, decoded colour plane or PAL phase rule is an oracle.
 * MESECAM converter frequencies follow IEC60774-1 AnnexE /6.3.1 and the
 * original vhs-decode get_rfparams_mesecam_vhs reference implementation. */
#include "../misrc_gui/signal/gui_rf_preview.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_PI 3.14159265358979323846
#define SAMPLE_RATE 20000000.0
#define FIELD_COUNT 10

typedef struct {
    double chroma_amplitude, rf_gain, clock_scale, jitter_us, luma_offset;
    double line_chroma_noise, noise_amplitude, flat_level, first_eq_extra_us;
    double inband_noise_amplitude, colour_saturation, low_frequency_amplitude, active_carrier_scale;
    double off_noise_amplitude;
    int dr_first, swapped_heads, solid_colour, solid_start_field, fields, ident_end_field;
    int carrier_loss_mode, carrier_loss_start, carrier_loss_end;
    int widen_mask, missing_eq;bool repeat_parity, bad_cadence;
    bool gray, chroma_tone, rail_tone, field_ident, no_line_ident, spatial_pattern, phase_artifacts;
    bool bottle_picture_only;
} mesecam_source_t;

typedef struct {
    uint8_t *rgba;
    int width, height, frames;
    uint64_t last_end;
    bool ever_colour_locked;
    gui_rf_decode_status_t status;
} frames_t;

/* Moderate saturation avoids gamut clipping in the display transfer. */
static const double colours[8][3] = {
    {.8,.8,.8}, {.8,.8,.2}, {.2,.8,.8}, {.2,.8,.2},
    {.8,.2,.8}, {.8,.2,.2}, {.2,.2,.8}, {.2,.2,.2}
};

static double limit(double x, double lo, double hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

static void receive(void *context, const uint8_t *rgba, int width, int height,
                    uint64_t end, const gui_rf_decode_status_t *status) {
    frames_t *f=context;
    assert(width>0 && width<=GUI_RF_FRAME_MAX_WIDTH);
    assert(height>0 && height<=GUI_RF_FRAME_MAX_HEIGHT);
    assert(!f->frames || end>=f->last_end);
    memcpy(f->rgba,rgba,(size_t)width*height*4u);
    f->width=width;f->height=height;f->frames++;
    f->last_end=end;f->status=*status;
    f->ever_colour_locked|=status->color_locked;
}

/* Alternate 312/313 line slots total 625 lines per frame. The equalizing
 * trains alternate their half-line phase to retain the real 625/50 field
 * reference. A warped physical tape clock moves H timing and BOTH FM
 * carriers together; it does not edit any decoded output coordinates. */
static float *make_source(const mesecam_source_t *source, size_t *length) {
    const int fields=source->fields?source->fields:FIELD_COUNT;
    assert(fields>=FIELD_COUNT && fields<=64);
    int starts[65];starts[0]=0;
    for(int f=0;f<fields;f++) starts[f+1]=starts[f]
        +(source->bad_cadence?((f&1)?325:300):((f&1)?313:312));
    const int half_lines=starts[fields]*2;
    double *timeline=malloc((size_t)(half_lines+1)*sizeof(*timeline));
    assert(timeline);
    for(int h=0;h<=half_lines;h++)
        timeline[h]=h*32.0*source->clock_scale
            +source->jitter_us*sin(h*2.0*TEST_PI/23.0);
    *length=(size_t)floor(timeline[half_lines]*20.0);
    float *rf=malloc(*length*sizeof(*rf));assert(rf);

    /* Independent analog record networks. Luma uses the VHS record shelf
     * (1+s*400ns)/(1+s*100ns). MESECAM LF pre-correction has DC gain one,
     * HF gain three, zero85kHz/pole255kHz; an exact one-pole state integrates
     * it causally. The playback test must undo the record network itself. */
    const double k=2.0*SAMPLE_RATE;
    const double luma_b0=(1.0+k*400e-9)/(1.0+k*100e-9);
    const double luma_b1=(1.0-k*400e-9)/(1.0+k*100e-9);
    const double luma_a1=(1.0-k*100e-9)/(1.0+k*100e-9);
    double last_luma=0.0,last_pre=0.0,colour_state=0.0;
    double luma_phase=.31,chroma_phase=.79,converter_phase=.37,last_time=0.0;
    double noise_fast=0.0,noise_slow=0.0,band_noise_energy=0.0;
    double off_noise_energy=0.0;size_t off_noise_samples=0;
    double low_envelope=0.0,low_phase_a=.23,low_phase_b=1.13;
    const double noise_fast_step=1.0-exp(-2.0*TEST_PI*1350000.0/SAMPLE_RATE);
    const double noise_slow_step=1.0-exp(-2.0*TEST_PI*250000.0/SAMPLE_RATE);
    const double low_envelope_step=1.0-exp(-2.0*TEST_PI*1200.0/SAMPLE_RATE);
    unsigned noise_state=0x748ac91u;
    double studio_low[2]={1e9,1e9},studio_high[2]={0,0};
    size_t studio_count[2]={0,0},studio_clipped[2]={0,0};
    int half=0,field=0,last_field=-1;
    for(size_t n=0;n<*length;n++) {
        const double real_us=n/20.0;
        while(half+1<half_lines && timeline[half+1]<=real_us) half++;
        double fraction=(real_us-timeline[half])/(timeline[half+1]-timeline[half]);
        double ideal_us=(half+fraction)*32.0;
        while(field<fields-1 && ideal_us>=starts[field+1]*64.0) field++;
        const double local=ideal_us-starts[field]*64.0;
        const int line=(int)floor(local/64.0);
        const double x=local-line*64.0;
        const double eq_start=source->repeat_parity?32.0:(field&1)?64.0:32.0;
        const double broad_start=eq_start+160.0;
        const double eq2_start=broad_start+160.0;
        const double vertical_end=eq2_start+160.0;
        bool sync;
        if(local<eq_start || local>=vertical_end) sync=x<4.7;
        else {
            double start=local<broad_start?eq_start:local<eq2_start?broad_start:eq2_start;
            double width=local>=broad_start && local<eq2_start?27.3:2.35;
            int before_broad=(int)floor((local-eq_start)/32.0);
            if(local>=eq_start && local<broad_start) {
                if(before_broad==0 && (source->widen_mask&(1<<(field&1))))
                    width+=source->first_eq_extra_us;
                if(source->missing_eq && before_broad==source->missing_eq-1)width=0.0;
            }
            sync=fmod(local-start,32.0)<width;
        }
        /* The VBI extends through line22 even after the EQ/broad train.
         * Ordinary picture may never masquerade as field identification. */
        const bool active=line>=22 && x>=10.5 && x<62.5;
        double r=0.0,g=0.0,b=0.0;
        if(active) {
            int bar=(int)((x-10.5)/52.0*8.0);if(bar>7)bar=7;
            r=colours[bar][0];g=colours[bar][1];b=colours[bar][2];
            /* Gray leftmost bar differs between physical fields, allowing
             * a final-RGBA assertion against accidental bob/field repeats. */
            if(source->gray || bar==0) r=g=b=(field&1)?.30:.70;
            if(source->chroma_tone || source->rail_tone) {
                /* Independent 85kHz chroma modulation with mathematically
                 * constant physical luma; no decoder phase/delay is used. */
                double angle=2.0*TEST_PI*(source->rail_tone?150000.0:85000.0)*(x-10.5)*1e-6;
                r=.5+(source->rail_tone?.49:.22)*sin(angle);
                b=.5+(source->rail_tone?.49:.16)*cos(angle);
                g=(.5-.299*r-.114*b)/.587;
                assert(r>=0 && r<=1 && g>=0 && g<=1 && b>=0 && b<=1);
            }
            if(source->solid_colour && field>=source->solid_start_field) {
                int colour=3+source->solid_colour;assert(colour>=4 && colour<=6);
                r=colours[colour][0];g=colours[colour][1];b=colours[colour][2];
            }
            if(source->spatial_pattern) {
                /* Pure vertical colour detail with exactly constant Y in
                 * each physical field. Opposite field colours and luma
                 * make blending across fields independently observable. */
                double field_y=(field&1)?.35:.65,sign=(field&1)?-1.0:1.0;
                double red=line<128?-.16:.16;
                double blue=line<80?-.16:line<96?-.16+.02*(line-80):.16;
                if(line>=160) blue=line<176?.16-.02*(line-160):-.16;
                r=field_y+sign*red;b=field_y+sign*blue;
                g=(field_y-.299*r-.114*b)/.587;
            }
        }
        if(active && source->flat_level>=0.0)r=g=b=source->flat_level;
        if(active && source->colour_saturation>0.0) {
            double gray=.299*r+.587*g+.114*b;
            r=gray+source->colour_saturation*(r-gray);
            g=gray+source->colour_saturation*(g-gray);
            b=gray+source->colour_saturation*(b-gray);
        }
        const double y=.299*r+.587*g+.114*b;
        const double raw_luma=sync?-300000.0:700000.0*y;
        const double pre_luma=luma_b0*raw_luma+luma_b1*last_luma-luma_a1*last_pre;
        last_luma=raw_luma;last_pre=pre_luma;
        const double dt=(ideal_us-last_time)*1e-6;
        last_time=ideal_us;
        double luma_hz=limit(4100000.0+source->luma_offset+pre_luma,2400000.0,6500000.0);
        luma_phase+=2.0*TEST_PI*luma_hz*dt;

        /* Generate studio-domain D'R and D'B before heterodyne conversion.
         * LF pre-emphasis keeps its studio temporal rate. The real mixer
         * subtracts independent phases; it does not divide FM deviation. */
        const bool dr=((starts[field]+line+source->dr_first)&1)!=0;
        double d=active?(dr?-1.902*(r-y):1.505*(b-y)):0.0;
        if(source->spatial_pattern) {
            if(active) {
                /* A deterministic line-correlated FM frequency error is
                 * recorded BEFORE the analog MESECAM pre-correction. It
                 * changes chroma only; physical luma is untouched. */
                double noise=source->line_chroma_noise*((line/2)&1?1.0:-1.0);
                d+=dr?-1.902*noise:1.505*noise;
            } else if(line==20 || line==21) {
                /* Deliberate VBI colour interferer, opposite the first
                 * active colour. This is a boundary stress signal, not
                 * an invented broadcast identification reference. */
                double colour=(field&1)?-.16:.16;
                d=dr?-1.902*colour:1.505*colour;
            }
        }
        /* Documented field-identification option: MESECAM lines7..15 carry
         * the alternate Dr-up/Db-down trapezoids (BT.470-6 Table2.18).
         * No arbitrary rest carriers are invented in the remaining VBI.
         * Field2's half-line origin is represented by its alternate EQ
         * phase, as for the physical luma sync oracle above. */
        const bool bottle=source->field_ident && line>=6 && line<=14
            && (!source->ident_end_field || field<source->ident_end_field);
        const double decay=exp(-2.0*TEST_PI*255000.0*dt);
        colour_state=decay*colour_state+(1.0-decay)*d;
        double pre_d=3.0*d-2.0*colour_state;
        /* The field-ident sawtooth is inserted AFTER LF pre-correction
         * (Philips TDA8505 p7/Fig4), starting at the horizontal picture
         * window. Its 15/18us ramp is independent of picture and receiver. */
        if(bottle)pre_d=dr?1.25*limit((x-10.5)/15.0,0.0,1.0)
            :-1.52*limit((x-10.5)/18.0,0.0,1.0);
        const double nominal=dr?4406250.0:4250000.0;
        const double deviation=dr?280000.0:230000.0;
        const double studio_hz=limit(nominal+deviation*pre_d,3900000.0,4756250.0);
        if(source->rail_tone && active) {
            int component=dr?1:0;double unclipped=nominal+deviation*pre_d;
            studio_low[component]=fmin(studio_low[component],studio_hz);
            studio_high[component]=fmax(studio_high[component],studio_hz);
            studio_count[component]++;
            if(unclipped<3900000.0 || unclipped>4756250.0)studio_clipped[component]++;
        }
        if(field!=last_field) {
            /* Independent head phases: reversing heads must not swap Dr/Db
             * or require PAL's line-by-line quadrature correction. */
            int head=(field&1)^source->swapped_heads;
            chroma_phase+=head?2.17:.71;last_field=field;
        }
        chroma_phase+=2.0*TEST_PI*studio_hz*dt;
        converter_phase+=2.0*TEST_PI*5060571.875*dt;
        /* Unlike a /4 counter, heterodyne recording retains SECAM's AM
         * bell shaping. Form that physical envelope before the mixer. */
        double normalized=studio_hz/4286000.0;
        double bell_f=normalized-1.0/normalized;
        double bell_gain=sqrt((1.0+256.0*bell_f*bell_f)/(1.0+1.5876*bell_f*bell_f));
        /* H identification is the undeviated carrier before picture. The
         * no-ID negative removes this AND the optional field bottles.
         * Remaining VBI lines carry no chroma per BT.470-6 Table2.17. */
        bool line_ident=!source->no_line_ident && (!source->ident_end_field || field<source->ident_end_field);
        bool carrier=(line>=22 && x>=(line_ident?5.6:10.5) && x<62.5)
            || (bottle && x>=(source->bottle_picture_only?10.5:5.6) && x<62.5 && !sync);
        if(source->spatial_pattern && (line==20 || line==21))
            carrier=x>=5.6 && x<62.5 && !sync;
        if(source->spatial_pattern && line==208)carrier=false;
        const bool interrupted=source->carrier_loss_mode && field>=source->carrier_loss_start
            && field<source->carrier_loss_end;
        if(interrupted) {
            if(source->carrier_loss_mode==1 || source->carrier_loss_mode==6)carrier=false;
            if(source->carrier_loss_mode==2 && active)carrier=false;
            if(source->carrier_loss_mode==3 && line>=120 && line<128)carrier=false;
            if(source->carrier_loss_mode==4 && active && x>=29.0 && x<43.0)carrier=false;
            if(source->carrier_loss_mode==5 && active && x>=12.0 && x<43.0)carrier=false;
        }
        double envelope=carrier?1.0:0.0;
        /* Tape AM loss may affect picture while H references remain loud.
         * Preserve the original instantaneous FM and physical RGB values. */
        if(active && source->active_carrier_scale>0.0)envelope*=source->active_carrier_scale;
        double phase_error=0.0,click=0.0;
        if(source->phase_artifacts && active) {
            /* Channel damage to an otherwise undeviated physical carrier:
             * a complete phase excursion, a deep AM fade and a short RF
             * click. These are applied to the real RF waveform, not to
             * frequency samples, decoded planes or expected RGB values. */
            double centre=33.0+(line%5)*.55;
            double delta=(x-centre)/.30;
            phase_error=2.0*TEST_PI*exp(-delta*delta);
            if(line%9==0) {
                delta=(x-23.0)/.35;
                envelope*=1.0-.985*exp(-delta*delta);
            }
            if(line%7==0 && x>=47.0 && x<47.05)click=.9;
        }
        noise_state=1664525u*noise_state+1013904223u;
        double white=(noise_state>>8)*(1.0/8388608.0)-1.0;
        double noise=white*source->noise_amplitude;
        /* Independent physical channel noise: the difference between two
         * causal RC lowpasses places most power in the MESECAM tape band.
         * It has no undeviated reference tone or component modulation. */
        noise_fast+=noise_fast_step*(white-noise_fast);
        noise_slow+=noise_slow_step*(white-noise_slow);
        double noise_level=interrupted && source->off_noise_amplitude>0.0
            ?source->off_noise_amplitude:source->inband_noise_amplitude;
        double inband=noise_level*(noise_fast-noise_slow);
        band_noise_energy+=inband*inband;
        if(interrupted){off_noise_energy+=inband*inband;off_noise_samples++;}
        /* Independent RF ingress below the colour-under band. Random slow
         * AM on two unrelated real oscillators gives the receiver a channel
         * disturbance without changing physical Y, colour FM or H ident. */
        double low_interference=0.0;
        if(source->low_frequency_amplitude>0.0) {
            low_envelope+=low_envelope_step*(white-low_envelope);
            low_interference=source->low_frequency_amplitude
                *limit(1.0+6.0*low_envelope,.5,1.5)
                *(sin(low_phase_a)+.5*cos(low_phase_b));
            low_phase_a+=2.0*TEST_PI*90000.0/SAMPLE_RATE;
            low_phase_b+=2.0*TEST_PI*150000.0/SAMPLE_RATE;
            if(low_phase_a>2.0*TEST_PI)low_phase_a-=2.0*TEST_PI;
            if(low_phase_b>2.0*TEST_PI)low_phase_b-=2.0*TEST_PI;
        }
        rf[n]=(float)(source->rf_gain*(.72*cos(luma_phase)
            +source->chroma_amplitude*envelope*bell_gain*cos(converter_phase-chroma_phase+phase_error)
            +noise+inband+click+low_interference));
        if(luma_phase>2.0*TEST_PI)luma_phase=fmod(luma_phase,2.0*TEST_PI);
        if(chroma_phase>2.0*TEST_PI)chroma_phase=fmod(chroma_phase,2.0*TEST_PI);
        if(converter_phase>2.0*TEST_PI)converter_phase=fmod(converter_phase,2.0*TEST_PI);
    }
    if(source->inband_noise_amplitude>0.0) {
        double ratio=sqrt(band_noise_energy/(*length))/(.72/sqrt(2.0));
        printf("MESECAM independently generated band-noise RMS / physical Y carrier RMS %.5f\n",ratio);
        fflush(stdout);assert(ratio>.025);
    }
    if(source->off_noise_amplitude>0.0) {
        assert(off_noise_samples && off_noise_samples<*length);
        double on=sqrt((band_noise_energy-off_noise_energy)/(*length-off_noise_samples))/(.72/sqrt(2.0));
        double off=sqrt(off_noise_energy/off_noise_samples)/(.72/sqrt(2.0));
        printf("MESECAM physical band-noise RMS/Y unchanged before loss%.5f, only during carrier-off%.5f\n",on,off);
        fflush(stdout);assert(off>3.0*on);
    }
    if(source->rail_tone) {
        for(int c=0;c<2;c++) {
            printf("MESECAM physical150kHz strong FM %s studio range%.3f..%.3fkHz limiter%.2f%%\n",
                c?"Dr":"Db",studio_low[c]/1000.0,studio_high[c]/1000.0,
                100.0*studio_clipped[c]/studio_count[c]);
            assert(studio_count[c] && studio_low[c]<4000000.0);
        }
        fflush(stdout);assert(studio_high[1]==4756250.0 && studio_clipped[1]>studio_count[1]/20u);
    }
    free(timeline);return rf;
}

static void process(gui_rf_decoder_t *decoder,const float *rf,size_t length,
                    uint64_t offset,frames_t *frames) {
    size_t position=0;
    uint64_t hsyncs=0;
    while(position<length) {
        size_t count=(position&1)?32771u:4093u;
        if(count>length-position)count=length-position;
        gui_rf_decoder_process(decoder,rf+position,count,offset+position,receive,frames);
        gui_rf_decode_status_t status;gui_rf_decoder_get_status(decoder,&status);
        /* A sync reclassification must not look like a counter reset to a
         * worker observing the stream at arbitrary block boundaries. */
        assert(status.hsync_count>=hsyncs);hsyncs=status.hsync_count;
        position+=count;
    }
}

static double bar_mean(const frames_t *f,int bar,int parity,int channel) {
    double sum=0.0;int count=0;
    int centre=(int)((bar+.5)*f->width/8.0);
    for(int row=100+parity;row<300;row+=2)
        for(int x=centre-3;x<=centre+3;x++) {
            sum+=f->rgba[((size_t)row*f->width+x)*4u+channel];count++;
        }
    return sum/(count*255.0);
}

static void check_sync(const frames_t *f,const char *name) {
    printf("MESECAM %s: frames%d fields%llu rejects%llu H%d V%d C%d, %s\n",name,
        f->frames,(unsigned long long)f->status.fields_accepted,
        (unsigned long long)f->status.fields_rejected,f->status.h_locked,
        f->status.v_locked,f->status.color_locked,f->status.reason);
    fflush(stdout);
    assert(f->frames>=2 && f->status.h_locked && f->status.v_locked);
    assert(f->width==768 && f->height==576);
    assert(f->status.fields_rejected==f->status.reject_cadence+
        f->status.reject_parity+f->status.reject_line_map);
}

static void check_registration(const frames_t *f,const char *name) {
    int centre=f->width*6/8,radius=f->width/32;
    for(int parity=0;parity<2;parity++) {
        double previous_y=0,previous_u=0,previous_v=0;
        double y_gradient=0,c_gradient=0;int y_edge=0,c_edge=0;
        for(int x=centre-radius;x<=centre+radius;x++) {
            double rgb[3]={0};int count=0;
            for(int row=100+parity;row<300;row+=2) {
                for(int c=0;c<3;c++)rgb[c]+=f->rgba[((size_t)row*f->width+x)*4u+c]/255.0;
                count++;
            }
            for(int c=0;c<3;c++)rgb[c]/=count;
            double y=.299*rgb[0]+.587*rgb[1]+.114*rgb[2];
            double u=rgb[2]-y,v=rgb[0]-y;
            if(x>centre-radius) {
                double dy=fabs(y-previous_y),dc=hypot(u-previous_u,v-previous_v);
                if(dy>y_gradient){y_gradient=dy;y_edge=x;}
                if(dc>c_gradient){c_gradient=dc;c_edge=x;}
            }
            previous_y=y;previous_u=u;previous_v=v;
        }
        printf("MESECAM %s field%d red/blue edge Y%d C%d separation%d pixels\n",
            name,parity,y_edge,c_edge,abs(y_edge-c_edge));fflush(stdout);
        assert(y_gradient>.005 && c_gradient>.01 && abs(y_edge-c_edge)<=7);
    }
}

static void check_bars(const frames_t *f,const char *name) {
    check_sync(f,name);assert(f->status.color_locked);
    double max_error=0.0;
    for(int parity=0;parity<2;parity++) {
        for(int bar=1;bar<8;bar++) {
            double rgb[3];
            for(int channel=0;channel<3;channel++) {
                rgb[channel]=bar_mean(f,bar,parity,channel);
                max_error=fmax(max_error,fabs(rgb[channel]-colours[bar][channel]));
            }
            if(bar==5)assert(rgb[0]>rgb[1]+.30 && rgb[0]>rgb[2]+.30);
            if(bar==6)assert(rgb[2]>rgb[0]+.30 && rgb[2]>rgb[1]+.30);
        }
    }
    printf("MESECAM %s physical RGB maximum error %.4f\n",name,max_error);fflush(stdout);
    /* Tolerance includes studio swing, preview bandwidth, and causal
     * record-filter settling. Targets are physical RGB, not decoder output
     * matrix calculations or coefficient-derived expected values. */
    assert(max_error<.14);
    double a=bar_mean(f,0,0,1),b=bar_mean(f,0,1,1);
    assert(fabs(a-b)>.30 && fmin(a,b)<.40 && fmax(a,b)>.60);
    check_registration(f,name);
}

static void check_gray(const frames_t *f,const char *name,bool native_carrier) {
    check_sync(f,name);
    if(!native_carrier)assert(!f->status.color_locked);
    double highest=0.0;
    for(int parity=0;parity<2;parity++)for(int bar=1;bar<8;bar++) {
        double r=bar_mean(f,bar,parity,0),g=bar_mean(f,bar,parity,1),b=bar_mean(f,bar,parity,2);
        highest=fmax(highest,fmax(r,fmax(g,b))-fmin(r,fmin(g,b)));
        assert(fmin(r,fmin(g,b))>.20 && fmax(r,fmax(g,b))<.80);
    }
    printf("MESECAM %s neutral RGB channel spread %.4f\n",name,highest);fflush(stdout);
    assert(highest<.035);
}

static void check_solid(const frames_t *f,const mesecam_source_t *source,const char *name) {
    check_sync(f,name);
    const bool identified=(source->field_ident || !source->no_line_ident) && !source->ident_end_field;
    assert(f->status.color_locked==identified);
    if(source->ident_end_field)assert(f->ever_colour_locked);
    double largest=0.0,spread=0.0;
    int colour=3+source->solid_colour;
    printf("MESECAM physical solid colour index%d\n",colour);
    for(int parity=0;parity<2;parity++) {
        double rgb[3];
        for(int c=0;c<3;c++) {
            rgb[c]=bar_mean(f,3,parity,c);
            if(identified)largest=fmax(largest,fabs(rgb[c]-colours[colour][c]));
        }
        spread=fmax(spread,fmax(rgb[0],fmax(rgb[1],rgb[2]))-fmin(rgb[0],fmin(rgb[1],rgb[2])));
    }
    printf("MESECAM %s: solidRGBerror%.4f channelspread%.4f identified%d\n",name,largest,spread,identified);
    fflush(stdout);
    if(identified)assert(largest<.08);
    else assert(spread<.035);
}

/* Free-phase harmonic regression separates transfer magnitude from timing.
 * This is an output-signal measurement against physical RGB amplitudes,
 * independent of the receiver's filter design or coefficient helpers. */
static double tone_amplitude(const frames_t *f,int parity,int channel,double frequency) {
    double matrix[3][4]={{0}};
    for(int x=f->width/8;x<f->width*7/8;x++) {
        double rgb[3]={0};int count=0;
        for(int row=100+parity;row<300;row+=2) {
            for(int c=0;c<3;c++)rgb[c]+=f->rgba[((size_t)row*f->width+x)*4u+c]/255.0;
            count++;
        }
        for(int c=0;c<3;c++)rgb[c]/=count;
        double value=channel<3?rgb[channel]:.299*rgb[0]+.587*rgb[1]+.114*rgb[2];
        double angle=2.0*TEST_PI*frequency*52e-6*(x+.5)/f->width;
        double basis[3]={1.0,sin(angle),cos(angle)};
        for(int a=0;a<3;a++) {
            for(int b=0;b<3;b++)matrix[a][b]+=basis[a]*basis[b];
            matrix[a][3]+=basis[a]*value;
        }
    }
    for(int k=0;k<3;k++) {
        double divisor=matrix[k][k];assert(fabs(divisor)>1e-6);
        for(int b=k;b<4;b++)matrix[k][b]/=divisor;
        for(int a=0;a<3;a++)if(a!=k) {
            double factor=matrix[a][k];
            for(int b=k;b<4;b++)matrix[a][b]-=factor*matrix[k][b];
        }
    }
    return hypot(matrix[1][3],matrix[2][3]);
}

static void check_tone(const frames_t *f) {
    check_sync(f,"85kHz colour-difference modulation");assert(f->status.color_locked);
    for(int parity=0;parity<2;parity++) {
        double r=tone_amplitude(f,parity,0,85000.0),b=tone_amplitude(f,parity,2,85000.0);
        double y=tone_amplitude(f,parity,3,85000.0);
        printf("MESECAM field%d 85kHz RGB amplitudes R%.5f B%.5f, luma%.5f\n",parity,r,b,y);
        fflush(stdout);
        /* Omitting LF de-emphasis leaves the record network's ~1.34 gain
         * at 85kHz and fails this bound by a wide margin. */
        assert(fabs(r-.22)<.025 && fabs(b-.16)<.025 && y<.015);
    }
}

static void check_full_frame(gui_rf_decoder_t *decoder,const float *rf,size_t length,
                             frames_t *frames) {
    gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_MESECAM_VHS);
    gui_rf_decoder_set_frame_mode(decoder,true);
    memset(frames->rgba,0xa5,(size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    frames->frames=0;frames->last_end=0;
    process(decoder,rf,length,0,frames);
    assert(frames->frames>=2 && frames->width==768 && frames->height==626);
    assert(frames->status.h_locked && frames->status.v_locked && frames->status.color_locked);
    for(int row=0;row<626;row++)for(int x=0;x<768;x++) {
        const uint8_t *pixel=frames->rgba+((size_t)row*768+x)*4u;
        if(row==624)assert(pixel[0]==0 && pixel[1]==0 && pixel[2]==0 && pixel[3]==0);
        else assert(pixel[3]==255);
    }
    /* A full frame retains native field markers and both last valid rows.
     * These columns come from physical microseconds, not receiver geometry
     * helpers; a crop, wrong PAL/NTSC height, or stale parity plane fails. */
    const int marker=(int)((10.5+.5*52.0/8.0)/64.0*768.0);
    const int red=(int)((10.5+5.5*52.0/8.0)/64.0*768.0);
    const int blue=(int)((10.5+6.5*52.0/8.0)/64.0*768.0);
    double marker_mean[2]={0};
    for(int parity=0;parity<2;parity++) {
        for(int row=100+parity;row<300;row+=2)
            marker_mean[parity]+=frames->rgba[((size_t)row*768+marker)*4u+1]/255.0/100.0;
        const int last=parity?625:622;
        const uint8_t *r=frames->rgba+((size_t)last*768+red)*4u;
        const uint8_t *b=frames->rgba+((size_t)last*768+blue)*4u;
        assert(r[0]>r[1]+75 && r[0]>r[2]+75);
        assert(b[2]>b[0]+75 && b[2]>b[1]+75);
    }
    assert(fabs(marker_mean[0]-marker_mean[1])>.30);
    printf("MESECAM Full: %dx%d, opaque written rows, empty624, bottom622/625 hue and weave preserved\n",
        frames->width,frames->height);fflush(stdout);
}

static void check_level(const frames_t *f,const char *name,double expected) {
    check_sync(f,name);assert(f->status.color_locked);
    double level=0.0,spread=0.0;
    for(int p=0;p<2;p++) {
        double rgb[3];for(int c=0;c<3;c++)rgb[c]=bar_mean(f,4,p,c);
        level+=(rgb[0]+rgb[1]+rgb[2])/6.0;
        spread=fmax(spread,fmax(rgb[0],fmax(rgb[1],rgb[2]))-fmin(rgb[0],fmin(rgb[1],rgb[2])));
    }
    printf("MESECAM %s level%.5f, neutral spread%.5f\n",name,level,spread);fflush(stdout);
    /* Physical black/50%/white should remain black/gray/white, with this
     * preview's known display footroom/headroom. No decoder matrix enters
     * the expected hue or level. */
    assert(fabs(level-expected)<.025 && spread<.025);
}

typedef struct {
    frames_t frame;
    gui_rf_profile_t profile;
    uint64_t accepted;
    double max_luma_error, max_neutral_spread;
} luma_contract_t;

/* The receiver's colour-system choice must not gate the independent PAL
 * luminance timing. Check EVERY delivered weave against the original RF's
 * physical bar Y and opposite field markers, not another decoder output. */
static void receive_shared_luma(void *context,const uint8_t *rgba,int width,int height,
                                uint64_t end,const gui_rf_decode_status_t *status) {
    luma_contract_t *contract=context;frames_t *f=&contract->frame;
    uint64_t previous_end=f->last_end;int previous_frames=f->frames;
    receive(f,rgba,width,height,end,status);
    assert(width==768 && height==576 && status->h_locked && status->v_locked);
    if(previous_frames) {
        /* 625 physical lines at 64us and 20MSPS: one callback each 40ms.
         * A dropped field/weave cannot hide behind a correct final frame. */
        assert(fabs((double)(end-previous_end)-800000.0)<1280.0);
        assert(status->fields_accepted==contract->accepted+2);
    }
    contract->accepted=status->fields_accepted;
    if(contract->profile==GUI_RF_PROFILE_SECAM_VHS)assert(!status->color_locked);
    for(int parity=0;parity<2;parity++)for(int bar=0;bar<8;bar++) {
        double rgb[3];for(int c=0;c<3;c++)rgb[c]=bar_mean(f,bar,parity,c);
        double actual_y=.299*rgb[0]+.587*rgb[1]+.114*rgb[2];
        double physical_y=bar==0?(parity?.30:.70)
            :.299*colours[bar][0]+.587*colours[bar][1]+.114*colours[bar][2];
        double expected_y=(10.0+235.0*physical_y)/255.0;
        double error=fabs(actual_y-expected_y);
        contract->max_luma_error=fmax(contract->max_luma_error,error);
        assert(error<.045);
        if(contract->profile==GUI_RF_PROFILE_SECAM_VHS) {
            double spread=fmax(rgb[0],fmax(rgb[1],rgb[2]))-fmin(rgb[0],fmin(rgb[1],rgb[2]));
            contract->max_neutral_spread=fmax(contract->max_neutral_spread,spread);
            assert(spread<.025);
        }
    }
}

static void check_shared_luma(const mesecam_source_t *ordinary) {
    const gui_rf_profile_t profiles[]={GUI_RF_PROFILE_PAL_VHS,
        GUI_RF_PROFILE_SECAM_VHS,GUI_RF_PROFILE_MESECAM_VHS};
    for(int shape=0;shape<2;shape++) {
        mesecam_source_t source=*ordinary;source.fields=18;
        source.first_eq_extra_us=1.25;source.widen_mask=shape?3:2;
        size_t length;float *rf=make_source(&source,&length);
        for(size_t i=0;i<sizeof(profiles)/sizeof(profiles[0]);i++) {
            luma_contract_t contract={.profile=profiles[i]};
            contract.frame.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
            assert(contract.frame.rgba);
            gui_rf_decoder_t *d=gui_rf_decoder_create_profile(profiles[i]);assert(d);
            uint64_t hsyncs=0;size_t at=0;
            while(at<length) {
                size_t count=(at&1)?32771u:4093u;if(count>length-at)count=length-at;
                gui_rf_decoder_process(d,rf+at,count,at,receive_shared_luma,&contract);
                gui_rf_decode_status_t status;gui_rf_decoder_get_status(d,&status);
                assert(status.hsync_count>=hsyncs);hsyncs=status.hsync_count;at+=count;
            }
            gui_rf_decode_status_t status;gui_rf_decoder_get_status(d,&status);
            printf("ME RF shared Y, %s, broadened mask%d: frames%d accepted%llu rejects%llu Yerror%.5f neutral%.5f C%d\n",
                gui_rf_profile_name(profiles[i]),source.widen_mask,contract.frame.frames,
                (unsigned long long)status.fields_accepted,(unsigned long long)status.fields_rejected,
                contract.max_luma_error,contract.max_neutral_spread,status.color_locked);fflush(stdout);
            assert(contract.frame.frames==8 && status.fields_accepted==16);
            assert(status.h_locked && status.v_locked && status.fields_rejected<=1);
            if(profiles[i]==GUI_RF_PROFILE_SECAM_VHS)assert(!contract.frame.ever_colour_locked);
            free(contract.frame.rgba);gui_rf_decoder_destroy(d);
        }
        free(rf);
    }
}

typedef struct {
    frames_t frame;
    const mesecam_source_t *source;
    int before,interrupted,recovered;
    double max_off_mean,max_off_p99,max_good_error;
} carrier_contract_t;

/* A pixel-domain observation, independent of any receiver confidence
 * statistic: report channel disagreement in the physical dropout region.
 * Per-pixel quantiles prevent opposite colour errors from averaging away. */
static void region_chroma(const frames_t *f,int x0,int x1,int y0,int y1,int parity,
                          double *mean,double *p99) {
    unsigned histogram[256]={0};unsigned count=0;double sum=0.0;
    int step=parity<0?1:2;
    if(parity>=0 && (y0&1)!=parity)y0++;
    for(int y=y0;y<y1;y+=step)for(int x=x0;x<x1;x++) {
        const uint8_t *p=f->rgba+((size_t)y*f->width+x)*4u;
        int hi=p[0]>p[1]?p[0]:p[1],lo=p[0]<p[1]?p[0]:p[1];
        if(p[2]>hi)hi=p[2];
        if(p[2]<lo)lo=p[2];
        histogram[hi-lo]++;sum+=hi-lo;count++;
    }
    assert(count);unsigned accumulated=0;int q=0;
    for(;q<255;q++){accumulated+=histogram[q];if(accumulated>=count*.99)break;}
    *mean=sum/(count*255.0);*p99=q/255.0;
}

static double carrier_good_colour(const frames_t *f,int first_row,int last_row,int only_parity) {
    double worst=0.0;
    for(int parity=0;parity<2;parity++)for(int bar=1;bar<8;bar++) {
        if(only_parity>=0 && parity!=only_parity)continue;
        double rgb[3]={0};int count=0,centre=(int)((bar+.5)*f->width/8.0);
        for(int row=first_row+parity;row<last_row;row+=2)for(int x=centre-3;x<=centre+3;x++) {
            for(int c=0;c<3;c++)rgb[c]+=f->rgba[((size_t)row*f->width+x)*4u+c]/255.0;
            count++;
        }
        for(int c=0;c<3;c++) {rgb[c]/=count;worst=fmax(worst,fabs(rgb[c]-colours[bar][c]));}
        if(bar==5)assert(rgb[0]>rgb[1]+.24 && rgb[0]>rgb[2]+.24);
        if(bar==6)assert(rgb[2]>rgb[0]+.24 && rgb[2]>rgb[1]+.24);
    }
    assert(worst<.17);return worst;
}

static void carrier_dropout_luma(const frames_t *f,int x0,int x1,int y0,int y1,int parity) {
    int seen=0;
    for(int bar=0;bar<8;bar++) {
        int centre=(int)((bar+.5)*f->width/8.0);
        if(centre-3<x0 || centre+3>=x1)continue;
        double sum=0.0;int count=0;
        int first=y0;if((first&1)!=parity)first++;
        for(int row=first;row<y1;row+=2)for(int x=centre-3;x<=centre+3;x++) {
            const uint8_t *p=f->rgba+((size_t)row*f->width+x)*4u;
            sum+=(.299*p[0]+.587*p[1]+.114*p[2])/255.0;count++;
        }
        double physical_y=bar==0?(parity?.30:.70)
            :.299*colours[bar][0]+.587*colours[bar][1]+.114*colours[bar][2];
        assert(count && fabs(sum/count-(10.0+235.0*physical_y)/255.0)<.045);seen++;
    }
    assert(seen>=2);
}

static double carrier_tail_colour(const frames_t *f,int parity) {
    double worst=0.0,measured[2][3];
    for(int bar=5;bar<=6;bar++) {
        for(int c=0;c<3;c++) {
            measured[bar-5][c]=bar_mean(f,bar,parity,c);
            worst=fmax(worst,fabs(measured[bar-5][c]-colours[bar][c]));
        }
    }
    printf("MESECAM intact partial-line tail parity%d red(%.4f,%.4f,%.4f) blue(%.4f,%.4f,%.4f) RGBerror%.5f\n",
        parity,measured[0][0],measured[0][1],measured[0][2],
        measured[1][0],measured[1][1],measured[1][2],worst);
    fflush(stdout);assert(worst<.12);return worst;
}

static void receive_carrier_contract(void *context,const uint8_t *rgba,int width,int height,
                                      uint64_t end,const gui_rf_decode_status_t *status) {
    carrier_contract_t *contract=context;frames_t *f=&contract->frame;
    uint64_t prior=f->last_end;int previous=f->frames;
    receive(f,rgba,width,height,end,status);
    assert(width==768 && height==576 && status->h_locked && status->v_locked);
    if(previous)assert(fabs((double)(end-prior)-800000.0)<1280.0);
    /* 625 lines / two physical fields, at64us and20MSPS. Determine each
     * delivered row's physical field even at a transition's mixed weave. */
    int end_field=(int)llround(end/(SAMPLE_RATE*.020));
    int start=contract->source->carrier_loss_start,stop=contract->source->carrier_loss_end;
    if(end_field>=6 && end_field<=start) {
        contract->before++;
        assert(status->color_locked);
        contract->max_good_error=fmax(contract->max_good_error,carrier_good_colour(f,100,300,-1));
    }
    for(int parity=0;parity<2;parity++) {
        int physical=end_field-1;
        if((physical&1)!=parity)physical--;
        if(physical<start || physical>=stop)continue;
        contract->interrupted++;
        int mode=contract->source->carrier_loss_mode;
        int x0=20,x1=width-20,y0=80,y1=height-40;
        if(mode==3) {y0=2*(122-22);y1=2*(126-22);}
        if(mode==4) {x0=(int)((31.5-10.5)/52.0*width);x1=(int)((40.5-10.5)/52.0*width);}
        if(mode==5) {x0=(int)((14.5-10.5)/52.0*width);x1=(int)((40.5-10.5)/52.0*width);}
        double mean,p99;region_chroma(f,x0,x1,y0,y1,parity,&mean,&p99);
        contract->max_off_mean=fmax(contract->max_off_mean,mean);
        contract->max_off_p99=fmax(contract->max_off_p99,p99);
        printf("MESECAM physical carrier loss%d sourcefield%d parity%d endfield%d: meanRGBspread%.5f p99%.5f C%d\n",
            mode,physical,parity,end_field,mean,p99,status->color_locked);fflush(stdout);
        /* Check the first delivered damaged FIELD, including a mixed weave
         * whose other field is still sound. Keeping an old H identity for
         * 32fields may not retain false hue in this newly damaged field. */
        assert(mean<.018 && p99<.045);
        carrier_dropout_luma(f,x0,x1,y0,y1,parity);
        if(mode==3) {
            assert(status->color_locked);
            contract->max_good_error=fmax(contract->max_good_error,carrier_good_colour(f,70,160,parity));
        }
        if(mode==4 || mode==5) {
            /* A damaged centre patch must not blank the sound red/blue
             * region after physical43us or the whole colour field. */
            if(mode==4)assert(status->color_locked);
            /* Above-half-line damage may legitimately lower field C-lock
             * statistics. Every sound pixel still needs BOTH components;
             * a red/blue hue inequality alone can miss a lost component. */
            contract->max_good_error=fmax(contract->max_good_error,carrier_tail_colour(f,parity));
        }
    }
    /* A whole-field dropout may share one weave with a healthy physical
     * field. Preserve colour in that healthy field instead of globally
     * turning the complete image gray. */
    if(end_field>=start && end_field<=stop+2)for(int parity=0;parity<2;parity++) {
        int physical=end_field-1;if((physical&1)!=parity)physical--;
        if(physical>=6 && physical<start)
            contract->max_good_error=fmax(contract->max_good_error,carrier_good_colour(f,100,300,parity));
    }
    if(end_field>=stop+4) {
        contract->recovered++;assert(status->color_locked);
        contract->max_good_error=fmax(contract->max_good_error,carrier_good_colour(f,100,300,-1));
    }
}

static void check_carrier_interruption(int mode,bool short_loss,bool bottle_only) {
    mesecam_source_t source={.chroma_amplitude=.13,.rf_gain=1,.clock_scale=1,
        .flat_level=-1,.inband_noise_amplitude=.10,.fields=short_loss?18:26,
        .off_noise_amplitude=mode==6?.40:0.0,
        .carrier_loss_mode=mode,.carrier_loss_start=8,.carrier_loss_end=short_loss?10:16,
        .field_ident=bottle_only,.no_line_ident=bottle_only,.bottle_picture_only=bottle_only};
    carrier_contract_t contract={.source=&source};
    contract.frame.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(contract.frame.rgba);
    gui_rf_decoder_t *d=gui_rf_decoder_create_profile(GUI_RF_PROFILE_MESECAM_VHS);assert(d);
    size_t length;float *rf=make_source(&source,&length);size_t at=0;
    while(at<length) {
        size_t count=(at&1)?32771u:4093u;if(count>length-at)count=length-at;
        gui_rf_decoder_process(d,rf+at,count,at,receive_carrier_contract,&contract);at+=count;
    }
    printf("MESECAM physical loss%d short%d bottle-only%d: before%d off%d recovered%d maxgraymean%.5f p99%.5f RGBerror%.5f\n",
        mode,short_loss,bottle_only,contract.before,contract.interrupted,contract.recovered,
        contract.max_off_mean,contract.max_off_p99,contract.max_good_error);fflush(stdout);
    assert(contract.before>=1 && contract.interrupted>=1 && contract.recovered>=1);
    free(rf);free(contract.frame.rgba);gui_rf_decoder_destroy(d);
}

static void check_neutral_phase_damage(void) {
    mesecam_source_t source={.chroma_amplitude=.10,.rf_gain=1,.clock_scale=1,
        .flat_level=.5,.noise_amplitude=.003,.phase_artifacts=true};
    frames_t f={0};f.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(f.rgba);
    gui_rf_decoder_t *d=gui_rf_decoder_create_profile(GUI_RF_PROFILE_MESECAM_VHS);assert(d);
    size_t length;float *rf=make_source(&source,&length);process(d,rf,length,0,&f);
    check_sync(&f,"strong undeviated carrier with physical phase/AM/click damage");assert(f.status.color_locked);
    double mean,p99;region_chroma(&f,20,f.width-20,80,f.height-40,-1,&mean,&p99);
    printf("MESECAM physical neutral phase/AM/click RGB spread mean%.5f p99%.5f\n",mean,p99);fflush(stdout);
    assert(mean<.025 && p99<.075);
    free(rf);free(f.rgba);gui_rf_decoder_destroy(d);
}

static void check_low_saturation(void) {
    mesecam_source_t source={.chroma_amplitude=.065,.rf_gain=1,.clock_scale=1,
        .flat_level=-1,.colour_saturation=.08};
    frames_t f={0};f.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(f.rgba);
    gui_rf_decoder_t *d=gui_rf_decoder_create_profile(GUI_RF_PROFILE_MESECAM_VHS);assert(d);
    size_t length;float *rf=make_source(&source,&length);process(d,rf,length,0,&f);
    check_sync(&f,"physical8percent-saturation colour bars");assert(f.status.color_locked);
    double largest=0.0;
    for(int parity=0;parity<2;parity++)for(int bar=1;bar<8;bar++) {
        double rgb[3];for(int c=0;c<3;c++)rgb[c]=bar_mean(&f,bar,parity,c);
        double expected_rg=.08*(colours[bar][0]-colours[bar][1]);
        double expected_bg=.08*(colours[bar][2]-colours[bar][1]);
        largest=fmax(largest,fabs(rgb[0]-rgb[1]-expected_rg));
        largest=fmax(largest,fabs(rgb[2]-rgb[1]-expected_bg));
        if(bar==5)assert(rgb[0]>rgb[1]+.025);
        if(bar==6)assert(rgb[2]>rgb[1]+.025);
    }
    printf("MESECAM physical8percent-saturation maximum colour-difference error%.5f\n",largest);fflush(stdout);
    assert(largest<.018);
    free(rf);free(f.rgba);gui_rf_decoder_destroy(d);
}

static void check_low_frequency_ingress(void) {
    for(int gray=0;gray<2;gray++) {
        mesecam_source_t source={.chroma_amplitude=.065,.rf_gain=1,.clock_scale=1,
            .flat_level=gray?.5:-1,.low_frequency_amplitude=.10};
        frames_t f={0};f.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(f.rgba);
        gui_rf_decoder_t *d=gui_rf_decoder_create_profile(GUI_RF_PROFILE_MESECAM_VHS);assert(d);
        size_t length;float *rf=make_source(&source,&length);process(d,rf,length,0,&f);
        check_sync(&f,gray?"neutral carrier with90/150kHz real RF ingress"
            :"colour bars with90/150kHz real RF ingress");assert(f.status.color_locked);
        if(gray) {
            double mean,p99;region_chroma(&f,20,f.width-20,80,f.height-40,-1,&mean,&p99);
            printf("MESECAM neutral physical low-frequency ingress RGB spread mean%.5f p99%.5f\n",mean,p99);fflush(stdout);
            assert(mean<.025 && p99<.075);
        } else {
            double error=carrier_good_colour(&f,100,300,-1);
            printf("MESECAM physical low-frequency ingress colour RGBerror%.5f\n",error);fflush(stdout);
        }
        free(rf);free(f.rgba);gui_rf_decoder_destroy(d);
    }
}

static void check_low_carrier_solid(void) {
    /* Low RF amplitude is distinct from low colour saturation: keep an
     * actual modulated magenta picture, whose two component frequencies
     * lie across the opposite rest frequency, on weak but coherent RF. */
    mesecam_source_t source={.chroma_amplitude=.025,.rf_gain=1,.clock_scale=1,
        .flat_level=-1,.solid_colour=1,.noise_amplitude=.001};
    frames_t f={0};f.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(f.rgba);
    gui_rf_decoder_t *d=gui_rf_decoder_create_profile(GUI_RF_PROFILE_MESECAM_VHS);assert(d);
    size_t length;float *rf=make_source(&source,&length);process(d,rf,length,0,&f);
    check_solid(&f,&source,"low RF carrier amplitude, modulated magenta");
    free(rf);free(f.rgba);gui_rf_decoder_destroy(d);
}

/* Numerically integrate the ideal studio limiter's fundamental over one
 * cycle, then undo the STANDARD record LF network at that frequency. This
 * is a physical amplitude prediction, not a receiver transfer function or
 * a golden output generated by the decoder being tested. */
static double limited_tone_fundamental(bool dr) {
    const double frequency=150000.0;
    double gain=sqrt((1.0+pow(frequency/85000.0,2.0))
        /(1.0+pow(frequency/255000.0,2.0)));
    double deviation=dr?280000.0:230000.0,component_scale=dr?1.902:1.505;
    double nominal=dr?4406250.0:4250000.0;
    double amplitude=.49*component_scale*deviation*gain,sum=0.0;
    for(int i=0;i<8192;i++) {
        double s=sin(2.0*TEST_PI*(i+.5)/8192.0);
        sum+=(limit(nominal+amplitude*s,3900000.0,4756250.0)-nominal)*s;
    }
    return 2.0*sum/8192.0/(component_scale*deviation*gain);
}

static void check_strong_deviation(void) {
    mesecam_source_t source={.chroma_amplitude=.065,.rf_gain=1,.clock_scale=1,
        .flat_level=-1,.rail_tone=true};
    frames_t f={0};f.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(f.rgba);
    gui_rf_decoder_t *d=gui_rf_decoder_create_profile(GUI_RF_PROFILE_MESECAM_VHS);assert(d);
    size_t length;float *rf=make_source(&source,&length);process(d,rf,length,0,&f);
    check_sync(&f,"physical150kHz strong FM near studio limiter rails");assert(f.status.color_locked);
    double expected_r=limited_tone_fundamental(true),expected_b=limited_tone_fundamental(false);
    for(int parity=0;parity<2;parity++) {
        double r=tone_amplitude(&f,parity,0,150000.0),b=tone_amplitude(&f,parity,2,150000.0);
        double y=tone_amplitude(&f,parity,3,150000.0);
        printf("MESECAM field%d strong150kHz RGB amplitudes R%.5f B%.5f physical targets%.5f/%.5f Y%.5f\n",
            parity,r,b,expected_r,expected_b,y);fflush(stdout);
        /* Retain the real strongly modulated carrier, including its lawful
         * limiter distortion. A one-tone coherence killer, wrong component
         * assignment or an overly narrow colour path must fail. */
        assert(fabs(r-expected_r)<.045 && fabs(b-expected_b)<.045 && y<.020);
    }
    free(rf);free(f.rgba);gui_rf_decoder_destroy(d);
}

static void check_active_carrier_fade(void) {
    mesecam_source_t source={.chroma_amplitude=.065,.rf_gain=1,.clock_scale=1,
        .flat_level=-1,.noise_amplitude=.001,.active_carrier_scale=.5};
    frames_t f={0};f.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(f.rgba);
    gui_rf_decoder_t *d=gui_rf_decoder_create_profile(GUI_RF_PROFILE_MESECAM_VHS);assert(d);
    size_t length;float *rf=make_source(&source,&length);process(d,rf,length,0,&f);
    check_bars(&f,"true colour FM with physical picture-only6dB AM loss and intact H carrier");
    free(rf);free(f.rgba);gui_rf_decoder_destroy(d);
}

typedef struct {
    uint64_t rgba_hash,end;
    int width,height;
    gui_rf_decode_status_t status;
} frame_digest_t;

typedef struct {
    frame_digest_t frames[32];
    int count;
    bool ever_colour;
} digest_sequence_t;

static void receive_digest(void *context,const uint8_t *rgba,int width,int height,
                           uint64_t end,const gui_rf_decode_status_t *status) {
    digest_sequence_t *sequence=context;assert(sequence->count<32);
    assert(width==768 && (height==576 || height==626));
    assert(status->h_locked && status->v_locked);
    frame_digest_t *frame=&sequence->frames[sequence->count++];
    uint64_t hash=UINT64_C(14695981039346656037);
    for(size_t i=0;i<(size_t)width*height*4u;i++)hash=(hash^rgba[i])*UINT64_C(1099511628211);
    frame->rgba_hash=hash;frame->end=end;frame->width=width;frame->height=height;frame->status=*status;
    sequence->ever_colour|=status->color_locked;
}

static void equal_status(const gui_rf_decode_status_t *a,const gui_rf_decode_status_t *b) {
    /* Compare semantic fields; struct padding is not initialized API data. */
#define SAME_STATUS(member) assert(a->member==b->member)
    SAME_STATUS(h_locked);SAME_STATUS(v_locked);SAME_STATUS(color_locked);
    SAME_STATUS(hsync_count);SAME_STATUS(vsync_count);SAME_STATUS(fields_decoded);
    SAME_STATUS(line_period_us);SAME_STATUS(frames_decoded);SAME_STATUS(vsync_candidates);
    SAME_STATUS(fields_accepted);SAME_STATUS(fields_rejected);SAME_STATUS(reject_cadence);
    SAME_STATUS(reject_parity);SAME_STATUS(reject_line_map);SAME_STATUS(color_losses);
#undef SAME_STATUS
    assert(!strcmp(a->last_reject_reason,b->last_reject_reason));
    assert(!strcmp(a->last_color_loss_reason,b->last_color_loss_reason));
    assert(!strcmp(a->reason,b->reason));
}

static void check_chunk_invariance(void) {
    mesecam_source_t source={.chroma_amplitude=.13,.rf_gain=1,.clock_scale=1,
        .flat_level=-1,.fields=14,.field_ident=true,.inband_noise_amplitude=.10,
        .carrier_loss_mode=5,.carrier_loss_start=8,.carrier_loss_end=10};
    size_t length;float *rf=make_source(&source,&length);
    const size_t ragged[]={1,127,4093,32771,524287,17,65521};
    for(int full=0;full<2;full++) {
        digest_sequence_t sequence[2]={0};gui_rf_decode_status_t final_status[2];
        for(int pass=0;pass<2;pass++) {
            gui_rf_decoder_t *d=gui_rf_decoder_create_profile(GUI_RF_PROFILE_MESECAM_VHS);assert(d);
            gui_rf_decoder_set_frame_mode(d,full!=0);
            size_t at=0,block=0;
            while(at<length) {
                size_t count=pass?ragged[block++%(sizeof(ragged)/sizeof(ragged[0]))]:16384u;
                if(count>length-at)count=length-at;
                gui_rf_decoder_process(d,rf+at,count,at,receive_digest,&sequence[pass]);at+=count;
            }
            gui_rf_decoder_get_status(d,&final_status[pass]);gui_rf_decoder_destroy(d);
            assert(sequence[pass].count>=4 && sequence[pass].ever_colour);
        }
        assert(sequence[0].count==sequence[1].count);
        equal_status(&final_status[0],&final_status[1]);
        for(int frame=0;frame<sequence[0].count;frame++) {
            const frame_digest_t *a=&sequence[0].frames[frame],*b=&sequence[1].frames[frame];
            assert(a->end==b->end && a->width==b->width && a->height==b->height);
            assert(a->rgba_hash==b->rgba_hash);equal_status(&a->status,&b->status);
            printf("MESECAM %s chunk invariant frame%d end%llu RGBAhash%016llx status identical\n",
                full?"Full":"Active",frame,(unsigned long long)a->end,(unsigned long long)a->rgba_hash);
        }
        fflush(stdout);
    }
    free(rf);
}

static void check_carrier_guards(void) {
    for(int mode=1;mode<=6;mode++)check_carrier_interruption(mode,false,false);
    check_carrier_interruption(2,true,false);check_carrier_interruption(1,false,true);
    check_neutral_phase_damage();check_low_saturation();check_low_frequency_ingress();
    check_low_carrier_solid();check_strong_deviation();check_active_carrier_fade();
    check_chunk_invariance();
}

int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--carrier-guards")) {check_carrier_guards();return 0;}
    if(argc==3 && !strcmp(argv[1],"--carrier-loss")) {
        int mode=atoi(argv[2]);assert(mode>=1 && mode<=6);check_carrier_interruption(mode,false,false);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--carrier-short")) {check_carrier_interruption(2,true,false);return 0;}
    if(argc==2 && !strcmp(argv[1],"--bottle-loss")) {check_carrier_interruption(1,false,true);return 0;}
    if(argc==2 && !strcmp(argv[1],"--neutral-phase")) {check_neutral_phase_damage();return 0;}
    if(argc==2 && !strcmp(argv[1],"--low-saturation")) {check_low_saturation();return 0;}
    if(argc==2 && !strcmp(argv[1],"--low-rf")) {check_low_frequency_ingress();return 0;}
    if(argc==2 && !strcmp(argv[1],"--low-carrier")) {check_low_carrier_solid();return 0;}
    if(argc==2 && !strcmp(argv[1],"--strong-fm")) {check_strong_deviation();return 0;}
    if(argc==2 && !strcmp(argv[1],"--active-fade")) {check_active_carrier_fade();return 0;}
    if(argc==2 && !strcmp(argv[1],"--chunks")) {check_chunk_invariance();return 0;}
    const size_t frame_bytes=(size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u;
    frames_t f={0};f.rgba=malloc(frame_bytes);assert(f.rgba);
    const mesecam_source_t ordinary={.chroma_amplitude=.065,.rf_gain=1.0,
        .clock_scale=1.0,.flat_level=-1.0};
    check_shared_luma(&ordinary);
    double decode_seconds=0.0,rf_seconds=0.0;
    for(int test=0;test<19;test++) {
        mesecam_source_t source=ordinary;const char *name="horizontal reference bars";
        if(test==1){name="reversed Dr/Db";source.dr_first=1;}
        if(test==2){name="swapped head phases";source.swapped_heads=1;}
        if(test==3){name="+0.8% tape clock, H jitter";source.clock_scale=1.008;source.jitter_us=.035;}
        if(test==4){name="gain0.35 plus RF noise";source.rf_gain=.35;source.noise_amplitude=.012;}
        if(test==5){name="vertical bottles only";source.field_ident=true;source.no_line_ident=true;}
        if(test==6){name="physical85kHz modulation";source.chroma_tone=true;}
        if(test==7){name="no chroma carrier";source.chroma_amplitude=0.0;source.gray=true;}
        if(test==8){name="weak chroma plus noise";source.chroma_amplitude=.00001;source.noise_amplitude=.006;source.gray=true;}
        if(test>=9 && test<12){name="no identification, saturated solid";source.no_line_ident=true;source.solid_colour=test-8;}
        if(test>=12 && test<15){name="saturated solid with quiet H references";source.solid_colour=test-11;}
        if(test==15){name="black level";source.flat_level=0.0;}
        if(test==16){name="gray50 level";source.flat_level=.5;}
        if(test==17){name="white level";source.flat_level=1.0;}
        if(test==18){name="first EQ physically broadened in BOTH field phases";
            /* Controlled physical sync broadening to 3.6us is a channel
             * stress shape, not a claim that broadcast EQ width is 3.6us.
             * Both half-line phases must retain their physical markers. */
            source.first_eq_extra_us=1.25;source.widen_mask=3;}
        gui_rf_decoder_t *d=gui_rf_decoder_create_profile(GUI_RF_PROFILE_MESECAM_VHS);assert(d);
        assert(gui_rf_profile_format(GUI_RF_PROFILE_MESECAM_VHS)==GUI_RF_PAL);
        memset(f.rgba,0xa5,frame_bytes);f.frames=0;f.last_end=0;f.ever_colour_locked=false;
        size_t length;float *rf=make_source(&source,&length);
        clock_t begin=clock();process(d,rf,length,0,&f);
        decode_seconds+=(double)(clock()-begin)/CLOCKS_PER_SEC;rf_seconds+=length/SAMPLE_RATE;
        if(test<6 || test==18)check_bars(&f,name);
        else if(test==6)check_tone(&f);
        else if(test==7 || test==8)check_gray(&f,name,false);
        else if(test>=9 && test<15)check_solid(&f,&source,name);
        else check_level(&f,name,test==15?10.0/255.0:test==16?.5:245.0/255.0);
        if(test==0) {
            check_full_frame(d,rf,length,&f);
            gui_rf_decoder_set_frame_mode(d,false);
            gui_rf_decoder_reset_profile(d,GUI_RF_PROFILE_MESECAM_VHS);
            f.frames=0;f.last_end=0;f.ever_colour_locked=false;
            process(d,rf,length,0,&f);assert(f.status.color_locked);
            f.frames=0;f.last_end=0;f.ever_colour_locked=false;
            size_t prefix=length/5u;uint64_t offset=(uint64_t)length+1234568u;
            process(d,rf,prefix,offset,&f);
            gui_rf_decode_status_t status;gui_rf_decoder_get_status(d,&status);
            assert(!status.color_locked && !f.ever_colour_locked);
            process(d,rf+prefix,length-prefix,offset+prefix,&f);
            check_bars(&f,"discontinuity reacquires references");
        }
        free(rf);gui_rf_decoder_destroy(d);
    }
    /* Stale reference identity must expire even if saturated active video
     * remains loud and looks like an opposite component's rest frequency. */
    for(int colour=1;colour<=3;colour++) {
        mesecam_source_t source=ordinary;
        source.fields=48;source.ident_end_field=8;source.solid_colour=colour;source.solid_start_field=8;
        gui_rf_decoder_t *d=gui_rf_decoder_create_profile(GUI_RF_PROFILE_MESECAM_VHS);assert(d);
        f.frames=0;f.last_end=0;f.ever_colour_locked=false;
        size_t length;float *rf=make_source(&source,&length);
        process(d,rf,length,0,&f);check_solid(&f,&source,"stale H identity expires");
        free(rf);gui_rf_decoder_destroy(d);
    }
    for(int malformed=0;malformed<6;malformed++) {
        mesecam_source_t source=ordinary;const char *name;
        if(malformed==0){name="missing first EQ";source.missing_eq=1;}
        else if(malformed==1){name="missing middle EQ";source.missing_eq=3;}
        else if(malformed==2){name="true repeated field parity";source.repeat_parity=true;}
        else if(malformed==3){name="wrong vertical cadence";source.bad_cadence=true;}
        else if(malformed==4){name="borderline first pulse plus missing second EQ";
            source.first_eq_extra_us=1.25;source.widen_mask=3;source.missing_eq=2;}
        else {name="borderline first pulse with true repeated parity";
            source.first_eq_extra_us=1.25;source.widen_mask=3;source.repeat_parity=true;}
        gui_rf_decoder_t *d=gui_rf_decoder_create_profile(GUI_RF_PROFILE_MESECAM_VHS);assert(d);
        f.frames=0;f.last_end=0;f.ever_colour_locked=false;
        size_t length;float *rf=make_source(&source,&length);
        process(d,rf,length,0,&f);gui_rf_decode_status_t status;gui_rf_decoder_get_status(d,&status);
        printf("MESECAM rejects %s: frames%d accepted%llu parity%llu cadence%llu\n",name,
            f.frames,(unsigned long long)status.fields_accepted,
            (unsigned long long)status.reject_parity,(unsigned long long)status.reject_cadence);fflush(stdout);
        assert(f.frames==0 && status.fields_accepted==0 && !f.ever_colour_locked);
        if(malformed==0 || malformed==2 || malformed==5)assert(status.reject_parity>0);
        else assert(status.reject_cadence>0 || status.reject_parity>0);
        free(rf);gui_rf_decoder_destroy(d);
    }
    check_carrier_guards();
    free(f.rgba);
    printf("MESECAM physical FM contracts PASS; measured process CPU %.3fs / RF %.3fs (%.3fx), generator excluded\n",
        decode_seconds,rf_seconds,decode_seconds/rf_seconds);
    return 0;
}
