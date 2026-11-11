#include "../misrc_gui/signal/gui_rf_resampler.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LIBSOXR_ENABLED
#define LIBSOXR_ENABLED 0
#endif

#define PI 3.14159265358979323846
#define SAMPLE_COUNT 131072u

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s (%d)\n", message, __LINE__); ++failures; } \
} while (0)

static size_t convert_mode(uint32_t rate, uint32_t output_rate, const int16_t *input,
                           size_t count, bool partitioned, float *output, bool ed_beta) {
    gui_rf_resampler_t *resampler = ed_beta ? gui_rf_resampler_create_ed_beta(rate)
        : gui_rf_resampler_create_rate(rate, output_rate);
    CHECK(resampler != NULL, "create supported converter");
    if (!resampler) return 0;
    size_t consumed = 0;
    size_t produced = 0;
    const size_t partitions[] = {1, 3, 257, 2, 4093, 31, 2048, 17};
    size_t partition_index = 0;
    while (consumed < count) {
        size_t chunk = partitioned ? partitions[partition_index++ % 8u] : count;
        if (chunk > count - consumed) chunk = count - consumed;
        size_t capacity = ed_beta ? gui_rf_resampler_output_capacity_ed_beta(rate, chunk)
            : gui_rf_resampler_output_capacity_rate(rate, output_rate, chunk);
        float *scratch = calloc(capacity ? capacity : 1u, sizeof(*scratch));
        CHECK(scratch != NULL, "allocate output");
        if (!scratch) break;
        size_t output_count = 0;
        CHECK(gui_rf_resampler_process(resampler, input + consumed, chunk,
              scratch, capacity, &output_count), "consume entire input chunk");
        CHECK(output_count <= capacity, "output capacity bound");
        if (output_count) memcpy(output + produced, scratch, output_count * sizeof(*output));
        produced += output_count;
        consumed += chunk;
        free(scratch);
    }
    CHECK(isfinite(gui_rf_resampler_delay_samples(resampler)), "finite delay");
    gui_rf_resampler_destroy(resampler);
    return produced;
}

static size_t convert_rate(uint32_t rate, uint32_t output_rate, const int16_t *input,
                           size_t count, bool partitioned, float *output) {
    return convert_mode(rate,output_rate,input,count,partitioned,output,false);
}

static size_t convert(uint32_t rate, const int16_t *input, size_t count,
                      bool partitioned, float *output) {
    return convert_rate(rate, 20000000u, input, count, partitioned, output);
}

static void partition_and_reset(uint32_t rate) {
    int16_t *input = malloc(SAMPLE_COUNT * sizeof(*input));
    float *whole = calloc(SAMPLE_COUNT * 2u, sizeof(*whole));
    float *split = calloc(SAMPLE_COUNT * 2u, sizeof(*split));
    CHECK(input && whole && split, "allocate equivalence fixture");
    if (!input || !whole || !split) goto cleanup;
    uint32_t random = 0x9183abcdu;
    for (size_t index = 0; index < SAMPLE_COUNT; ++index) {
        random = random * 1664525u + 1013904223u;
        input[index] = (int16_t)(random >> 16);
    }
    size_t n_whole = convert(rate, input, SAMPLE_COUNT, false, whole);
    size_t n_split = convert(rate, input, SAMPLE_COUNT, true, split);
    CHECK(n_whole == n_split, "chunk partition preserves output count");
    if (n_whole == n_split) {
        float maximum_error = 0.0f;
        for (size_t index = 0; index < n_whole; ++index) {
            float error = fabsf(whole[index] - split[index]);
            if (error > maximum_error) maximum_error = error;
        }
        CHECK(maximum_error < 2e-6f, "chunk partition preserves samples");
    }

    gui_rf_resampler_t *resampler = gui_rf_resampler_create(rate);
    size_t capacity = gui_rf_resampler_output_capacity(rate, SAMPLE_COUNT);
    float *scratch = calloc(capacity ? capacity : 1u, sizeof(*scratch));
    CHECK(resampler && scratch, "allocate reset fixture");
    if (resampler && scratch) {
        size_t first_count = 0;
        size_t reset_count = 0;
        CHECK(gui_rf_resampler_process(resampler, input, SAMPLE_COUNT,
              scratch, capacity, &first_count), "initial process");
        gui_rf_resampler_reset(resampler);
        CHECK(gui_rf_resampler_process(resampler, input, SAMPLE_COUNT,
              split, capacity, &reset_count), "process after reset");
        CHECK(first_count == reset_count, "reset restores initial output count");
        CHECK(first_count == reset_count &&
              memcmp(scratch, split, first_count * sizeof(*scratch)) == 0,
              "reset restores history and phase");
    }
    free(scratch);
    gui_rf_resampler_destroy(resampler);
cleanup:
    free(input);
    free(whole);
    free(split);
}

static double tone_gain_mode(uint32_t rate, uint32_t output_rate, double frequency, bool ed_beta) {
    int16_t *input = malloc(SAMPLE_COUNT * sizeof(*input));
    float *output = calloc(SAMPLE_COUNT * 2u, sizeof(*output));
    CHECK(input && output, "allocate tone fixture");
    if (!input || !output) { free(input); free(output); return 0.0; }
    for (size_t index = 0; index < SAMPLE_COUNT; ++index)
        input[index] = (int16_t)lrint(16384.0 * sin(2.0 * PI * frequency * index / rate));
    size_t count = convert_mode(rate, output_rate, input, SAMPLE_COUNT, true, output, ed_beta);
    CHECK(count > 2048u, "enough settled tone samples");
    double sum = 0.0;
    size_t begin = 1024u;
    size_t end = count > 1024u ? count - 1024u : 0;
    for (size_t index = begin; index < end; ++index) sum += output[index] * output[index];
    double gain = end > begin ? sqrt(sum / (end - begin)) / (0.5 / sqrt(2.0)) : 0.0;
    free(input);
    free(output);
    return gain;
}

static double tone_gain_rate(uint32_t rate, uint32_t output_rate, double frequency) {
    return tone_gain_mode(rate,output_rate,frequency,false);
}

static double tone_gain(uint32_t rate, double frequency) {
    return tone_gain_rate(rate, 20000000u, frequency);
}

static void dc_and_frequency(uint32_t rate, double stop_frequency) {
    int16_t *input = malloc(SAMPLE_COUNT * sizeof(*input));
    float *output = calloc(SAMPLE_COUNT * 2u, sizeof(*output));
    CHECK(input && output, "allocate DC fixture");
    if (!input || !output) { free(input); free(output); return; }
    for (size_t index = 0; index < SAMPLE_COUNT; ++index) input[index] = 8192;
    size_t count = convert(rate, input, SAMPLE_COUNT, true, output);
    double maximum_error = 0.0;
    for (size_t index = 2048u; index < count; ++index) {
        double error = fabs(output[index] - 0.25);
        if (error > maximum_error) maximum_error = error;
    }
    CHECK(count > 2048u && maximum_error < 2e-5, "unity DC gain after settling");
    free(input);
    free(output);
    double pass_gain = tone_gain(rate, 7500000.0);
    CHECK(pass_gain > 0.96 && pass_gain < 1.02, "retain 7.5 MHz VHS RF");
    if (stop_frequency > 0.0) {
        double stop_gain = tone_gain(rate, stop_frequency);
        CHECK(stop_gain < 0.025, "reject RF above output Nyquist before decimation");
    }
}

static void bypass_and_contract(void) {
    gui_rf_resampler_t *resampler = gui_rf_resampler_create(20000000u);
    const int16_t input[] = {-32768, -16384, -1, 0, 1, 16384, 32767};
    float output[7];
    size_t count = 99u;
    CHECK(resampler != NULL, "20 MSPS converter");
    CHECK(gui_rf_resampler_process(resampler, input, 7u, output, 6u, &count) == false
          && count == 0u, "small output rejected before consuming input");
    CHECK(gui_rf_resampler_process(resampler, input, 7u, output, 7u, &count)
          && count == 7u, "20 MSPS bypass count");
    for (size_t index = 0; index < 7u; ++index)
        CHECK(output[index] == (float)input[index] / 32768.0f, "exact normalized bypass");
    CHECK(gui_rf_resampler_delay_samples(resampler) == 0.0, "bypass delay");
    CHECK(gui_rf_resampler_process(resampler, NULL, 0, NULL, 0, &count) && count == 0,
          "empty chunk is not end of stream");
    gui_rf_resampler_destroy(resampler);
    CHECK(gui_rf_resampler_create(0) == NULL, "reject zero rate");
    CHECK(gui_rf_resampler_create(15000000u) == NULL, "reject insufficient input band");
    CHECK(gui_rf_resampler_create(201000000u) == NULL, "reject rate outside contract");
    CHECK(gui_rf_resampler_output_capacity(16000000u, SIZE_MAX) == 0,
          "oversized output bound cannot wrap");
#if !LIBSOXR_ENABLED
    CHECK(gui_rf_resampler_create(25000000u) == NULL, "arbitrary rate requires soxr");
    CHECK(gui_rf_resampler_output_capacity(25000000u, 1000) == 0,
          "unsupported rate has no output bound");
#endif
}

static void impulse_delay_rate(uint32_t rate, uint32_t output_rate, unsigned factor, double expected_delay) {
    int16_t input[256] = {0};
    input[64] = 16384;
    float output[256] = {0};
    gui_rf_resampler_t *resampler = gui_rf_resampler_create_rate(rate, output_rate);
    size_t count = 0;
    CHECK(resampler != NULL, "impulse converter");
    CHECK(gui_rf_resampler_delay_samples(resampler) == expected_delay, "reported FIR group delay");
    CHECK(gui_rf_resampler_process(resampler, input, 256, output, 256, &count), "impulse process");
    CHECK(count == 256u / factor, "halfband count follows fixed phase");
    double total = 0.0;
    double moment = 0.0;
    for (size_t index = 0; index < count; ++index) {
        total += output[index];
        moment += index * output[index];
    }
    CHECK(fabs(total - 0.5 / factor) < 2e-5, "impulse gain matches decimation factor");
    CHECK(fabs(moment / total - (64.0 / factor + expected_delay)) < 2e-4,
          "impulse sample position matches causal group delay");
    /* Odd chunk sizes must retain the decimation phase across buffer edges. */
    gui_rf_resampler_reset(resampler);
    size_t first = 0;
    size_t second = 0;
    CHECK(gui_rf_resampler_process(resampler, input, 65, output, 256, &first), "odd first chunk");
    CHECK(gui_rf_resampler_process(resampler, input + 65, 191, output + first,
          256 - first, &second), "remaining chunk");
    CHECK(first + second == count, "phase survives odd chunk boundary");
    gui_rf_resampler_destroy(resampler);
}

static void impulse_delay(uint32_t rate, unsigned factor, double expected_delay) {
    impulse_delay_rate(rate, 20000000u, factor, expected_delay);
}

static void wide_rate_contract(uint32_t rate) {
    int16_t *input = malloc(SAMPLE_COUNT * sizeof(*input));
    float *whole = calloc(SAMPLE_COUNT * 2u, sizeof(*whole));
    float *split = calloc(SAMPLE_COUNT * 2u, sizeof(*split));
    CHECK(input && whole && split, "allocate 40 MSPS contract");
    if (!input || !whole || !split) goto cleanup;
    uint32_t random = 0x76ab192du;
    for (size_t n=0;n<SAMPLE_COUNT;n++) {
        random=random*1664525u+1013904223u;input[n]=(int16_t)(random>>16);
    }
    size_t count=convert_rate(rate,40000000u,input,SAMPLE_COUNT,false,whole);
    size_t partitioned=convert_rate(rate,40000000u,input,SAMPLE_COUNT,true,split);
    CHECK(count==partitioned,"40 MSPS partition count is invariant");
    double error=0;
    if(count==partitioned) for(size_t n=0;n<count;n++) error=fmax(error,fabs(whole[n]-split[n]));
    CHECK(count>0 && error<2e-6,"40 MSPS partitioned samples agree");
    if(rate==40000000u) {
        CHECK(count==SAMPLE_COUNT,"40 MSPS bypass never halves video");
        for(size_t n=0;n<count;n++)
            CHECK(whole[n]==(float)input[n]/32768.0f,"40 MSPS bypass preserves every normalized sample");
    }
    gui_rf_resampler_t *r=gui_rf_resampler_create_rate(rate,40000000u);
    size_t capacity=gui_rf_resampler_output_capacity_rate(rate,40000000u,SAMPLE_COUNT),first=0,again=0;
    CHECK(r!=NULL && capacity<=SAMPLE_COUNT*2u,"40 MSPS reset allocation bound");
    if(r) {
        CHECK(gui_rf_resampler_process(r,input,SAMPLE_COUNT,whole,capacity,&first),"40 MSPS initial reset fixture");
        gui_rf_resampler_reset(r);
        CHECK(gui_rf_resampler_process(r,input,SAMPLE_COUNT,split,capacity,&again),"40 MSPS process after reset");
        CHECK(first==again && memcmp(whole,split,first*sizeof(*whole))==0,"40 MSPS reset restores state and phase");
    }
    gui_rf_resampler_destroy(r);
    double gain=tone_gain_rate(rate,40000000u,9300000.0);
    CHECK(gain>.98 && gain<1.02,"wide front-end preserves the 9.3 MHz ED white carrier");
    if(rate>=54000000u)
        CHECK(tone_gain_rate(rate,40000000u,25000000.0)<.025,"wide front-end rejects RF above 20 MHz Nyquist");
cleanup:
    free(input);free(whole);free(split);
}

static void compatibility_wrapper(uint32_t rate) {
    const size_t n=8192;
    int16_t *input=malloc(n*sizeof(*input));
    size_t capacity=gui_rf_resampler_output_capacity(rate,n);
    float *a=calloc(capacity?capacity:1,sizeof(*a)),*b=calloc(capacity?capacity:1,sizeof(*b));
    gui_rf_resampler_t *old=gui_rf_resampler_create(rate),*explicit=gui_rf_resampler_create_rate(rate,20000000u);
    CHECK(input && a && b && old && explicit,"allocate compatibility wrapper fixture");
    if(input && a && b && old && explicit) {
        for(size_t i=0;i<n;i++)input[i]=(int16_t)((i*117u)%65536u-32768);
        CHECK(capacity==gui_rf_resampler_output_capacity_rate(rate,20000000u,n),"20 MSPS capacity wrapper unchanged");
        size_t na=0,nb=0;
        CHECK(gui_rf_resampler_process(old,input,n,a,capacity,&na) &&
              gui_rf_resampler_process(explicit,input,n,b,capacity,&nb),"20 MSPS wrapper process");
        CHECK(na==nb && memcmp(a,b,na*sizeof(*a))==0,"legacy and explicit 20 MSPS outputs are bit-identical");
    }
    free(input);free(a);free(b);gui_rf_resampler_destroy(old);gui_rf_resampler_destroy(explicit);
}

/* ED's preview converter must retain white-carrier RF instead of selecting
 * the legacy halfband merely because both targets now have the same rate. */
static void ed_beta_contract(uint32_t rate) {
    int16_t *input=malloc(SAMPLE_COUNT*sizeof(*input));
    float *whole=calloc(SAMPLE_COUNT*2u,sizeof(*whole));
    float *split=calloc(SAMPLE_COUNT*2u,sizeof(*split));
    CHECK(input && whole && split,"allocate ED20 converter fixture");
    if(!input || !whole || !split)goto cleanup;
    uint32_t random=0x4d70b182u;
    for(size_t n=0;n<SAMPLE_COUNT;n++) {
        random=random*1664525u+1013904223u;input[n]=(int16_t)(random>>16);
    }
    size_t count=convert_mode(rate,20000000u,input,SAMPLE_COUNT,false,whole,true);
    size_t partitioned=convert_mode(rate,20000000u,input,SAMPLE_COUNT,true,split,true);
    CHECK(count>0 && count==partitioned,"ED20 arbitrary partitions preserve count");
    double error=0;
    if(count==partitioned)for(size_t n=0;n<count;n++)error=fmax(error,fabs(whole[n]-split[n]));
    CHECK(error<2e-6,"ED20 partitions preserve samples and buffered history");
    if(rate==20000000u) {
        CHECK(count==SAMPLE_COUNT,"native20 ED bypass retains every sample");
        for(size_t n=0;n<count;n++)
            CHECK(whole[n]==(float)input[n]/32768.0f,"native20 ED bypass is bit exact");
    }
    gui_rf_resampler_t *r=gui_rf_resampler_create_ed_beta(rate);
    size_t capacity=gui_rf_resampler_output_capacity_ed_beta(rate,SAMPLE_COUNT),first=0,again=0;
    CHECK(r && capacity<=SAMPLE_COUNT*2u,"ED20 safe allocation bound");
    if(r) {
        CHECK(gui_rf_resampler_process(r,input,SAMPLE_COUNT,whole,capacity,&first),"initial ED20 process");
        gui_rf_resampler_reset(r);
        CHECK(gui_rf_resampler_process(r,input,SAMPLE_COUNT,split,capacity,&again),"ED20 process after reset");
        CHECK(first==again && !memcmp(whole,split,first*sizeof(*whole)),"ED20 reset restores history and phase");
        CHECK(gui_rf_resampler_delay_samples(r)==0.0,"ED20 bypass/soxr alignment excludes availability latency");
    }
    gui_rf_resampler_destroy(r);
    double gain=tone_gain_mode(rate,20000000u,9300000.0,true);
    CHECK(gain>.98 && gain<1.02,"ED20 retains9.3MHz white carrier");
    if(rate>20000000u)
        CHECK(tone_gain_mode(rate,20000000u,11000000.0,true)<.025,"ED20 rejects RF above10MHz Nyquist");
cleanup:
    free(input);free(whole);free(split);
}

#if LIBSOXR_ENABLED
static void test_delayed_eof_output(void) {
    enum { input_count = 16384, expected = 10240 };
    int16_t input[input_count] = {0};
    float output[expected + 8192];
    gui_rf_resampler_t *r = gui_rf_resampler_create(32000000u);
    CHECK(r != NULL, "SOXR EOF fixture allocation");
    if (!r) return;
    size_t produced = 0, tail = 0;
    CHECK(gui_rf_resampler_process(r, input, input_count, output,
                                 sizeof(output) / sizeof(*output), &produced), "SOXR normal input");
    CHECK(gui_rf_resampler_process(r, NULL, 0, NULL, 0, &tail) && tail == 0,
          "empty input does not finish the stream");
    CHECK(gui_rf_resampler_finish(r, output, sizeof(output) / sizeof(*output), &tail) &&
          tail > 0 && produced + tail == expected, "SOXR EOF restores the exact output count");
    CHECK(gui_rf_resampler_finish(r, output, sizeof(output) / sizeof(*output), &tail) && tail == 0,
          "SOXR does not repeat the final samples");
    gui_rf_resampler_reset(r);
    size_t again = 0;
    CHECK(gui_rf_resampler_process(r, input, input_count, output,
                                 sizeof(output) / sizeof(*output), &again) && again == produced,
          "SOXR reset starts a fresh stream after EOF");
    gui_rf_resampler_destroy(r);
}
#endif

int main(void) {
    bypass_and_contract();
    CHECK(gui_rf_resampler_create_ed_beta(16000000u)==NULL,"ED20 rejects insufficient input bandwidth");
    CHECK(gui_rf_resampler_output_capacity_ed_beta(16000000u,100)==0,"invalid ED input has no capacity");
    ed_beta_contract(20000000u);
#if LIBSOXR_ENABLED
    size_t huge=gui_rf_resampler_output_capacity_ed_beta(40000000u,SIZE_MAX);
    CHECK(huge>SIZE_MAX/2u && huge<SIZE_MAX,"ED20 downconversion capacity cannot wrap");
    ed_beta_contract(40000000u);ed_beta_contract(54000000u);ed_beta_contract(80000000u);
#else
    CHECK(gui_rf_resampler_create_ed_beta(40000000u)==NULL,"ED20 downconversion needs soxr");
    CHECK(gui_rf_resampler_output_capacity_ed_beta(40000000u,100)==0,"unavailable ED converter has no capacity");
#endif
    CHECK(gui_rf_resampler_create_rate(40000000u,30000000u)==NULL,"reject unsupported target rate");
    CHECK(gui_rf_resampler_output_capacity_rate(40000000u,30000000u,100)==0,"invalid target has no bound");
    CHECK(gui_rf_resampler_output_capacity_rate(16000000u,40000000u,SIZE_MAX)==0,"wide output capacity cannot overflow");
    const uint32_t wide_exact[]={40000000u,80000000u,160000000u};
    for(size_t n=0;n<3;n++)wide_rate_contract(wide_exact[n]);
    impulse_delay_rate(80000000u,40000000u,2u,7.5);
    impulse_delay_rate(160000000u,40000000u,4u,11.25);
    compatibility_wrapper(20000000u);compatibility_wrapper(40000000u);compatibility_wrapper(80000000u);
    const uint32_t exact_rates[] = {20000000u, 40000000u, 80000000u};
    for (size_t index = 0; index < 3u; ++index) {
        partition_and_reset(exact_rates[index]);
        dc_and_frequency(exact_rates[index], index ? 12500000.0 : 0.0);
    }
    impulse_delay(40000000u, 2u, 7.5);
    impulse_delay(80000000u, 4u, 11.25);
    CHECK(tone_gain(80000000u, 30000000.0) < 0.025, "first 80 MSPS stage rejects high RF");
#if LIBSOXR_ENABLED
    test_delayed_eof_output();
    wide_rate_contract(32000000u);wide_rate_contract(54000000u);wide_rate_contract(200000000u);
    compatibility_wrapper(28636363u);
    const uint32_t rates[] = {16000000u, 25000000u, 28636363u, 54000000u, 160000000u, 200000000u};
    for (size_t index = 0; index < 6u; ++index) {
        partition_and_reset(rates[index]);
        dc_and_frequency(rates[index], index ? 11000000.0 : 0.0);
    }
#endif
    if (failures) { fprintf(stderr, "%d RF resampler checks failed\n", failures); return 1; }
    puts("RF resampler continuity, gain, anti-aliasing, phase and reset checks passed");
    return 0;
}
