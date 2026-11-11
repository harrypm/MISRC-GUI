/* Physical native-SECAM VHS RF contracts, not hardware/media qualification.
 * Generator constants come from ITU-R BT.470-6 Table 2 and the JVC Video
 * Technical Guide 3.1.4: frequency-modulated line-sequential D'R/D'B is
 * divided by four on tape. No decoder filters, colour conversion helpers,
 * pre-decoded planes, or PAL colour-under phase rules enter this oracle. */
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
    double line_chroma_noise;
    int dr_first, swapped_heads, solid_colour, solid_start_field, fields, ident_end_field;
    bool gray, chroma_tone, field_ident, no_line_ident, spatial_pattern;
} secam_source_t;

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
static float *make_source(const secam_source_t *source, size_t *length) {
    const int fields=source->fields?source->fields:FIELD_COUNT;
    assert(fields>=FIELD_COUNT && fields<=64);
    int starts[65];starts[0]=0;
    for(int f=0;f<fields;f++) starts[f+1]=starts[f]+((f&1)?313:312);
    const int half_lines=starts[fields]*2;
    double *timeline=malloc((size_t)(half_lines+1)*sizeof(*timeline));
    assert(timeline);
    for(int h=0;h<=half_lines;h++)
        timeline[h]=h*32.0*source->clock_scale
            +source->jitter_us*sin(h*2.0*TEST_PI/23.0);
    *length=(size_t)floor(timeline[half_lines]*20.0);
    float *rf=malloc(*length*sizeof(*rf));assert(rf);

    /* Independent analog record networks. Luma uses the VHS record shelf
     * (1+s*400ns)/(1+s*100ns). SECAM LF pre-correction has DC gain one,
     * HF gain three, zero85kHz/pole255kHz; an exact one-pole state integrates
     * it causally. The playback test must undo the record network itself. */
    const double k=2.0*SAMPLE_RATE;
    const double luma_b0=(1.0+k*400e-9)/(1.0+k*100e-9);
    const double luma_b1=(1.0-k*400e-9)/(1.0+k*100e-9);
    const double luma_a1=(1.0-k*100e-9)/(1.0+k*100e-9);
    double last_luma=0.0,last_pre=0.0,colour_state=0.0;
    double luma_phase=.31,chroma_phase=.79,last_time=0.0;
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
        const double eq_start=(field&1)?64.0:32.0;
        const double broad_start=eq_start+160.0;
        const double eq2_start=broad_start+160.0;
        const double vertical_end=eq2_start+160.0;
        bool sync;
        if(local<eq_start || local>=vertical_end) sync=x<4.7;
        else {
            double start=local<broad_start?eq_start:local<eq2_start?broad_start:eq2_start;
            double width=local>=broad_start && local<eq2_start?27.3:2.35;
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
            if(source->chroma_tone) {
                /* Independent 85kHz chroma modulation with mathematically
                 * constant physical luma; no decoder phase/delay is used. */
                double angle=2.0*TEST_PI*85000.0*(x-10.5)*1e-6;
                r=.5+.22*sin(angle);b=.5+.16*cos(angle);
                g=(.5-.299*r-.114*b)/.587;
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
        const double y=.299*r+.587*g+.114*b;
        const double raw_luma=sync?-300000.0:700000.0*y;
        const double pre_luma=luma_b0*raw_luma+luma_b1*last_luma-luma_a1*last_pre;
        last_luma=raw_luma;last_pre=pre_luma;
        const double dt=(ideal_us-last_time)*1e-6;
        last_time=ideal_us;
        double luma_hz=limit(4100000.0+source->luma_offset+pre_luma,2400000.0,6500000.0);
        luma_phase+=2.0*TEST_PI*luma_hz*dt;

        /* D'R has negative polarity for R-Y. Both rest frequency and FM
         * deviation divide by four, while the colour-difference waveform
         * keeps its original temporal rate. The divider makes constant
         * carrier amplitude; there is no PAL burst or head phase rotation. */
        const bool dr=((starts[field]+line+source->dr_first)&1)!=0;
        double d=active?(dr?-1.902*(r-y):1.505*(b-y)):0.0;
        if(source->spatial_pattern) {
            if(active) {
                /* A deterministic line-correlated FM frequency error is
                 * recorded BEFORE the analog SECAM pre-correction. It
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
        /* Documented field-identification option: SECAM lines7..15 carry
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
        if(field!=last_field) {
            /* Independent head phases: reversing heads must not swap Dr/Db
             * or require PAL's line-by-line quadrature correction. */
            int head=(field&1)^source->swapped_heads;
            chroma_phase+=head?2.17:.71;last_field=field;
        }
        chroma_phase+=2.0*TEST_PI*(studio_hz/4.0)*dt;
        /* H identification is the undeviated carrier before picture. The
         * no-ID negative removes this AND the optional field bottles.
         * Remaining VBI lines carry no chroma per BT.470-6 Table2.17. */
        bool carrier=(line>=22 && x>=(source->no_line_ident?10.5:5.6) && x<62.5)
            || (bottle && x>=5.6 && x<62.5 && !sync);
        if(source->spatial_pattern && (line==20 || line==21))
            carrier=x>=5.6 && x<62.5 && !sync;
        if(source->spatial_pattern && line==208)carrier=false;
        double envelope=carrier?1.0:0.0;
        rf[n]=(float)(source->rf_gain*(.72*cos(luma_phase)
            +source->chroma_amplitude*envelope*cos(chroma_phase)));
        if(luma_phase>2.0*TEST_PI)luma_phase=fmod(luma_phase,2.0*TEST_PI);
        if(chroma_phase>2.0*TEST_PI)chroma_phase=fmod(chroma_phase,2.0*TEST_PI);
    }
    free(timeline);return rf;
}

static void process(gui_rf_decoder_t *decoder,const float *rf,size_t length,
                    uint64_t offset,frames_t *frames) {
    size_t position=0;
    while(position<length) {
        size_t count=(position&1)?32771u:4093u;
        if(count>length-position)count=length-position;
        gui_rf_decoder_process(decoder,rf+position,count,offset+position,receive,frames);
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
    printf("SECAM %s: frames%d fields%llu rejects%llu H%d V%d C%d, %s\n",name,
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
        printf("SECAM %s field%d red/blue edge Y%d C%d separation%d pixels\n",
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
    printf("SECAM %s physical RGB maximum error %.4f\n",name,max_error);fflush(stdout);
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
    printf("SECAM %s neutral RGB channel spread %.4f\n",name,highest);fflush(stdout);
    assert(highest<.035);
}

static void check_solid(const frames_t *f,const secam_source_t *source,const char *name) {
    check_sync(f,name);
    const bool identified=source->field_ident && !source->ident_end_field;
    assert(f->status.color_locked==identified);
    if(source->ident_end_field)assert(f->ever_colour_locked);
    double largest=0.0,spread=0.0;
    int colour=3+source->solid_colour;
    for(int parity=0;parity<2;parity++) {
        double rgb[3];
        for(int c=0;c<3;c++) {
            rgb[c]=bar_mean(f,3,parity,c);
            if(identified)largest=fmax(largest,fabs(rgb[c]-colours[colour][c]));
        }
        spread=fmax(spread,fmax(rgb[0],fmax(rgb[1],rgb[2]))-fmin(rgb[0],fmin(rgb[1],rgb[2])));
    }
    printf("SECAM %s: solidRGBerror%.4f channelspread%.4f identified%d\n",name,largest,spread,identified);
    fflush(stdout);
    if(identified)assert(largest<.08);
    else assert(spread<.035);
}

/* Free-phase harmonic regression separates transfer magnitude from timing.
 * This is an output-signal measurement against physical RGB amplitudes,
 * independent of the receiver's filter design or coefficient helpers. */
static double tone_amplitude(const frames_t *f,int parity,int channel) {
    double matrix[3][4]={{0}};
    for(int x=f->width/8;x<f->width*7/8;x++) {
        double rgb[3]={0};int count=0;
        for(int row=100+parity;row<300;row+=2) {
            for(int c=0;c<3;c++)rgb[c]+=f->rgba[((size_t)row*f->width+x)*4u+c]/255.0;
            count++;
        }
        for(int c=0;c<3;c++)rgb[c]/=count;
        double value=channel<3?rgb[channel]:.299*rgb[0]+.587*rgb[1]+.114*rgb[2];
        double angle=2.0*TEST_PI*85000.0*52e-6*(x+.5)/f->width;
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
        double r=tone_amplitude(f,parity,0),b=tone_amplitude(f,parity,2);
        double y=tone_amplitude(f,parity,3);
        printf("SECAM field%d 85kHz RGB amplitudes R%.5f B%.5f, luma%.5f\n",parity,r,b,y);
        fflush(stdout);
        /* Omitting LF de-emphasis leaves the record network's ~1.34 gain
         * at 85kHz and fails this bound by a wide margin. */
        assert(fabs(r-.22)<.025 && fabs(b-.16)<.025 && y<.015);
    }
}

static void check_full_frame(gui_rf_decoder_t *decoder,const float *rf,size_t length,
                             frames_t *frames) {
    gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_SECAM_VHS);
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
    printf("SECAM Full: %dx%d, opaque written rows, empty624, bottom622/625 hue and weave preserved\n",
        frames->width,frames->height);fflush(stdout);
}

/* Measure final RGB against physical line coordinates and constant-luma
 * source values. There are no receiver planes, coefficients or filter
 * formulas in these expectations. Averaging a broad horizontal patch
 * removes RF-scale ripple while retaining individual physical lines. */
static void spatial_line(const frames_t *f,int line,int parity,double result[3]) {
    int first=f->height==576?22:0;
    int row=(line-first)*2+parity,count=0;
    double rgb[3]={0};assert(row>=0 && row<f->height);
    int x0=f->height==576?f->width/3:f->width*2/5;
    int x1=f->height==576?f->width*2/3:f->width*3/5;
    for(int x=x0;x<x1;x++) {
        const uint8_t *pixel=f->rgba+((size_t)row*f->width+x)*4u;
        assert(pixel[3]==255);
        for(int c=0;c<3;c++)rgb[c]+=pixel[c]/255.0;
        count++;
    }
    for(int c=0;c<3;c++)rgb[c]/=count;
    result[0]=.299*rgb[0]+.587*rgb[1]+.114*rgb[2];
    result[1]=rgb[0]-result[0];result[2]=rgb[2]-result[0];
}

static void check_spatial(const frames_t *f,bool full) {
    assert(f->status.color_locked && f->status.h_locked && f->status.v_locked);
    assert(f->width==768 && f->height==(full?626:576));
    double field_luma[2]={0};
    for(int parity=0;parity<2;parity++) {
        double pixel[3];spatial_line(f,110,parity,pixel);
        field_luma[parity]=pixel[0];
        double sign=pixel[0]>.5?1.0:-1.0;
        double low=1.0,high=0.0;
        for(int line=40;line<280;line++) {
            spatial_line(f,line,parity,pixel);
            low=fmin(low,pixel[0]);high=fmax(high,pixel[0]);
        }
        /* Generic neighbouring-row RGB smoothing would erase the large
         * physical field-luma difference, even though each field is flat. */
        assert(high-low<.018);
        if(!full) {
            double centre[3],next[3];
            spatial_line(f,88,parity,centre);spatial_line(f,89,parity,next);
            printf("SECAM spatial field%d physical B-Y line88 %.5f line89 %.5f\n",
                parity,centre[2],next[2]);fflush(stdout);
            /* The physical B-Y ramp crosses zero on line88 and reaches
             * +.02 on line89. Previous-line hold shifts one of these;
             * centred missing-component recovery preserves both. */
            assert(fabs(centre[2])<.009 && fabs(next[2]-sign*.02)<.009);
            double before[3],after[3];
            spatial_line(f,126,parity,before);spatial_line(f,130,parity,after);
            assert(fabs(before[1]+sign*.16)<.035);
            assert(fabs(after[1]-sign*.16)<.035);
        } else {
            /* The opposite-colour VBI interferer occupies20/21. Line22
             * is the first active row and must retain its own colour. */
            spatial_line(f,22,parity,pixel);
            printf("SECAM spatial Full field%d first active R-Y %.5f B-Y %.5f\n",
                parity,pixel[1],pixel[2]);fflush(stdout);
            assert(fabs(pixel[1]+sign*.16)<.04 && fabs(pixel[2]+sign*.16)<.04);
        }
        /* Complete loss of the physical colour carrier on line208 must
         * produce neutral RGB, including the otherwise recoverable C. */
        spatial_line(f,208,parity,pixel);
        assert(fabs(pixel[1])<.02 && fabs(pixel[2])<.02);
    }
    assert(fabs(field_luma[0]-field_luma[1])>.24);
}

static void check_line_noise(const frames_t *clean,const frames_t *noisy) {
    assert(noisy->status.color_locked && clean->width==noisy->width && clean->height==noisy->height);
    double squared[3]={0},largest[3]={0};int count=0;
    for(int parity=0;parity<2;parity++) for(int line=100;line<248;line++) {
        if((line>=122 && line<=134) || (line>=156 && line<=180) ||
            (line>=204 && line<=212))continue;
        double a[3],b[3];spatial_line(clean,line,parity,a);spatial_line(noisy,line,parity,b);
        for(int c=0;c<3;c++) {
            double difference=b[c]-a[c];
            squared[c]+=difference*difference;largest[c]=fmax(largest[c],fabs(difference));
        }
        count++;
    }
    double y=sqrt(squared[0]/count),r=sqrt(squared[1]/count),b=sqrt(squared[2]/count);
    printf("SECAM physical +/-0.025 line FM noise: Y RMS%.5f R-Y RMS%.5f B-Y RMS%.5f, C peak%.5f/%.5f\n",
        y,r,b,largest[1],largest[2]);fflush(stdout);
    /* Without spatial noise suppression the known .025 alternating input
     * produces approximately .018 RMS in each reconstructed colour plane.
     * The output must reduce it substantially without touching luma. */
    assert(y<.004 && r<.010 && b<.010);
}

static void check_physical_spatial_processing(void) {
    secam_source_t source={.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,
        .field_ident=true,.spatial_pattern=true};
    frames_t frames[2]={{0}};
    for(int index=0;index<2;index++) {
        gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(GUI_RF_PROFILE_SECAM_VHS);assert(decoder);
        frames[index].rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(frames[index].rgba);
        source.line_chroma_noise=index?.025:0.0;
        size_t length;float *rf=make_source(&source,&length);
        process(decoder,rf,length,0,frames+index);
        check_spatial(frames+index,false);
        if(index==1)check_line_noise(frames,frames+1);
        if(index==0) {
            frames_t full={0};full.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(full.rgba);
            gui_rf_decoder_reset_profile(decoder,GUI_RF_PROFILE_SECAM_VHS);
            gui_rf_decoder_set_frame_mode(decoder,true);
            process(decoder,rf,length,0,&full);check_spatial(&full,true);free(full.rgba);
        }
        free(rf);gui_rf_decoder_destroy(decoder);
    }
    for(int i=0;i<2;i++)free(frames[i].rgba);
}

int main(void) {
    const clock_t begin=clock();
    secam_source_t cases[]={
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.field_ident=true},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.dr_first=1,.swapped_heads=1,.field_ident=true},
        {.chroma_amplitude=.15,.rf_gain=.55,.clock_scale=1.003,.jitter_us=.25,.luma_offset=-100000,.field_ident=true},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1.008,.jitter_us=.15,.field_ident=true},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.chroma_tone=true,.field_ident=true},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.gray=true,.field_ident=true},
        {.rf_gain=1,.clock_scale=1,.gray=true},
        {.chroma_amplitude=.00001,.rf_gain=1,.clock_scale=1,.gray=true,.field_ident=true},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.field_ident=true,.solid_colour=1},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.field_ident=true,.solid_colour=2},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.field_ident=true,.solid_colour=3},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.field_ident=true,.solid_colour=1,.solid_start_field=4,.fields=44},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.field_ident=true,.solid_colour=2,.solid_start_field=4,.fields=44},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.field_ident=true,.solid_colour=3,.solid_start_field=4,.fields=44},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.no_line_ident=true,.solid_colour=1},
        {.chroma_amplitude=.15,.rf_gain=1,.clock_scale=1,.no_line_ident=true,.solid_colour=1,
            .field_ident=true,.ident_end_field=8,.fields=48}
    };
    const char *names[]={"bars","identity and heads reversed","clock gain offset",
        "0.8 percent clock offset","85kHz constant-luma chroma modulation",
        "gray with native carrier","gray without chroma","gray with weak chroma",
        "cold solid magenta","cold solid red","cold solid blue",
        "long solid magenta","long solid red","long solid blue","no references solid magenta",
        "identification expires during solid magenta"};
    for(size_t index=0;index<sizeof(cases)/sizeof(cases[0]);index++) {
        gui_rf_decoder_t *decoder=gui_rf_decoder_create_profile(GUI_RF_PROFILE_SECAM_VHS);assert(decoder);
        frames_t frames={0};
        frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(frames.rgba);
        size_t length;float *rf=make_source(cases+index,&length);
        process(decoder,rf,length,0,&frames);
        if(index<4)check_bars(&frames,names[index]);
        else if(index==4)check_tone(&frames);
        else if(index<8)check_gray(&frames,names[index],index==5);
        else check_solid(&frames,cases+index,names[index]);
        if(index==0) {
            /* A sample-index discontinuity has to discard stale H/V/C lock
             * before another field can appear, then recover from real RF. */
            uint64_t offset=(uint64_t)length+100003u;
            gui_rf_decoder_process(decoder,rf,1,offset,receive,&frames);
            gui_rf_decode_status_t lost;gui_rf_decoder_get_status(decoder,&lost);
            assert(!lost.h_locked && !lost.v_locked && !lost.color_locked);
            frames.frames=0;frames.last_end=0;
            process(decoder,rf+1,length-1,offset+1,&frames);
            check_bars(&frames,"discontinuity reacquisition");
            check_full_frame(decoder,rf,length,&frames);
        }
        free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
    }
    check_physical_spatial_processing();
    printf("Native SECAM VHS RF contracts passed in %.2f CPU seconds\n",
        (double)(clock()-begin)/CLOCKS_PER_SEC);
    return 0;
}
