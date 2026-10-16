/* Production async playback startup regression. Real libFLAC and playback
 * thread; controlled local reads and a raw sink replace hardware/extraction/UI.
 * Including the implementation lets test-only wrappers observe allocations and
 * callback userdata without adding production hooks. This is not capture proof.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#if LIBFFTW_ENABLED
#include <fftw3.h>
#endif
#include "../misrc_gui/input/gui_playback_length_cache.h"
#include "../common/file_utils.h"

static void *start_test_malloc(size_t bytes);
static void *start_test_calloc(size_t count,size_t bytes);
static void start_test_free(void *memory);
static FILE *start_test_open_read_utf8(const char *filepath);
static int start_test_fclose(FILE *file);
static size_t start_test_fread(void *memory,size_t size,size_t count,FILE *file);
static int start_test_ferror(FILE *file);
static bool start_test_cache_snapshot(const char *filepath,gui_playback_length_cache_key_t *key);
static FLAC__uint64 start_test_tail_total(FLAC__StreamDecoder *decoder);
static FLAC__StreamDecoder *start_test_decoder_new(void);
static void start_test_decoder_delete(FLAC__StreamDecoder *decoder);

static FLAC__StreamDecoderInitStatus start_test_decoder_init_stream(
    FLAC__StreamDecoder *decoder,FLAC__StreamDecoderReadCallback read_callback,
    FLAC__StreamDecoderSeekCallback seek_callback,FLAC__StreamDecoderTellCallback tell_callback,
    FLAC__StreamDecoderLengthCallback length_callback,FLAC__StreamDecoderEofCallback eof_callback,
    FLAC__StreamDecoderWriteCallback write_callback,
    FLAC__StreamDecoderMetadataCallback metadata_callback,
    FLAC__StreamDecoderErrorCallback error_callback,void *userdata);

#define malloc start_test_malloc
#define calloc start_test_calloc
#define free start_test_free
#define file_open_read_utf8 start_test_open_read_utf8
#define fclose start_test_fclose
#define fread start_test_fread
#define ferror start_test_ferror
#define gui_playback_length_cache_snapshot start_test_cache_snapshot
#define FLAC__stream_decoder_find_total_samples start_test_tail_total
#define FLAC__stream_decoder_new start_test_decoder_new
#define FLAC__stream_decoder_delete start_test_decoder_delete
#define FLAC__stream_decoder_init_stream start_test_decoder_init_stream
#include "../misrc_gui/input/gui_playback.c"
#undef gui_playback_length_cache_snapshot
#undef FLAC__stream_decoder_find_total_samples
#undef FLAC__stream_decoder_init_stream
#undef FLAC__stream_decoder_delete
#undef FLAC__stream_decoder_new
#undef fread
#undef ferror
#undef fclose
#undef file_open_read_utf8
#undef free
#undef calloc
#undef malloc

#define START_TEST_TIMEOUT_MS 5000u
#define START_TEST_SAMPLES 131072u
#define START_TEST_OTHER_SAMPLES (START_TEST_SAMPLES+RAW_BUFFER_SAMPLES)
#define START_TEST_BLOCK 4096u
#define START_TEST_SLOTS 1024u

static atomic_flag start_test_lock=ATOMIC_FLAG_INIT;
static void *start_test_allocations[START_TEST_SLOTS];
static atomic_uint start_test_live_allocations,start_test_live_decoders;
static atomic_uint start_test_live_files;
static atomic_bool start_test_init_failure_once;
static atomic_bool start_test_scan_malloc_failure_once;
static atomic_uint start_test_scan_read_failure_once;
static atomic_bool start_test_snapshot_failure_once;
static atomic_bool start_test_tail_disabled=true; // Legacy Unknown cases exercise bounded-tail miss.
static atomic_bool start_test_tail_delay_once;
static atomic_bool start_test_tail_hold_once,start_test_tail_entered,start_test_tail_release;
static _Atomic(FILE *) start_test_error_file;
static atomic_bool start_test_slow_next,start_test_read_entered,start_test_release_read;
static atomic_size_t start_test_slow_read_count;
static atomic_bool start_test_extract_failure,start_test_extract_running;
static atomic_uint start_test_sink_writes,start_test_callback_writes;
static atomic_bool start_test_sink_hold_once,start_test_sink_entered,start_test_sink_release;
static atomic_uint start_test_callback_writes_a,start_test_callback_writes_b;
static atomic_uint start_test_channel_mask=1;
static atomic_uint start_test_source_rate=20000000u;
static atomic_uint_fast64_t start_test_last_epoch[2];
static uint32_t start_test_sink[RAW_BUFFER_SAMPLES];
static gui_app_t start_test_app;

typedef struct {
    const FLAC__StreamDecoder *decoder;
    FLAC__StreamDecoderWriteCallback callback;
    void *userdata;
    FILE *owned_file;
} start_test_callback_registration_t;
static start_test_callback_registration_t start_test_callbacks[START_TEST_SLOTS];
typedef struct { FILE *file; bool decoder_owned; } start_test_file_t;
static start_test_file_t start_test_files[START_TEST_SLOTS];

static void start_test_acquire(void) {
    while(atomic_flag_test_and_set_explicit(&start_test_lock,memory_order_acquire)) {}
}
static void start_test_release(void) {
    atomic_flag_clear_explicit(&start_test_lock,memory_order_release);
}
static void *start_test_register_allocation(void *memory) {
    if (!memory) return NULL;
    start_test_acquire();
    unsigned slot=0;
    while(slot<START_TEST_SLOTS && start_test_allocations[slot]) ++slot;
    assert(slot<START_TEST_SLOTS);
    start_test_allocations[slot]=memory;
    atomic_fetch_add(&start_test_live_allocations,1);
    start_test_release();
    return memory;
}
static void *start_test_malloc(size_t bytes) {
    if(bytes==PLAYBACK_FRAME_SCAN_CHUNK_BYTES+64u &&
       atomic_exchange(&start_test_scan_malloc_failure_once,false)) return NULL;
    return start_test_register_allocation(malloc(bytes));
}
static void *start_test_calloc(size_t count,size_t bytes) {
    return start_test_register_allocation(calloc(count,bytes));
}
static void start_test_free(void *memory) {
    if (!memory) return;
    start_test_acquire();
    unsigned slot=0;
    while(slot<START_TEST_SLOTS && start_test_allocations[slot]!=memory) ++slot;
    assert(slot<START_TEST_SLOTS); /* Only implementation-owned allocations. */
    start_test_allocations[slot]=NULL;
    atomic_fetch_sub(&start_test_live_allocations,1);
    start_test_release();
    free(memory);
}
static FILE *start_test_open_read_utf8(const char *filepath) {
    FILE *file=file_open_read_utf8(filepath);
    if (!file) return NULL;
    start_test_acquire();
    unsigned slot=0;
    while(slot<START_TEST_SLOTS && start_test_files[slot].file) ++slot;
    assert(slot<START_TEST_SLOTS);
    start_test_files[slot]=(start_test_file_t){file,false};
    atomic_fetch_add(&start_test_live_files,1);
    start_test_release();
    return file;
}
static int start_test_fclose(FILE *file) {
    if(atomic_load(&start_test_error_file)==file) atomic_store(&start_test_error_file,NULL);
    start_test_acquire();
    unsigned slot=0;
    while(slot<START_TEST_SLOTS && start_test_files[slot].file!=file) ++slot;
    assert(slot<START_TEST_SLOTS && !start_test_files[slot].decoder_owned);
    memset(&start_test_files[slot],0,sizeof(start_test_files[slot]));
    atomic_fetch_sub(&start_test_live_files,1);
    start_test_release();
    return fclose(file);
}
static size_t start_test_fread(void *memory,size_t size,size_t count,FILE *file) {
    size_t match=atomic_load(&start_test_slow_read_count);
    if ((!match || count==match) && atomic_exchange(&start_test_slow_next,false)) {
        atomic_store(&start_test_read_entered,true);
        while(!atomic_load(&start_test_release_read)) thrd_sleep_ms(1);
    }
    if(count==PLAYBACK_FRAME_SCAN_CHUNK_BYTES) {
        unsigned failure=atomic_exchange(&start_test_scan_read_failure_once,0);
        if(failure) {
            atomic_store(&start_test_error_file,file);
            return failure==1u ? 0u : fread(memory,size,4096u,file);
        }
    }
    return fread(memory,size,count,file);
}
static int start_test_ferror(FILE *file) {
    return atomic_load(&start_test_error_file)==file ? 1 : ferror(file);
}
static bool start_test_cache_snapshot(const char *filepath,gui_playback_length_cache_key_t *key) {
    if(atomic_exchange(&start_test_snapshot_failure_once,false)) {
        memset(key,0,sizeof(*key)); return false;
    }
    return gui_playback_length_cache_snapshot(filepath,key);
}
static FLAC__uint64 start_test_tail_total(FLAC__StreamDecoder *decoder) {
    if (atomic_load(&start_test_tail_disabled)) return 0;
    if (atomic_exchange(&start_test_tail_delay_once,false)) thrd_sleep_ms(300);
    if (atomic_exchange(&start_test_tail_hold_once,false)) {
        atomic_store(&start_test_tail_entered,true);
        while(!atomic_load(&start_test_tail_release)) thrd_sleep_ms(1);
    }
    return FLAC__stream_decoder_find_total_samples(decoder);
}
static FLAC__StreamDecoder *start_test_decoder_new(void) {
    FLAC__StreamDecoder *decoder=FLAC__stream_decoder_new();
    if (decoder) atomic_fetch_add(&start_test_live_decoders,1);
    return decoder;
}
static void start_test_decoder_delete(FLAC__StreamDecoder *decoder) {
    if (!decoder) return;
    FLAC__stream_decoder_delete(decoder);
    start_test_acquire();
    for(unsigned n=0;n<START_TEST_SLOTS;++n) {
        if(start_test_callbacks[n].decoder==decoder) {
            FILE *closed=start_test_callbacks[n].owned_file;
            if(closed) {
                unsigned slot=0;
                while(slot<START_TEST_SLOTS && start_test_files[slot].file!=closed) ++slot;
                assert(slot<START_TEST_SLOTS && start_test_files[slot].decoder_owned);
                memset(&start_test_files[slot],0,sizeof(start_test_files[slot]));
                atomic_fetch_sub(&start_test_live_files,1);
            }
            memset(&start_test_callbacks[n],0,sizeof(start_test_callbacks[n]));
        }
    }
    start_test_release();
    atomic_fetch_sub(&start_test_live_decoders,1);
}
static FLAC__StreamDecoderWriteStatus start_test_write_callback(
    const FLAC__StreamDecoder *decoder,const FLAC__Frame *frame,
    const FLAC__int32 *const samples[],void *userdata) {
    FLAC__StreamDecoderWriteCallback original=NULL;
    start_test_acquire();
    for(unsigned n=0;n<START_TEST_SLOTS;++n) {
        if(start_test_callbacks[n].decoder==decoder) {
            assert(start_test_callbacks[n].userdata==userdata && userdata);
            original=start_test_callbacks[n].callback;
            break;
        }
    }
    start_test_release();
    assert(original);
    atomic_fetch_add(&start_test_callback_writes,1);
    if(original==decoder_io_write_cb_a || original==decoder_io_write_cb_b) {
        playback_io_t *io=userdata;
        playback_ctx_t *ctx=io->owner;
        assert(ctx && io->file);
        bool channel_b=original==decoder_io_write_cb_b;
        assert(io==(channel_b?&ctx->io_b:&ctx->io_a));
        assert(decoder==(channel_b?ctx->decoder_b:ctx->decoder_a));
    }
    if(original==decoder_write_cb_a || original==decoder_io_write_cb_a)
        atomic_fetch_add(&start_test_callback_writes_a,1);
    else {
        assert(original==decoder_write_cb_b || original==decoder_io_write_cb_b);
        atomic_fetch_add(&start_test_callback_writes_b,1);
    }
    return original(decoder,frame,samples,userdata);
}
/* init_stream leaves FILE ownership with the private loader. Wrap only the
 * producer callbacks: bounded tail, verifier and paused analysis retain their
 * actual IO callbacks and are excluded from producer callback counters. */
static FLAC__StreamDecoderInitStatus start_test_decoder_init_stream(
    FLAC__StreamDecoder *decoder,FLAC__StreamDecoderReadCallback read_callback,
    FLAC__StreamDecoderSeekCallback seek_callback,FLAC__StreamDecoderTellCallback tell_callback,
    FLAC__StreamDecoderLengthCallback length_callback,FLAC__StreamDecoderEofCallback eof_callback,
    FLAC__StreamDecoderWriteCallback write_callback,
    FLAC__StreamDecoderMetadataCallback metadata_callback,
    FLAC__StreamDecoderErrorCallback error_callback,void *userdata) {
    bool producer=write_callback==decoder_io_write_cb_a || write_callback==decoder_io_write_cb_b;
    if(producer) {
        start_test_acquire();
        unsigned slot=0;
        while(slot<START_TEST_SLOTS && start_test_callbacks[slot].decoder) ++slot;
        assert(slot<START_TEST_SLOTS);
        start_test_callbacks[slot]=(start_test_callback_registration_t){decoder,write_callback,userdata,NULL};
        start_test_release();
    }
    FLAC__StreamDecoderInitStatus status=FLAC__stream_decoder_init_stream(decoder,
        read_callback,seek_callback,tell_callback,length_callback,eof_callback,
        producer?start_test_write_callback:write_callback,metadata_callback,error_callback,userdata);
    if(producer && status==FLAC__STREAM_DECODER_INIT_STATUS_OK &&
       atomic_exchange(&start_test_init_failure_once,false))
        return FLAC__STREAM_DECODER_INIT_STATUS_MEMORY_ALLOCATION_ERROR;
    return status;
}

/* Surrounding services are controlled, while the actual FLAC producer runs. */
double GetTime(void) { return (double)gui_playback_clock_us()/1000000.0; }
void gui_app_set_status(gui_app_t *app,const char *message) {
    snprintf(app->status_message,sizeof(app->status_message),"%s",message);
}
void gui_extract_prepare_stream(gui_app_t *app) {
    atomic_fetch_add(&app->rf_stream_epoch,1);
}
int gui_extract_start_tagged(gui_app_t *app) {
    (void)app;
    if (atomic_load(&start_test_extract_failure)) return -1;
    atomic_store(&start_test_extract_running,true);
    return 0;
}
void gui_extract_stop(void) { atomic_store(&start_test_extract_running,false); }
bool gui_extract_stamp_playback(const gui_rf_block_info_t info[2],const size_t counts[2]) {
    unsigned channels=atomic_load(&start_test_channel_mask);
    for(unsigned n=0;n<2;++n) {
        bool active=(channels&(1u<<n))!=0;
        assert(info[n].sample_rate_hz==(active?atomic_load(&start_test_source_rate):0u));
        assert(active ? counts[n]>0 && counts[n]<=RAW_BUFFER_SAMPLES : counts[n]==0);
        atomic_store(&start_test_last_epoch[n],info[n].epoch);
    }
    return true;
}
int gui_display_thread_start(display_thread_t *display,struct gui_app *app,
                              struct buffer_manager *manager) {
    (void)display; (void)app; (void)manager; return 0;
}
void gui_display_thread_stop(display_thread_t *display) { (void)display; }
void bufmgr_reset_stats(buffer_manager_t *manager,buffer_id_t id) {
    unsigned first=id==BUF_COUNT?0u:(unsigned)id;
    unsigned end=id==BUF_COUNT?(unsigned)BUF_COUNT:first+1u;
    for(unsigned n=first;n<end;++n) {
        atomic_store(&manager->stats[n].bytes_written,0);
        atomic_store(&manager->stats[n].bytes_read,0);
        atomic_store(&manager->stats[n].write_drops,0);
    }
}
void *bufmgr_write_begin(buffer_manager_t *manager,buffer_id_t id,size_t bytes,
                         const backpressure_policy_t *policy) {
    (void)manager; (void)policy;
    assert(id==BUF_CAPTURE_RF && bytes==sizeof(start_test_sink));
    return start_test_sink;
}
void bufmgr_write_end(buffer_manager_t *manager,buffer_id_t id,size_t bytes) {
    assert(id==BUF_CAPTURE_RF && bytes==sizeof(start_test_sink));
    unsigned channels=atomic_load(&start_test_channel_mask);
    assert(start_test_sink[0]==encode_raw_sample(channels&1u?-2048:0,channels&2u?-2048:0));
    assert(start_test_sink[37]==encode_raw_sample(channels&1u?-2011:0,channels&2u?-2011:0));
    if(atomic_exchange(&start_test_sink_hold_once,false)) {
        atomic_store(&start_test_sink_entered,true);
        while(!atomic_load(&start_test_sink_release)) thrd_sleep_ms(1);
    }
    atomic_fetch_add(&start_test_sink_writes,1);
    atomic_fetch_add(&manager->stats[id].bytes_written,bytes);
    atomic_fetch_add(&manager->stats[id].bytes_read,bytes); // Immediate headless sink
}
void bufmgr_signal_data(buffer_manager_t *manager,buffer_id_t id) {
    (void)manager; (void)id;
}

static uint64_t start_test_clock_ms(void) { return gui_playback_clock_us()/1000u; }
static void start_test_wait_flag(atomic_bool *flag) {
    uint64_t begin=start_test_clock_ms();
    while(!atomic_load(flag)) {
        assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS);
        thrd_sleep_ms(1);
    }
}
static int start_test_poll_complete(void) {
    uint64_t begin=start_test_clock_ms();
    int result;
    while((result=gui_playback_poll_start(&start_test_app))==0) {
        assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS);
        thrd_sleep_ms(1);
    }
    return result;
}
static void start_test_assert_clean(void) {
    assert(!gui_playback_is_starting(&start_test_app));
    assert(!gui_playback_is_running(&start_test_app));
    assert(!gui_playback_is_indexing(&start_test_app,0));
    assert(!gui_playback_is_indexing(&start_test_app,1));
    playback_index_progress_t progress={.active=true,.waiting=true,.queued=true,.bytes_scanned=99,.total_bytes=100};
    assert(!gui_playback_get_index_progress(&start_test_app,0,&progress));
    assert(!progress.active && !progress.waiting && !progress.queued && progress.bytes_scanned==0 && progress.total_bytes==0);
    assert(!start_test_app.is_capturing);
    assert(!atomic_load(&start_test_extract_running));
    assert(atomic_load(&start_test_live_decoders)==0);
    assert(atomic_load(&start_test_live_allocations)==0);
    assert(atomic_load(&start_test_live_files)==0);
}
static void start_test_stop_clean(void) {
    gui_playback_stop(&start_test_app);
    gui_playback_finish_start(&start_test_app);
    start_test_assert_clean();
}
static void start_test_arm_read(void) {
    atomic_store(&start_test_read_entered,false);
    atomic_store(&start_test_release_read,false);
    atomic_store(&start_test_slow_read_count,0);
    atomic_store(&start_test_slow_next,true);
}
static void start_test_encode_rate_fixture(const char *normal,unsigned total_samples,
                                           unsigned rf_rate,unsigned padding_bytes) {
    FLAC__StreamEncoder *encoder=FLAC__stream_encoder_new(); assert(encoder);
    assert(FLAC__stream_encoder_set_channels(encoder,1));
    assert(FLAC__stream_encoder_set_bits_per_sample(encoder,16));
    assert(FLAC__stream_encoder_set_sample_rate(encoder,20000));
    assert(FLAC__stream_encoder_set_compression_level(encoder,0));
    assert(FLAC__stream_encoder_set_blocksize(encoder,START_TEST_BLOCK));
    assert(FLAC__stream_encoder_set_total_samples_estimate(encoder,total_samples));
    FLAC__StreamMetadata *metadata[2]={0}; unsigned metadata_count=0;
    if(rf_rate) {
        metadata[metadata_count]=FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
        assert(metadata[metadata_count]);
        char value[32]; snprintf(value,sizeof(value),"%u",rf_rate);
        FLAC__StreamMetadata_VorbisComment_Entry entry;
        assert(FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry,"RF_SAMPLE_RATE",value));
        assert(FLAC__metadata_object_vorbiscomment_append_comment(metadata[metadata_count++],entry,false));
    }
    if(padding_bytes) {
        metadata[metadata_count]=FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
        assert(metadata[metadata_count]); metadata[metadata_count++]->length=padding_bytes;
    }
    if(metadata_count) assert(FLAC__stream_encoder_set_metadata(encoder,metadata,metadata_count));
    assert(FLAC__stream_encoder_init_file(encoder,normal,NULL,NULL)==FLAC__STREAM_ENCODER_INIT_STATUS_OK);
    FLAC__int32 block[START_TEST_BLOCK];
    for(unsigned offset=0;offset<total_samples;offset+=START_TEST_BLOCK) {
        unsigned available=total_samples-offset;
        if(available>START_TEST_BLOCK) available=START_TEST_BLOCK;
        for(unsigned n=0;n<available;++n)
            block[n]=(FLAC__int32)((int)((offset+n)%4096u)-2048)*16;
        assert(FLAC__stream_encoder_process_interleaved(encoder,block,available));
    }
    assert(FLAC__stream_encoder_finish(encoder));
    FLAC__stream_encoder_delete(encoder);
    for(unsigned n=0;n<metadata_count;++n) FLAC__metadata_object_delete(metadata[n]);
}
static void start_test_encode_fixture(const char *normal,unsigned total_samples) {
    start_test_encode_rate_fixture(normal,total_samples,0,0);
}
static void start_test_zero_total_copy(const char *normal,const char *unknown) {
    FILE *input=fopen(normal,"rb"),*output=fopen(unknown,"wb"); assert(input && output);
    unsigned char copy[8192]; size_t count;
    while((count=fread(copy,1,sizeof(copy),input))>0) assert(fwrite(copy,1,count,output)==count);
    fclose(input); fclose(output);
    output=fopen(unknown,"r+b"); assert(output);
    assert(fseek(output,21,SEEK_SET)==0);
    int preserved=fgetc(output); assert(preserved>=0);
    assert(fseek(output,21,SEEK_SET)==0);
    unsigned char zero_total[5]={(unsigned char)(preserved&0xf0),0,0,0,0};
    assert(fwrite(zero_total,1,sizeof(zero_total),output)==sizeof(zero_total));
    fclose(output);
}
static void start_test_make_fixture(const char *normal,const char *unknown,const char *bad) {
    start_test_encode_fixture(normal,START_TEST_SAMPLES);
    start_test_zero_total_copy(normal,unknown);
    FILE *output=fopen(bad,"wb"); assert(output);
    assert(fwrite("not a FLAC",1,10,output)==10); fclose(output);
}
static void start_test_play_count(const char *fixture,bool unknown,unsigned channels,unsigned expected_total) {
    atomic_store(&start_test_sink_writes,0);
    atomic_store(&start_test_channel_mask,channels);
    unsigned callbacks=atomic_load(&start_test_callback_writes);
    unsigned callbacks_a=atomic_load(&start_test_callback_writes_a);
    unsigned callbacks_b=atomic_load(&start_test_callback_writes_b);
    uint64_t begin=start_test_clock_ms();
    assert(gui_playback_start_async(&start_test_app,channels&1u?fixture:NULL,channels&2u?fixture:NULL)==0);
    assert(start_test_poll_complete()==1);
    assert(gui_playback_is_running(&start_test_app) && start_test_app.is_capturing);
    assert(start_test_app.capture_has_channel_b==((channels&2u)!=0));
    playback_file_info_t info_a,info_b;
    assert(gui_playback_get_file_info_a(&start_test_app,&info_a)==((channels&1u)!=0));
    assert(gui_playback_get_file_info_b(&start_test_app,&info_b)==((channels&2u)!=0));
    while(atomic_load(&start_test_sink_writes)==0) {
        assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS);
        gui_playback_poll_start(&start_test_app); thrd_sleep_ms(1);
    }
    assert(atomic_load(&start_test_callback_writes)>callbacks);
    assert(channels&1u ? atomic_load(&start_test_callback_writes_a)>callbacks_a
                        : atomic_load(&start_test_callback_writes_a)==callbacks_a);
    assert(channels&2u ? atomic_load(&start_test_callback_writes_b)>callbacks_b
                        : atomic_load(&start_test_callback_writes_b)==callbacks_b);
    while(gui_playback_is_indexing(&start_test_app,0) || gui_playback_is_indexing(&start_test_app,1)) {
        assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS);
        assert(gui_playback_poll_start(&start_test_app)==0); thrd_sleep_ms(1);
    }
    assert(gui_playback_get_file_info_a(&start_test_app,&info_a)==((channels&1u)!=0));
    assert(gui_playback_get_file_info_b(&start_test_app,&info_b)==((channels&2u)!=0));
    if(channels&1u) assert(info_a.total_samples==expected_total && info_a.sample_rate==20000u);
    if(channels&2u) assert(info_b.total_samples==expected_total && info_b.sample_rate==20000u);
    assert(gui_playback_get_total_samples(&start_test_app)==expected_total);
    start_test_stop_clean();
    printf("Playback async %s %s: channel metadata, real decoder userdata callbacks, raw sink and cleanup passed\n",
        channels==1u?"A-only":channels==2u?"B-only":"A+B",unknown?"total=0":"normal");
}
static void start_test_play(const char *fixture,bool unknown,unsigned channels) {
    start_test_play_count(fixture,unknown,channels,START_TEST_SAMPLES);
}
static void start_test_slow_heartbeat(const char *fixture) {
    start_test_arm_read();
    uint64_t begin=start_test_clock_ms();
    assert(gui_playback_start_async(&start_test_app,fixture,NULL)==0);
    uint64_t accepted=start_test_clock_ms()-begin;
    start_test_wait_flag(&start_test_read_entered);
    assert(gui_playback_is_starting(&start_test_app));
    assert(gui_playback_start_async(&start_test_app,fixture,NULL)==-1);
    unsigned heartbeats=0;
    begin=start_test_clock_ms();
    while(start_test_clock_ms()-begin<300u) {
        assert(gui_playback_poll_start(&start_test_app)==0);
        assert(gui_playback_is_starting(&start_test_app));
        ++heartbeats; thrd_sleep_ms(1);
    }
    assert(heartbeats>=10u); /* No strict machine-specific frame-time threshold. */
    atomic_store(&start_test_release_read,true);
    assert(start_test_poll_complete()==1);
    start_test_stop_clean();
    printf("Playback pending slow fread: 300ms, %u UI heartbeats, request=%" PRIu64 "ms passed\n",heartbeats,accepted);
}
static void start_test_cancel_retry(const char *fixture) {
    start_test_arm_read();
    assert(gui_playback_start_async(&start_test_app,fixture,NULL)==0);
    start_test_wait_flag(&start_test_read_entered);
    uint64_t begin=start_test_clock_ms();
    gui_playback_cancel_start(&start_test_app);
    uint64_t request_ms=start_test_clock_ms()-begin;
    /* Cancellation must return while the controlled read remains blocked. */
    assert(!atomic_load(&start_test_release_read));
    atomic_store(&start_test_release_read,true);
    assert(start_test_poll_complete()==-1);
    gui_playback_finish_start(&start_test_app);
    start_test_assert_clean();
    printf("Playback pending metadata cancellation: returned before read release (%" PRIu64 "ms), no late commit passed\n",request_ms);
    start_test_play(fixture,false,1u);
}
static atomic_bool start_test_finish_done;
static int start_test_finish_thread(void *unused) {
    (void)unused;
    gui_playback_finish_start(&start_test_app);
    atomic_store(&start_test_finish_done,true);
    return 0;
}
static void start_test_exit_pending(const char *fixture) {
    start_test_arm_read();
    atomic_store(&start_test_finish_done,false);
    assert(gui_playback_start_async(&start_test_app,fixture,NULL)==0);
    start_test_wait_flag(&start_test_read_entered);
    thrd_t thread;
    assert(thrd_create_with_priority(&thread,start_test_finish_thread,NULL,THRD_PRIORITY_NORMAL)==thrd_success);
    thrd_sleep_ms(30);
    assert(!atomic_load(&start_test_finish_done)); /* finish joins pending I/O. */
    atomic_store(&start_test_release_read,true);
    start_test_wait_flag(&start_test_finish_done);
    thrd_join(thread,NULL);
    start_test_assert_clean();
    puts("Playback exit during pending read: cancel, join, decoder/allocation cleanup passed");
}
static void start_test_wait_writes(unsigned previous) {
    uint64_t begin=start_test_clock_ms();
    while(atomic_load(&start_test_sink_writes)<=previous) {
        assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS);
        assert(gui_playback_poll_start(&start_test_app)==0);
        thrd_sleep_ms(1);
    }
}
static void start_test_wait_index(void) {
    uint64_t begin=start_test_clock_ms();
    while(gui_playback_is_indexing(&start_test_app,0) || gui_playback_is_indexing(&start_test_app,1)) {
        assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS);
        assert(gui_playback_poll_start(&start_test_app)==0);
        thrd_sleep_ms(1);
    }
    assert(gui_playback_poll_start(&start_test_app)==0);
}
static void start_test_pause_stable(void) {
    gui_playback_pause(&start_test_app);
    assert(gui_playback_get_state(&start_test_app)==PLAYBACK_STATE_PAUSED);
    /* A currently executing output block may finish after the pause request. */
    thrd_sleep_ms(60);
    unsigned writes=atomic_load(&start_test_sink_writes);
    uint64_t position[2]={gui_playback_get_position_samples_channel(&start_test_app,0),
                          gui_playback_get_position_samples_channel(&start_test_app,1)};
    thrd_sleep_ms(30);
    assert(atomic_load(&start_test_sink_writes)==writes);
    for(unsigned n=0;n<2;++n)
        assert(gui_playback_get_position_samples_channel(&start_test_app,(int)n)==position[n]);
}
static void start_test_unknown_seek_noop(int unknown_channel,bool all_unknown) {
    uint64_t position[2]={gui_playback_get_position_samples_channel(&start_test_app,0),
                          gui_playback_get_position_samples_channel(&start_test_app,1)};
    uint64_t epoch=atomic_load(&start_test_app.rf_stream_epoch);
    unsigned writes=atomic_load(&start_test_sink_writes);
    gui_playback_seek_sample_channel(&start_test_app,unknown_channel,17u);
    if(all_unknown) {
        gui_playback_seek_sample(&start_test_app,31u);
        gui_playback_seek_normalized(&start_test_app,0.5);
    }
    thrd_sleep_ms(30);
    for(unsigned n=0;n<2;++n)
        assert(gui_playback_get_position_samples_channel(&start_test_app,(int)n)==position[n]);
    assert(atomic_load(&start_test_sink_writes)==writes);
    assert(atomic_load(&start_test_app.rf_stream_epoch)==epoch);
    assert(!atomic_load(&s_playback->seek_requested_a) && !atomic_load(&s_playback->seek_requested_b));
}
static void start_test_begin_held_index(const char *known,const char *unknown) {
    unsigned channels=known?3u:1u;
    unsigned callbacks_a=atomic_load(&start_test_callback_writes_a);
    unsigned callbacks_b=atomic_load(&start_test_callback_writes_b);
    atomic_store(&start_test_sink_writes,0);
    atomic_store(&start_test_channel_mask,channels);
    start_test_arm_read();
    /* Only the scanner's 8 MiB request is held. Metadata prepare and libFLAC
     * producer I/O remain free to run, so this cannot fake a pending startup. */
    atomic_store(&start_test_slow_read_count,PLAYBACK_FRAME_SCAN_CHUNK_BYTES);
    uint64_t begin=start_test_clock_ms();
    assert(gui_playback_start_async(&start_test_app,known?known:unknown,known?unknown:NULL)==0);
    assert(start_test_poll_complete()==1);
    uint64_t committed_ms=start_test_clock_ms()-begin;
    assert(!gui_playback_is_starting(&start_test_app));
    assert(gui_playback_is_running(&start_test_app) && start_test_app.is_capturing);
    assert(start_test_app.capture_has_channel_b==(known!=NULL));
    start_test_wait_flag(&start_test_read_entered);
    assert(gui_playback_is_indexing(&start_test_app,known?1:0));
    assert(!gui_playback_is_indexing(&start_test_app,known?0:1));
    assert(!gui_playback_is_indexing(&start_test_app,-1));
    assert(!gui_playback_is_indexing(&start_test_app,2));
    playback_index_progress_t progress;
    assert(gui_playback_get_index_progress(&start_test_app,known?1:0,&progress));
    assert(progress.active && !progress.queued && !progress.waiting && progress.bytes_scanned==0 && progress.total_bytes>0);
    assert(!gui_playback_get_index_progress(&start_test_app,-1,&progress));
    assert(!progress.active && progress.bytes_scanned==0);
    assert(gui_playback_get_channel_rate_hz(&start_test_app,0)==20000000.0);
    assert(gui_playback_get_channel_rate_hz(&start_test_app,1)==(known?20000000.0:0.0));
    assert(gui_playback_get_channel_rate_hz(&start_test_app,-1)==0.0);
    start_test_wait_writes(0);
    uint64_t raw_ms=start_test_clock_ms()-begin;
    assert(atomic_load(&start_test_callback_writes_a)>callbacks_a);
    assert(known ? atomic_load(&start_test_callback_writes_b)>callbacks_b
                 : atomic_load(&start_test_callback_writes_b)==callbacks_b);
    playback_file_info_t info;
    assert(known ? gui_playback_get_file_info_b(&start_test_app,&info)
                 : gui_playback_get_file_info_a(&start_test_app,&info));
    assert(info.total_samples==0 && info.duration_seconds==0.0);
    assert(gui_playback_get_total_samples(&start_test_app)==(known?START_TEST_SAMPLES:0u));
    assert(!atomic_load(&start_test_release_read));
    assert(atomic_load(&start_test_live_files)>=2u); /* Producer and scanner. */
    printf("Playback held background index %s: commit=%" PRIu64 "ms raw-before-index=%" PRIu64 "ms passed\n",
           known?"A known/B unknown":"A unknown",committed_ms,raw_ms);
}
static void start_test_index_publish(const char *known,const char *unknown,unsigned expected_total) {
    start_test_begin_held_index(known,unknown);
    unsigned heartbeats=0;
    uint64_t begin=start_test_clock_ms();
    while(start_test_clock_ms()-begin<300u) {
        assert(gui_playback_poll_start(&start_test_app)==0);
        assert(gui_playback_is_indexing(&start_test_app,known?1:0));
        ++heartbeats; thrd_sleep_ms(1);
    }
    assert(heartbeats>=10u && atomic_load(&start_test_sink_writes)>1u);
    start_test_pause_stable();
    start_test_unknown_seek_noop(known?1:0,known==NULL);
    gui_playback_set_speed(&start_test_app,PLAYBACK_SPEED_2X);
    gui_playback_set_loop(&start_test_app,false);
    uint64_t position[2]={gui_playback_get_position_samples_channel(&start_test_app,0),
                          gui_playback_get_position_samples_channel(&start_test_app,1)};
    double rates[2]={gui_playback_get_channel_rate_hz(&start_test_app,0),
                     gui_playback_get_channel_rate_hz(&start_test_app,1)};
    uint64_t epoch=atomic_load(&start_test_app.rf_stream_epoch);
    uint64_t block_epoch[2]={atomic_load(&start_test_last_epoch[0]),atomic_load(&start_test_last_epoch[1])};
    unsigned writes=atomic_load(&start_test_sink_writes);
    atomic_store(&start_test_release_read,true);
    start_test_wait_index();
    playback_file_info_t info;
    assert(known ? gui_playback_get_file_info_b(&start_test_app,&info)
                 : gui_playback_get_file_info_a(&start_test_app,&info));
    assert(info.total_samples==expected_total && info.sample_rate==20000u);
    assert(fabs(info.duration_seconds-(double)expected_total/20000000.0)<1e-12);
    assert(gui_playback_get_total_samples(&start_test_app)==expected_total);
    assert(fabs(gui_playback_get_duration_seconds(&start_test_app)-(double)expected_total/20000000.0)<1e-12);
    if(known) {
        assert(gui_playback_get_file_info_a(&start_test_app,&info));
        assert(info.total_samples==START_TEST_SAMPLES);
    }
    for(unsigned n=0;n<2;++n) {
        assert(gui_playback_get_position_samples_channel(&start_test_app,(int)n)==position[n]);
        assert(gui_playback_get_channel_rate_hz(&start_test_app,(int)n)==rates[n]);
        assert(atomic_load(&start_test_last_epoch[n])==block_epoch[n]);
    }
    assert(atomic_load(&start_test_sink_writes)==writes);
    assert(atomic_load(&start_test_app.rf_stream_epoch)==epoch);
    assert(gui_playback_get_state(&start_test_app)==PLAYBACK_STATE_PAUSED);
    assert(gui_playback_get_speed(&start_test_app)==PLAYBACK_SPEED_2X);
    assert(!gui_playback_get_loop(&start_test_app));
    start_test_stop_clean();
    printf("Playback index publication %s: %u held UI heartbeats; completes while paused, exact length, position/rate/speed/loop/epoch preserved passed\n",
           known?"A known/B unknown":"A unknown",heartbeats);
}
static int start_test_stop_thread(void *unused) {
    (void)unused;
    gui_playback_stop(&start_test_app);
    gui_playback_finish_start(&start_test_app);
    atomic_store(&start_test_finish_done,true);
    return 0;
}
static void start_test_index_stop_reopen(const char *unknown,const char *replacement) {
    start_test_begin_held_index(NULL,unknown);
    atomic_store(&start_test_finish_done,false);
    thrd_t thread;
    assert(thrd_create_with_priority(&thread,start_test_stop_thread,NULL,THRD_PRIORITY_NORMAL)==thrd_success);
    /* Main observes only test atomics while the stop thread owns UI/session API. */
    thrd_sleep_ms(30);
    assert(!atomic_load(&start_test_finish_done));
    atomic_store(&start_test_release_read,true);
    start_test_wait_flag(&start_test_finish_done);
    thrd_join(thread,NULL);
    start_test_assert_clean();
    atomic_store(&start_test_sink_writes,0);
    assert(gui_playback_start_async(&start_test_app,replacement,NULL)==0);
    assert(start_test_poll_complete()==1);
    start_test_wait_writes(0);
    uint64_t begin=start_test_clock_ms();
    while(start_test_clock_ms()-begin<60u) {
        assert(gui_playback_poll_start(&start_test_app)==0);
        assert(!gui_playback_is_indexing(&start_test_app,0));
        assert(gui_playback_get_total_samples(&start_test_app)==START_TEST_OTHER_SAMPLES);
        thrd_sleep_ms(1);
    }
    start_test_stop_clean();
    puts("Playback stop during held index: cancel/join/resources zero, reopen different length receives no stale result passed");
}
static void start_test_index_eof(const char *unknown) {
    start_test_begin_held_index(NULL,unknown);
    gui_playback_set_loop(&start_test_app,false);
    uint64_t begin=start_test_clock_ms();
    while(gui_playback_get_state(&start_test_app)!=PLAYBACK_STATE_PAUSED) {
        assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS);
        assert(gui_playback_poll_start(&start_test_app)==0);
        thrd_sleep_ms(1);
    }
    assert(gui_playback_get_position_samples(&start_test_app)==START_TEST_SAMPLES);
    assert(gui_playback_get_position_samples_channel(&start_test_app,0)==START_TEST_SAMPLES);
    assert(gui_playback_get_total_samples(&start_test_app)==0);
    assert(fabs(gui_playback_get_position_seconds(&start_test_app)-(double)START_TEST_SAMPLES/20000000.0)<1e-12);
    uint64_t epoch=atomic_load(&start_test_app.rf_stream_epoch);
    unsigned writes=atomic_load(&start_test_sink_writes);
    atomic_store(&start_test_release_read,true);
    start_test_wait_index();
    assert(gui_playback_get_position_samples(&start_test_app)==START_TEST_SAMPLES);
    assert(gui_playback_get_position_samples_channel(&start_test_app,0)==START_TEST_SAMPLES);
    assert(gui_playback_get_total_samples(&start_test_app)==START_TEST_SAMPLES);
    assert(gui_playback_get_position_normalized(&start_test_app)==1.0);
    assert(fabs(gui_playback_get_position_seconds(&start_test_app)-(double)START_TEST_SAMPLES/20000000.0)<1e-12);
    assert(gui_playback_get_state(&start_test_app)==PLAYBACK_STATE_PAUSED);
    assert(!gui_playback_get_loop(&start_test_app));
    assert(atomic_load(&start_test_sink_writes)==writes);
    assert(atomic_load(&start_test_app.rf_stream_epoch)==epoch);
    start_test_stop_clean();
    puts("Playback unknown Loop Off EOF: before/after index position=131072, normalized=1, seconds=0.0065536 preserved passed");
}
static void start_test_index_failure(const char *unknown,unsigned failure) {
    atomic_store(&start_test_sink_writes,0);
    atomic_store(&start_test_channel_mask,1u);
    if(failure==0) atomic_store(&start_test_scan_malloc_failure_once,true);
    else atomic_store(&start_test_scan_read_failure_once,failure);
    assert(gui_playback_start_async(&start_test_app,unknown,NULL)==0);
    assert(start_test_poll_complete()==1);
    start_test_wait_writes(0);
    start_test_wait_index();
    assert(!atomic_load(&start_test_scan_malloc_failure_once));
    assert(atomic_load(&start_test_scan_read_failure_once)==0u);
    assert(atomic_load(&start_test_error_file)==NULL);
    playback_file_info_t info;
    assert(gui_playback_get_file_info_a(&start_test_app,&info));
    assert(info.total_samples==0 && info.duration_seconds==0.0);
    assert(gui_playback_get_total_samples(&start_test_app)==0);
    assert(gui_playback_get_channel_rate_hz(&start_test_app,0)==20000000.0);
    assert(gui_playback_is_running(&start_test_app) && start_test_app.is_capturing);
    assert(gui_playback_get_state(&start_test_app)==PLAYBACK_STATE_PLAYING);
    start_test_wait_writes(atomic_load(&start_test_sink_writes));
    start_test_pause_stable();
    start_test_unknown_seek_noop(0,true);
    start_test_stop_clean();
    printf("Playback index %s: unavailable length stays unknown, producer continues, seek no-op, resources zero passed\n",
           failure==0?"scan-window OOM":failure==1?"zero-read ferror":"partial-read ferror");
}
static playback_streaminfo_aux_t start_test_stream_aux(const char *fixture) {
    FLAC__StreamMetadata metadata;
    assert(FLAC__metadata_get_streaminfo(fixture,&metadata));
    const FLAC__StreamMetadata_StreamInfo *info=&metadata.data.stream_info;
    assert(info->total_samples==0);
    return (playback_streaminfo_aux_t){
        .min_block_size=info->min_blocksize,.max_block_size=info->max_blocksize,
        .min_frame_size=info->min_framesize,.max_frame_size=info->max_framesize,
        .channels=info->channels};
}
static bool start_test_count_samples(const char *filepath, uint64_t audio_offset,
                                                   uint32_t min_frame_size, uint32_t max_frame_size,
                                                   uint64_t *out_samples,
                                                   const atomic_bool *cancel) {
    return gui_playback_scan_samples(filepath, audio_offset, min_frame_size, max_frame_size,
                                     out_samples, cancel, false, NULL);
}

static void start_test_scan_exact(const char *fixture) {
    playback_streaminfo_aux_t aux=start_test_stream_aux(fixture);
    uint64_t audio_offset=0,file_size=0,total=0;
    assert(gui_playback_find_audio_offset_and_size(fixture,&audio_offset,&file_size));
    assert(start_test_count_samples(fixture,audio_offset,aux.min_frame_size,
        aux.max_frame_size,&total,NULL));
    total=0;
    assert(start_test_count_samples(fixture,audio_offset,aux.min_frame_size,0,&total,NULL));
    assert(total==START_TEST_SAMPLES+137u);
    playback_total_source_t source=PLAYBACK_TOTAL_SOURCE_UNRESOLVED;
    double rate=0;
    assert(gui_playback_resolve_total_samples(fixture,20000,16,0,&aux,&source,&rate,NULL,NULL,NULL)==0);
    assert(source==PLAYBACK_TOTAL_SOURCE_UNRESOLVED && rate==20000000.0);
    start_test_assert_clean();
    puts("Playback full scan: total=0 final 137-sample short block exact; metadata-only resolver does not scan passed");
}
typedef struct {
    const char *filepath;
    uint64_t audio_offset,total;
    uint32_t min_frame_size,max_frame_size;
    atomic_bool cancel,done;
    bool result;
} start_test_scan_t;
static int start_test_scan_thread(void *context) {
    start_test_scan_t *scan=context;
    scan->result=start_test_count_samples(scan->filepath,
        scan->audio_offset,scan->min_frame_size,scan->max_frame_size,&scan->total,&scan->cancel);
    atomic_store(&scan->done,true);
    return 0;
}
static void start_test_scan_cancel(const char *fixture) {
    playback_streaminfo_aux_t aux=start_test_stream_aux(fixture);
    uint64_t audio_offset=0,file_size=0;
    assert(gui_playback_find_audio_offset_and_size(fixture,&audio_offset,&file_size));
    start_test_scan_t scan={.filepath=fixture,.audio_offset=audio_offset,
        .min_frame_size=aux.min_frame_size,.max_frame_size=aux.max_frame_size};
    atomic_init(&scan.cancel,false); atomic_init(&scan.done,false);
    start_test_arm_read();
    thrd_t thread;
    assert(thrd_create_with_priority(&thread,start_test_scan_thread,&scan,THRD_PRIORITY_NORMAL)==thrd_success);
    start_test_wait_flag(&start_test_read_entered);
    atomic_store(&scan.cancel,true);
    atomic_store(&start_test_release_read,true);
    start_test_wait_flag(&scan.done);
    thrd_join(thread,NULL);
    assert(!scan.result && scan.total==0);
    start_test_assert_clean();
    puts("Playback fallback frame scan: cancellation after blocked read discards partial count and frees window passed");
}
static FLAC__StreamDecoderWriteStatus start_test_embedded_write(
    const FLAC__StreamDecoder *decoder,const FLAC__Frame *frame,
    const FLAC__int32 *const samples[],void *context) {
    (void)decoder; (void)samples;
    assert(frame->header.channels==1 && frame->header.bits_per_sample==16);
    *(uint64_t *)context+=frame->header.blocksize;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void start_test_embedded_error(const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus error,void *context) {
    (void)decoder; (void)error; (void)context;
    assert(false); /* This counterexample is a valid FLAC stream, not damage. */
}
static void start_test_embedded_headers(const char *fixture) {
    /* One real 64-sample verbatim frame. PCM contains fake frame headers 1/2
     * with valid CRC8 and deliberately constructed CRC16 residues. A tail-only
     * pair shortcut previously accepted 192, although libFLAC decodes only 64.
     * STREAMINFO min/max frame size 138 lets the sequential scanner skip PCM.
     */
    static const uint8_t bytes[180]={
        [0]=0x66,[1]=0x4c,[2]=0x61,[3]=0x43,[4]=0x80,[7]=0x22,
        [9]=0x40,[11]=0x40,[14]=0x8a,[17]=0x8a,[18]=0x04,[19]=0xe2,[21]=0xf0,
        [42+0]=0xff,[42+1]=0xf8,[42+2]=0x60,[42+3]=0x08,[42+5]=0x3f,
        [42+6]=0x06,[42+7]=0x02,[42+10]=0xe6,[42+11]=0x17,
        [42+12]=0xff,[42+13]=0xf8,[42+14]=0x60,[42+15]=0x08,
        [42+16]=0x01,[42+17]=0x3f,[42+18]=0x13,[42+21]=0x0b,[42+22]=0x1a,
        [42+23]=0xff,[42+24]=0xf8,[42+25]=0x60,[42+26]=0x08,
        [42+27]=0x02,[42+28]=0x3f,[42+29]=0x2c,[42+136]=0xed,[42+137]=0x72
    };
    FILE *file=fopen(fixture,"wb"); assert(file);
    assert(fwrite(bytes,1,sizeof(bytes),file)==sizeof(bytes)); fclose(file);
    uint64_t decoded=0;
    FLAC__StreamDecoder *decoder=FLAC__stream_decoder_new(); assert(decoder);
    file=fopen(fixture,"rb"); assert(file);
    assert(FLAC__stream_decoder_init_FILE(decoder,file,start_test_embedded_write,NULL,
        start_test_embedded_error,&decoded)==FLAC__STREAM_DECODER_INIT_STATUS_OK);
    assert(FLAC__stream_decoder_process_until_end_of_stream(decoder));
    assert(decoded==64u);
    assert(FLAC__stream_decoder_finish(decoder)); FLAC__stream_decoder_delete(decoder);
    playback_streaminfo_aux_t aux=start_test_stream_aux(fixture);
    assert(aux.min_frame_size==138u && aux.max_frame_size==138u);
    playback_total_source_t source=PLAYBACK_TOTAL_SOURCE_UNRESOLVED;
    double rate=0;
    assert(gui_playback_resolve_total_samples(fixture,20000,16,0,&aux,&source,&rate,NULL,NULL,NULL)==0);
    uint64_t offset=0,size=0,count=0;
    assert(gui_playback_find_audio_offset_and_size(fixture,&offset,&size));
    assert(start_test_count_samples(fixture,offset,aux.min_frame_size,aux.max_frame_size,&count,NULL) && count==64u);
    start_test_assert_clean();
    puts("Playback valid FLAC embedded fake headers: libFLAC and sequential grammar reader both count 64, never 192 passed");
}
static void start_test_stale_stop_request(const char *fixture) {
    for (unsigned attempt=0; attempt<2; ++attempt) {
        // Fault injection: the previous extraction session posted a request
        // just before disconnect; idle UI frames never consume it.
        atomic_store(&start_test_app.dropout_stop_reason,GUI_DROPOUT_BACKPRESSURE);
        atomic_store(&start_test_app.dropout_stop_requested,true);
        start_test_app.low_signal_time=60.0f;
        start_test_app.low_signal_armed=true;
        atomic_store(&start_test_sink_writes,0);
        atomic_store(&start_test_channel_mask,1u);
        assert(gui_playback_start_async(&start_test_app,fixture,NULL)==0);
        assert(start_test_poll_complete()==1);
        // This is the main-loop check immediately following startup polling.
        assert(!atomic_exchange(&start_test_app.dropout_stop_requested,false));
        assert(atomic_load(&start_test_app.dropout_stop_reason)==GUI_DROPOUT_NONE);
        assert(start_test_app.low_signal_time==0.0f && !start_test_app.low_signal_armed);
        uint64_t begin=start_test_clock_ms();
        while(!atomic_load(&start_test_sink_writes)) {
            assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS);
            thrd_sleep_ms(1);
        }
        // Stop must also clear requests posted while the old producer unwinds.
        atomic_store(&start_test_app.dropout_stop_reason,GUI_DROPOUT_DISK_SPACE);
        atomic_store(&start_test_app.dropout_stop_requested,true);
        start_test_app.low_signal_time=12.0f;
        start_test_app.low_signal_armed=true;
        start_test_stop_clean();
        assert(!atomic_load(&start_test_app.dropout_stop_requested));
        assert(atomic_load(&start_test_app.dropout_stop_reason)==GUI_DROPOUT_NONE);
        assert(start_test_app.low_signal_time==0.0f && !start_test_app.low_signal_armed);
    }
    puts("Playback stale old-session stop request: startup main-loop check stays clear, stop/restart resets dropout and low-signal state passed (fault injected)");
}

static void start_test_cache_directory(const char *directory) {
#ifdef _WIN32
    assert(SetEnvironmentVariableA("MISRC_PLAYBACK_CACHE_DIR",directory));
#else
    assert(setenv("MISRC_PLAYBACK_CACHE_DIR",directory,1)==0);
#endif
}

static void start_test_pressure_policy(void) {
    buffer_manager_t manager={0};
    atomic_int state; atomic_init(&state,PLAYBACK_STATE_PLAYING);
    playback_scan_pressure_t pressure={.scan_buffers=&manager,.state=&state,
        .bytes_per_second={80000000u,40000000u,40000000u}};
    assert(!gui_playback_scan_has_pressure(&pressure,1000000u));
    atomic_store(&manager.stats[BUF_CAPTURE_RF].bytes_written,16000000u);
    assert(gui_playback_scan_has_pressure(&pressure,1100000u));
    atomic_store(&manager.stats[BUF_CAPTURE_RF].bytes_read,16000000u);
    assert(!gui_playback_scan_has_pressure(&pressure,1200000u));
    atomic_store(&manager.stats[BUF_RECORD_B].bytes_written,20000000u);
    assert(gui_playback_scan_has_pressure(&pressure,1200000u));
    atomic_store(&manager.stats[BUF_RECORD_B].bytes_read,20000000u);
    assert(!gui_playback_scan_has_pressure(&pressure,1300000u));
    atomic_fetch_add(&manager.stats[BUF_RECORD_A].write_drops,1u);
    assert(gui_playback_scan_has_pressure(&pressure,1300000u));
    atomic_store(&state,PLAYBACK_STATE_PAUSED);
    assert(!gui_playback_scan_has_pressure(&pressure,1550000u));
    atomic_store(&state,PLAYBACK_STATE_PLAYING);
    assert(!gui_playback_scan_has_pressure(&pressure,1550000u));
    assert(gui_playback_scan_has_pressure(&pressure,1800000u)); // Real producer stopped advancing
    atomic_fetch_add(&manager.stats[BUF_CAPTURE_RF].bytes_written,262144u);
    atomic_fetch_add(&manager.stats[BUF_CAPTURE_RF].bytes_read,262144u);
    assert(!gui_playback_scan_has_pressure(&pressure,1800001u));
    // A transient read-ahead counter snapshot is clamped, never unsigned-wrap pressure.
    atomic_store(&manager.stats[BUF_CAPTURE_RF].bytes_read,UINT64_MAX);
    assert(!gui_playback_scan_has_pressure(&pressure,1800002u));
    puts("Playback index pressure: RF/record backlog, drop cooldown, starvation, recovery and snapshot clamp passed (atomic counter injection)");
}

static void start_test_index_pressure_cancel(const char *unknown) {
    start_test_begin_held_index(NULL,unknown);
    start_test_pause_stable();
    atomic_store(&start_test_app.buffers.stats[BUF_RECORD_A].bytes_written,20000000u);
    atomic_store(&start_test_app.buffers.stats[BUF_RECORD_A].bytes_read,0);
    atomic_store(&start_test_release_read,true);
    uint64_t begin=start_test_clock_ms();
    playback_index_progress_t progress;
    do {
        assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS);
        assert(gui_playback_get_index_progress(&start_test_app,0,&progress));
        if(!progress.waiting) thrd_sleep_ms(1);
    } while(!progress.waiting);
    assert(progress.bytes_scanned>0 && progress.bytes_scanned==progress.total_bytes);
    start_test_stop_clean(); // Requests cancel while the pressure wait is active.
    assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS);
    assert(atomic_load(&s_playback_index.bytes_scanned[0])==0 && !atomic_load(&s_playback_index.waiting[0]));
    bufmgr_reset_stats(&start_test_app.buffers,BUF_COUNT);
    puts("Playback index pressure wait: visible nonzero progress, stop cancellation and cleared progress passed");
}

static void start_test_index_both_queued(const char *unknown,const char *other_unknown) {
    atomic_store(&start_test_channel_mask,3u);
    atomic_store(&start_test_sink_writes,0);
    start_test_arm_read();
    atomic_store(&start_test_slow_read_count,PLAYBACK_FRAME_SCAN_CHUNK_BYTES);
    assert(gui_playback_start_async(&start_test_app,unknown,other_unknown)==0);
    assert(start_test_poll_complete()==1);
    start_test_wait_flag(&start_test_read_entered);
    playback_index_progress_t progress_a,progress_b;
    assert(gui_playback_get_index_progress(&start_test_app,0,&progress_a));
    assert(gui_playback_get_index_progress(&start_test_app,1,&progress_b));
    assert(progress_a.active && !progress_a.queued && progress_a.bytes_scanned==0);
    assert(progress_b.active && progress_b.queued && progress_b.bytes_scanned==0 && !progress_b.waiting);
    start_test_wait_writes(0); // A/B producer proceeds before either length is known.
    atomic_store(&start_test_release_read,true);
    start_test_wait_index();
    playback_file_info_t a,b;
    assert(gui_playback_get_file_info_a(&start_test_app,&a) && a.total_samples==START_TEST_SAMPLES);
    assert(gui_playback_get_file_info_b(&start_test_app,&b) && b.total_samples==START_TEST_OTHER_SAMPLES);
    start_test_stop_clean();
    puts("Playback index A/B: B Queued, both raw channels before lengths, independent correct totals and stop reset passed");
}

static uint16_t start_test_crc16(const uint8_t *bytes,size_t count) {
    uint16_t crc=0;
    for(size_t n=0;n<count;++n) {
        crc^=(uint16_t)((uint16_t)bytes[n]<<8);
        for(int bit=0;bit<8;++bit) crc=(uint16_t)((crc<<1)^((crc&0x8000u)?0x8005u:0u));
    }
    return crc;
}

static void start_test_scan_only_begin(const char *fixture,bool held) {
    playback_ctx_t *ctx=start_test_calloc(1,sizeof(*ctx)); assert(ctx);
    assert(gui_playback_prepare(ctx,&start_test_app,fixture,NULL,NULL)==0);
    assert(ctx->info_a.valid && ctx->info_a.total_samples==0);
    atomic_store(&ctx->state,PLAYBACK_STATE_PAUSED); // No producer/starvation in this metadata-only test
    s_playback=ctx;
    if(held) {
        start_test_arm_read();
        atomic_store(&start_test_slow_read_count,PLAYBACK_FRAME_SCAN_CHUNK_BYTES);
    }
    gui_playback_start_index(ctx);
    if(held) start_test_wait_flag(&start_test_read_entered);
}

static void start_test_cache_reject_changed(const char *normal,const char *unknown) {
    start_test_zero_total_copy(normal,unknown);
    gui_playback_length_cache_key_t before,after; uint64_t cached=999;
    assert(gui_playback_length_cache_snapshot(unknown,&before));
    assert(!gui_playback_length_cache_lookup(&before,&cached));
    start_test_scan_only_begin(unknown,true);
    // Change only the STREAMINFO MD5 while its audio scan is held. MD5 checking
    // is disabled by normal playback; audio/header counting is unaffected, but
    // the file key must prevent publishing or persisting this mixed snapshot.
    FILE *file=fopen(unknown,"r+b"); assert(file);
    assert(fseek(file,26,SEEK_SET)==0); int previous=fgetc(file); assert(previous>=0);
    assert(fseek(file,26,SEEK_SET)==0 && fputc(previous^1,file)!=EOF); fclose(file);
    assert(gui_playback_length_cache_snapshot(unknown,&after));
    assert(!gui_playback_length_cache_keys_equal(&before,&after));
    atomic_store(&start_test_release_read,true);
    start_test_wait_index();
    assert(s_playback->info_a.total_samples==0);
    assert(!gui_playback_length_cache_lookup(&before,&cached) && cached==0);
    assert(!gui_playback_length_cache_lookup(&after,&cached) && cached==0);
    gui_playback_retire_context(); start_test_assert_clean();
    puts("Playback index input changed during scan: completed counter discarded, no publication/cache for either snapshot passed");
}

static void start_test_scan_reject_partial(const char *embedded,const char *damaged) {
    uint8_t bytes[318];
    FILE *file=fopen(embedded,"rb"); assert(file);
    assert(fread(bytes,1,180,file)==180); fclose(file);
    // #0 and #2 are individually valid frames. For max=0, concatenated CRC16
    // still has residue zero; the skipped frame number must reject the result.
    memcpy(bytes+180,bytes+42,138);
    bytes[180+4]=2;
    uint8_t header_crc=0;
    for(unsigned n=0;n<6;++n) header_crc=playback_crc8_table[(uint8_t)(header_crc^bytes[180+n])];
    bytes[180+6]=header_crc;
    uint16_t frame_crc=start_test_crc16(bytes+180,136);
    bytes[180+136]=(uint8_t)(frame_crc>>8); bytes[180+137]=(uint8_t)frame_crc;
    assert(start_test_crc16(bytes+42,276)==0);
    file=fopen(damaged,"wb"); assert(file);
    assert(fwrite(bytes,1,sizeof(bytes),file)==sizeof(bytes)); fclose(file);
    uint64_t total=999;
    assert(!start_test_count_samples(damaged,42,138,138,&total,NULL) && total==0);
    assert(!start_test_count_samples(damaged,42,138,0,&total,NULL) && total==0);
    // A valid final header with a damaged final CRC cannot publish any total.
    bytes[179]^=1;
    file=fopen(damaged,"wb"); assert(file);
    assert(fwrite(bytes,1,180,file)==180); fclose(file);
    assert(!start_test_count_samples(damaged,42,138,138,&total,NULL) && total==0);
    // Truncating just the final footer also remains unknown.
    file=fopen(damaged,"wb"); assert(file);
    assert(fwrite(bytes,1,178,file)==178); fclose(file);
    assert(!start_test_count_samples(damaged,42,138,0,&total,NULL) && total==0);
    // libFLAC can reach EOS without an error for 1..6 bytes of an incomplete
    // next frame header. Only its last fully completed boundary may prove EOF.
    file=fopen(embedded,"rb"); assert(file);
    assert(fread(bytes,1,180,file)==180); fclose(file);
    for(unsigned tail=1;tail<=6;++tail) {
        memcpy(bytes+180,bytes+42,tail);
        if(tail>4) bytes[184]=1;
        file=fopen(damaged,"wb"); assert(file);
        assert(fwrite(bytes,1,180+tail,file)==180+tail); fclose(file);
        assert(!start_test_count_samples(damaged,42,0,0,&total,NULL) && total==0);
    }
    start_test_assert_clean();
    puts("Playback length grammar: skipped sequential frame with concatenated CRC=0, bad final CRC, truncated footer and 1..6 partial tail bytes remain unknown passed");
}

static uint64_t start_test_native_count(const char *fixture) {
    uint64_t decoded=0;
    FLAC__StreamDecoder *decoder=FLAC__stream_decoder_new(); assert(decoder);
    assert(FLAC__stream_decoder_init_file(decoder,fixture,start_test_embedded_write,NULL,
        start_test_embedded_error,&decoded)==FLAC__STREAM_DECODER_INIT_STATUS_OK);
    assert(FLAC__stream_decoder_process_until_end_of_stream(decoder));
    assert(FLAC__stream_decoder_finish(decoder)); FLAC__stream_decoder_delete(decoder);
    return decoded;
}

static void start_test_grammar_embedded(const char *embedded,const char *fixture) {
    uint8_t original[180],media[192];
    FILE *file=fopen(embedded,"rb"); assert(file);
    assert(fread(original,1,sizeof(original),file)==sizeof(original)); fclose(file);
    for(unsigned kind=0;kind<2;++kind) {
        size_t size=kind?192u:180u;
        uint64_t expected=kind?128u:64u;
        if(!kind) {
            memcpy(media,original,180); memset(media+12,0,3); // Legal unknown minimum
        } else {
            memcpy(media,original,42);
            media[12]=0; media[13]=0; media[14]=12; // True 12B constant + 138B verbatim frames
            memcpy(media+42,original+42,7); memset(media+49,0,3);
            uint16_t crc=start_test_crc16(media+42,10);
            media[52]=(uint8_t)(crc>>8); media[53]=(uint8_t)crc;
            memcpy(media+54,original+42,138);
            const unsigned starts[3]={0,12,23}, ends[3]={12,23,138};
            for(unsigned n=0;n<3;++n) {
                uint8_t *header=media+54+starts[n]; header[4]=(uint8_t)(n+1u);
                uint8_t header_crc=0;
                for(unsigned b=0;b<6;++b) header_crc=playback_crc8_table[(uint8_t)(header_crc^header[b])];
                header[6]=header_crc;
            }
            for(unsigned n=0;n<3;++n) {
                uint16_t segment=start_test_crc16(media+54+starts[n],ends[n]-starts[n]-2u);
                media[54+ends[n]-2u]=(uint8_t)(segment>>8);
                media[54+ends[n]-1u]=(uint8_t)segment;
            }
            assert(start_test_crc16(media+54,138)==0);
        }
        file=fopen(fixture,"wb"); assert(file);
        assert(fwrite(media,1,size,file)==size); fclose(file);
        assert(start_test_native_count(fixture)==expected); // Real valid PCM, no decoder error
        playback_streaminfo_aux_t aux=start_test_stream_aux(fixture);
        assert(aux.min_frame_size==(kind?12u:0u) && aux.max_frame_size==138u);
        uint64_t total=0;
        assert(start_test_count_samples(fixture,42,aux.min_frame_size,
            aux.max_frame_size,&total,NULL) && total==expected);
        assert(total!=192u); // The retired header-only scanner accepted 192 in both cases.
        gui_playback_length_cache_key_t key;
        assert(gui_playback_length_cache_snapshot(fixture,&key));
        start_test_scan_only_begin(fixture,false); start_test_wait_index();
        assert(s_playback->info_a.total_samples==expected);
        assert(gui_playback_length_cache_lookup(&key,&total) && total==expected);
        gui_playback_retire_context(); start_test_assert_clean();
        printf("Playback grammar embedded payload: legal min=%u native=%" PRIu64
               ", actual scanner/index/cache=%" PRIu64 " (never192) passed\n",aux.min_frame_size,expected,total);
    }
}

static void start_test_grammar_cache_boundary(const char *directory) {
    char normal[640],unknown[640];
    snprintf(normal,sizeof(normal),"%s/start-grammar-noise-normal.flac",directory);
    snprintf(unknown,sizeof(unknown),"%s/start-grammar-noise-unknown.flac",directory);
    const uint32_t samples=5u*1024u*1024u+137u;
    FLAC__StreamEncoder *encoder=FLAC__stream_encoder_new(); assert(encoder);
    assert(FLAC__stream_encoder_set_channels(encoder,1));
    assert(FLAC__stream_encoder_set_bits_per_sample(encoder,16));
    assert(FLAC__stream_encoder_set_sample_rate(encoder,20000));
    assert(FLAC__stream_encoder_set_compression_level(encoder,0));
    assert(FLAC__stream_encoder_set_blocksize(encoder,START_TEST_BLOCK));
    assert(FLAC__stream_encoder_init_file(encoder,normal,NULL,NULL)==FLAC__STREAM_ENCODER_INIT_STATUS_OK);
    FLAC__int32 block[START_TEST_BLOCK]; uint32_t random=0x12345678u;
    for(uint32_t offset=0;offset<samples;offset+=START_TEST_BLOCK) {
        uint32_t size=samples-offset; if(size>START_TEST_BLOCK) size=START_TEST_BLOCK;
        for(uint32_t n=0;n<size;++n) {
            random^=random<<13; random^=random>>17; random^=random<<5;
            block[n]=(FLAC__int32)(random&65535u)-32768;
        }
        assert(FLAC__stream_encoder_process_interleaved(encoder,block,size));
    }
    assert(FLAC__stream_encoder_finish(encoder)); FLAC__stream_encoder_delete(encoder);
    start_test_zero_total_copy(normal,unknown);
    playback_streaminfo_aux_t aux=start_test_stream_aux(unknown);
    uint64_t offset=0,size=0,total=0;
    assert(gui_playback_find_audio_offset_and_size(unknown,&offset,&size));
    assert(size>PLAYBACK_FRAME_SCAN_CHUNK_BYTES); // Actual grammar/read-cache rollover
    atomic_uint_fast64_t scanned; atomic_init(&scanned,0);
    atomic_bool waiting; atomic_init(&waiting,false);
    playback_scan_observer_t observer={.bytes_scanned=&scanned,.waiting=&waiting};
    assert(gui_playback_scan_samples(unknown,offset,aux.min_frame_size,aux.max_frame_size,
        &total,NULL,false,&observer) && total==samples);
    assert(atomic_load(&scanned)==size-offset && !atomic_load(&waiting));
    assert(start_test_native_count(unknown)==samples);
    remove(normal); remove(unknown); start_test_assert_clean();
    puts("Playback grammar 8 MiB cache rollover: real random mono16 FLAC, last short block, full native count and exact audio-byte progress passed");
}

static void start_test_snapshot_unavailable(const char *unknown) {
    gui_playback_length_cache_key_t key; uint64_t total=0,offset=0,size=0;
    playback_streaminfo_aux_t aux=start_test_stream_aux(unknown);
    assert(gui_playback_length_cache_snapshot(unknown,&key));
    assert(gui_playback_find_audio_offset_and_size(unknown,&offset,&size));
    assert(start_test_count_samples(unknown,offset,aux.min_frame_size,
        aux.max_frame_size,&total,NULL) && total==START_TEST_SAMPLES);
    assert(gui_playback_length_cache_store(&key,total)); // A real hit exists, but cannot bind to an unavailable loaded key.
    atomic_store(&start_test_snapshot_failure_once,true);
    atomic_store(&start_test_channel_mask,1u); atomic_store(&start_test_sink_writes,0);
    assert(gui_playback_start_async(&start_test_app,unknown,NULL)==0);
    assert(start_test_poll_complete()==1);
    assert(!atomic_load(&start_test_snapshot_failure_once));
    assert(!s_playback->loaded_key_valid[0]);
    playback_file_info_t info; assert(gui_playback_get_file_info_a(&start_test_app,&info));
    assert(info.total_samples==0 && !gui_playback_is_indexing(&start_test_app,0));
    start_test_wait_writes(0); // Real libFLAC/producer remains usable.
    start_test_stop_clean();
    puts("Playback optional key unavailable: Connect/raw producer succeed, cache hit ignored and index disabled passed (fingerprint failure injected)");
}

static void start_test_held_source_identity(const char *normal,const char *other_unknown) {
    gui_playback_length_cache_key_t old_key,new_key;
    assert(gui_playback_length_cache_snapshot(normal,&old_key));
    assert(gui_playback_length_cache_snapshot(other_unknown,&new_key));
    playback_streaminfo_aux_t aux=start_test_stream_aux(other_unknown);
    uint64_t offset=0,size=0,count=0;
    assert(gui_playback_find_audio_offset_and_size(other_unknown,&offset,&size));
    assert(start_test_count_samples(other_unknown,offset,aux.min_frame_size,
        aux.max_frame_size,&count,NULL) && count==START_TEST_OTHER_SAMPLES);
    assert(gui_playback_length_cache_store(&new_key,count));
    FILE *held=fopen(normal,"rb"); assert(held);
    assert(fseek(held,37,SEEK_SET)==0); long position=ftell(held);
    assert(gui_playback_length_cache_matches_open_file(held,&old_key));
    assert(!gui_playback_length_cache_matches_open_file(held,&new_key));
    playback_total_source_t source=PLAYBACK_TOTAL_SOURCE_UNRESOLVED; double rate=0;
    assert(gui_playback_resolve_total_samples(other_unknown,20000,16,0,&aux,&source,&rate,
        NULL,NULL,NULL)==START_TEST_OTHER_SAMPLES && source==PLAYBACK_TOTAL_SOURCE_LENGTH_CACHE);
    assert(gui_playback_resolve_total_samples(other_unknown,20000,16,0,&aux,&source,&rate,
        NULL,&old_key,held)==0); // Path no longer names the loaded file.
    assert(gui_playback_resolve_total_samples(other_unknown,20000,16,0,&aux,&source,&rate,
        NULL,&new_key,held)==0); // Even a matching path key cannot override the held FILE.
    assert(ftell(held)==position);
    playback_scan_observer_t observer={.expected_key=&old_key,.held_source_file=held};
    count=999;
    assert(!gui_playback_scan_samples(other_unknown,offset,aux.min_frame_size,aux.max_frame_size,
        &count,NULL,false,&observer) && count==0); // Actual scanner FILE is the wrong identity.
    observer.expected_key=&new_key;
    assert(!gui_playback_scan_samples(other_unknown,offset,aux.min_frame_size,aux.max_frame_size,
        &count,NULL,false,&observer) && count==0); // Actual producer FILE is the wrong identity.
    fclose(held);
    playback_ctx_t *ctx=start_test_calloc(1,sizeof(*ctx)); assert(ctx);
    assert(gui_playback_prepare(ctx,&start_test_app,normal,NULL,NULL)==0);
    snprintf(ctx->info_a.filepath,sizeof(ctx->info_a.filepath),"%s",other_unknown);
    ctx->info_a.total_samples=0; ctx->info_a.duration_seconds=0;
    ctx->loaded_key[0]=new_key; ctx->loaded_key_valid[0]=true;
    atomic_store(&ctx->state,PLAYBACK_STATE_PAUSED); s_playback=ctx;
    atomic_store(&start_test_scan_malloc_failure_once,true);
    gui_playback_start_index(ctx); start_test_wait_index();
    assert(s_playback->info_a.total_samples==0 && atomic_exchange(&start_test_scan_malloc_failure_once,false));
    assert(gui_playback_length_cache_lookup(&new_key,&count) && count==START_TEST_OTHER_SAMPLES);
    gui_playback_retire_context(); start_test_assert_clean();
    puts("Playback held/source identity: real cache hit rejected for old FILE/new path, scanner and producer handles checked, index never opens mismatched source passed (two-file identity injection)");
}

static void start_test_change_after_prepare(const char *normal,const char *unknown) {
    start_test_zero_total_copy(normal,unknown);
    atomic_store(&start_test_channel_mask,1u);
    assert(gui_playback_start_async(&start_test_app,unknown,NULL)==0);
    start_test_wait_flag(&s_playback_start.done); // Loader complete; UI has not committed it.
    FILE *file=fopen(unknown,"r+b"); assert(file);
    assert(fseek(file,26,SEEK_SET)==0); int byte=fgetc(file); assert(byte>=0);
    assert(fseek(file,26,SEEK_SET)==0 && fputc(byte^1,file)!=EOF); fclose(file);
    assert(gui_playback_poll_start(&start_test_app)==-1);
    start_test_assert_clean();
    start_test_zero_total_copy(normal,unknown);
    puts("Playback input changed after private prepare: UI commit rejects observed mismatch and releases prepared resources passed");
}

static void start_test_cache_flow(const char *directory,const char *normal,const char *unknown,
                                  const char *other_normal,const char *damaged,const char *short_unknown,
                                  const char *other_unknown,const char *embedded) {
    char cache_dir[640];
    snprintf(cache_dir,sizeof(cache_dir),"%s/cache-backend-%" PRIu64,directory,gui_playback_clock_us());
    start_test_cache_directory(cache_dir);
    gui_playback_length_cache_key_t before,after;
    uint64_t samples=999;
    assert(gui_playback_length_cache_snapshot(unknown,&before));
    assert(!gui_playback_length_cache_lookup(&before,&samples) && samples==0);
    start_test_index_publish(NULL,unknown,START_TEST_SAMPLES);
    assert(gui_playback_length_cache_lookup(&before,&samples) && samples==START_TEST_SAMPLES);
    // Metadata-only startup uses the actual persisted completed result. Arm a
    // scan allocation failure to prove no fallback reader was started on hit.
    atomic_store(&start_test_scan_malloc_failure_once,true);
    atomic_store(&start_test_channel_mask,1u);
    assert(gui_playback_start_async(&start_test_app,unknown,NULL)==0);
    assert(start_test_poll_complete()==1);
    playback_file_info_t info;
    assert(gui_playback_get_file_info_a(&start_test_app,&info));
    assert(info.total_samples==START_TEST_SAMPLES && !gui_playback_is_indexing(&start_test_app,0));
    assert(atomic_exchange(&start_test_scan_malloc_failure_once,false));
    start_test_wait_writes(0);
    start_test_stop_clean();
    // Same path rewritten with a different real length must miss, rescan and
    // replace its cache association; the old result must not enter the UI.
    start_test_zero_total_copy(other_normal,unknown);
    assert(gui_playback_length_cache_snapshot(unknown,&after));
    assert(!gui_playback_length_cache_keys_equal(&before,&after));
    assert(!gui_playback_length_cache_lookup(&after,&samples) && samples==0);
    start_test_index_publish(NULL,unknown,START_TEST_OTHER_SAMPLES);
    assert(gui_playback_length_cache_lookup(&after,&samples) && samples==START_TEST_OTHER_SAMPLES);
    start_test_zero_total_copy(normal,unknown);
    // A failed complete scan cannot enter persistent cache or keep Finding.
    assert(gui_playback_length_cache_snapshot(damaged,&after));
    start_test_scan_only_begin(damaged,false);
    start_test_wait_index();
    assert(s_playback->info_a.total_samples==0);
    assert(!gui_playback_length_cache_lookup(&after,&samples) && samples==0);
    gui_playback_retire_context(); start_test_assert_clean();
    start_test_cache_reject_changed(normal,unknown);
    // STREAMINFO max_frame_size=0 is legal and must use bounded EOF validation,
    // rather than reject valid streamed encodings just for missing metadata.
    start_test_zero_total_copy(short_unknown,damaged);
    FILE *file=fopen(damaged,"r+b"); assert(file);
    assert(fseek(file,15,SEEK_SET)==0);
    const uint8_t zero_max[3]={0}; assert(fwrite(zero_max,1,3,file)==3); fclose(file);
    playback_streaminfo_aux_t aux=start_test_stream_aux(damaged); assert(aux.max_frame_size==0);
    assert(gui_playback_length_cache_snapshot(damaged,&after));
    start_test_scan_only_begin(damaged,false);
    start_test_wait_index();
    assert(s_playback->info_a.total_samples==START_TEST_SAMPLES+137u);
    assert(gui_playback_length_cache_lookup(&after,&samples) && samples==START_TEST_SAMPLES+137u);
    gui_playback_retire_context(); start_test_assert_clean();
    // Both min/max frame sizes may be unknown in a valid stream. Frame grammar
    // establishes boundaries, so neither missing size is a reason to skip it.
    file=fopen(damaged,"r+b"); assert(file);
    assert(fseek(file,12,SEEK_SET)==0 && fwrite(zero_max,1,3,file)==3); fclose(file);
    aux=start_test_stream_aux(damaged); assert(aux.min_frame_size==0 && aux.max_frame_size==0);
    assert(gui_playback_length_cache_snapshot(damaged,&after));
    start_test_scan_only_begin(damaged,false); start_test_wait_index();
    assert(s_playback->info_a.total_samples==START_TEST_SAMPLES+137u);
    assert(gui_playback_length_cache_lookup(&after,&samples) && samples==START_TEST_SAMPLES+137u);
    gui_playback_retire_context(); start_test_assert_clean();
    puts("Playback valid streamed FLAC unknown min/max sizes: complete grammar count and persistent length passed");
    start_test_grammar_embedded(embedded,damaged);
    start_test_held_source_identity(normal,other_unknown);
    start_test_snapshot_unavailable(unknown);
    start_test_change_after_prepare(normal,unknown);
    start_test_cache_directory("");
    assert(!gui_playback_length_cache_lookup(&before,&samples) && samples==0);
    assert(!gui_playback_length_cache_store(&before,START_TEST_SAMPLES));
    puts("Playback completed-length cache: actual startup hit skips scan, rewritten same path misses and rescans, explicit disable passed");
}

static void start_test_tail_nested_fixture(const char *path) {
    unsigned char bytes[8254]={0};
    memcpy(bytes,"fLaC\x80\0\0\x22",8);
    bytes[8]=bytes[10]=0x10; // 4096 min/max block
    bytes[14]=11; bytes[16]=0x20; bytes[17]=9; // true frame sizes 11 / 8201
    uint64_t packed=((uint64_t)20000u<<44)|((uint64_t)15u<<36);
    for(unsigned n=0;n<8;++n) bytes[18+n]=(uint8_t)(packed>>(56u-8u*n));
    size_t offsets[4]={42,8221,8232,8243}; unsigned numbers[4]={0,100,101,102};
    unsigned char *outer=bytes+53;
    const unsigned char header[4]={0xff,0xf8,0xc0,0x08};
    memcpy(outer,header,4); outer[4]=1;
    for(unsigned n=0;n<5;++n) outer[5]=playback_crc8_table[(uint8_t)(outer[5]^outer[n])];
    outer[6]=2; // genuine verbatim PCM with three complete nested constant frames
    for(unsigned f=0;f<4;++f) {
        unsigned char *frame=bytes+offsets[f]; memcpy(frame,header,4); frame[4]=(uint8_t)numbers[f];
        for(unsigned n=0;n<5;++n) frame[5]=playback_crc8_table[(uint8_t)(frame[5]^frame[n])];
        uint16_t crc=start_test_crc16(frame,9); frame[9]=(uint8_t)(crc>>8); frame[10]=(uint8_t)crc;
    }
    uint16_t prefix=start_test_crc16(outer,8166);
    outer[8166]=(uint8_t)(prefix>>8); outer[8167]=(uint8_t)prefix;
    assert(start_test_crc16(outer,8201)==0);
    FILE *file=fopen(path,"wb"); assert(file);
    assert(fwrite(bytes,1,sizeof(bytes),file)==sizeof(bytes)); fclose(file);
    assert(start_test_native_count(path)==8192);
}

static playback_ctx_t *start_test_tail_prepare(const char *path,bool held) {
    playback_ctx_t *ctx=start_test_calloc(1,sizeof(*ctx)); assert(ctx);
    assert(gui_playback_prepare(ctx,&start_test_app,path,NULL,NULL)==0);
    atomic_store(&ctx->state,PLAYBACK_STATE_PAUSED);
    atomic_store(&ctx->speed,PLAYBACK_SPEED_0_5X); atomic_store(&ctx->loop_enabled,false);
    atomic_store(&ctx->current_sample,1234); atomic_store(&ctx->current_sample_a,1234);
    atomic_store(&ctx->channel_total_samples[0],ctx->info_a.total_samples);
    s_playback=ctx;
    if(held) { start_test_arm_read(); atomic_store(&start_test_slow_read_count,PLAYBACK_FRAME_SCAN_CHUNK_BYTES); }
    gui_playback_start_index(ctx);
    if(held) start_test_wait_flag(&start_test_read_entered);
    return ctx;
}

static void start_test_tail_producer_seek(const char *unknown,bool playing) {
    atomic_store(&start_test_channel_mask,1); atomic_store(&start_test_sink_writes,0);
    start_test_arm_read(); atomic_store(&start_test_slow_read_count,PLAYBACK_FRAME_SCAN_CHUNK_BYTES);
    assert(gui_playback_start_async(&start_test_app,unknown,NULL)==0);
    assert(start_test_poll_complete()==1); start_test_wait_flag(&start_test_read_entered);
    gui_playback_pause(&start_test_app); thrd_sleep_ms(20);
    gui_playback_seek_sample_channel(&start_test_app,0,0);
    uint64_t seek_begin=start_test_clock_ms();
    while(atomic_load(&s_playback->seek_requested_a)) {
        assert(start_test_clock_ms()-seek_begin<START_TEST_TIMEOUT_MS); thrd_sleep_ms(1);
    }
    // A successful paused seek now refills the first raw display batch before
    // holding. Preserve that actual decoder cursor when the next seek fails.
    thrd_sleep_ms(20); assert(atomic_load(&s_playback->current_sample_a)==RAW_BUFFER_SAMPLES);
    uint64_t position=atomic_load(&s_playback->current_sample_a);
    uint64_t epoch=atomic_load(&start_test_app.rf_stream_epoch);
    gui_playback_set_loop(&start_test_app,false);
    // The ordinary provisional endpoint is correct for this ramp. Publish an
    // intentionally overestimated hint to exercise the real request path.
    s_playback->info_a.total_samples=START_TEST_SAMPLES*100u;
    atomic_store(&s_playback->channel_total_samples[0],START_TEST_SAMPLES*100u);
    atomic_store(&start_test_sink_entered,false); atomic_store(&start_test_sink_release,false);
    atomic_store(&start_test_sink_hold_once,true);
    gui_playback_seek_sample_channel(&start_test_app,0,START_TEST_SAMPLES+10000u);
    if(playing) gui_playback_resume(&start_test_app);
    uint64_t begin=start_test_clock_ms();
    while(atomic_load(&s_playback->seek_requested_a)) {
        assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS); thrd_sleep_ms(1);
    }
    if(playing) start_test_wait_flag(&start_test_sink_entered); else thrd_sleep_ms(20);
    if(playing) assert(!s_playback->seek_recovery_failed[0]); // Sink release/acquire covers producer writes.
    assert(gui_playback_get_state(&start_test_app)==(playing ? PLAYBACK_STATE_PLAYING : PLAYBACK_STATE_PAUSED));
    assert(atomic_load(&s_playback->current_sample_a)==position);
    assert(atomic_load(&start_test_app.rf_stream_epoch)==epoch);
    atomic_store(&start_test_release_read,true);
    if(!playing) {
        gui_playback_resume(&start_test_app); start_test_wait_flag(&start_test_sink_entered);
        assert(!s_playback->seek_recovery_failed[0]);
        assert(atomic_load(&start_test_app.rf_stream_epoch)==epoch);
    }
    // Underestimated endpoint must not end/rewind actual producer data.
    s_playback->info_a.total_samples=1; atomic_store(&s_playback->channel_total_samples[0],1);
    unsigned writes=atomic_load(&start_test_sink_writes);
    atomic_store(&start_test_sink_release,true); start_test_wait_writes(writes);
    begin=start_test_clock_ms();
    while(atomic_load(&s_playback->current_sample_a)<=1) {
        assert(start_test_clock_ms()-begin<START_TEST_TIMEOUT_MS); thrd_sleep_ms(1);
    }
    assert(atomic_load(&start_test_app.rf_stream_epoch)==epoch);
    start_test_stop_clean();
    printf("Playback actual producer bad provisional seek %s: old cursor/epoch/state preserved, exact ramp raw resumes passed\n",
        playing ? "playing" : "paused");
}

static void start_test_tail_flow(const char *directory,const char *unknown,const char *other_unknown) {
    char nested[512],damaged[512],cache_dir[512];
    snprintf(nested,sizeof(nested),"%s/start-tail-nested.flac",directory);
    snprintf(damaged,sizeof(damaged),"%s/start-tail-damaged.flac",directory);
    snprintf(cache_dir,sizeof(cache_dir),"%s/tail-test-cache",directory);
    start_test_cache_directory(cache_dir);
    start_test_tail_nested_fixture(nested);
    atomic_store(&start_test_tail_disabled,false);
    gui_playback_length_cache_key_t key; uint64_t cached=0;
    assert(gui_playback_length_cache_snapshot(nested,&key));
    playback_ctx_t *ctx=start_test_tail_prepare(nested,true);
    assert(ctx->info_a.total_samples==421888 && ctx->info_a.length_provisional);
    assert(!ctx->info_a.length_verification_failed && gui_playback_is_indexing(&start_test_app,0));
    assert(!gui_playback_length_cache_lookup(&key,&cached)); // Tail must never write V2.
    uint64_t epoch=atomic_load(&start_test_app.rf_stream_epoch);
    atomic_store(&start_test_release_read,true); start_test_wait_index();
    assert(ctx->info_a.total_samples==8192 && !ctx->info_a.length_provisional && !ctx->info_a.length_verification_failed);
    assert(atomic_load(&ctx->current_sample)==1234 && atomic_load(&ctx->current_sample_a)==1234);
    assert(atomic_load(&ctx->state)==PLAYBACK_STATE_PAUSED && atomic_load(&ctx->speed)==PLAYBACK_SPEED_0_5X);
    assert(!atomic_load(&ctx->loop_enabled) && atomic_load(&start_test_app.rf_stream_epoch)==epoch);
    assert(gui_playback_length_cache_lookup(&key,&cached) && cached==8192);
    gui_playback_retire_context(); start_test_assert_clean();
    ctx=start_test_calloc(1,sizeof(*ctx)); assert(ctx);
    assert(gui_playback_prepare(ctx,&start_test_app,nested,NULL,NULL)==0);
    assert(ctx->info_a.total_samples==8192 && !ctx->info_a.length_provisional); // Confirmed cache beats tail.
    gui_playback_release_context(ctx); start_test_free(ctx); start_test_assert_clean();
    puts("Playback tail: genuine nested-frame8192 is temporarily421888, complete scan corrects it, controls/cursor/epoch preserved, only8192 in V2 passed");

    unsigned char bytes[8254]; FILE *file=fopen(nested,"rb"); assert(file);
    assert(fread(bytes,1,sizeof(bytes),file)==sizeof(bytes)); fclose(file);
    for(unsigned variant=0;variant<2;++variant) {
        unsigned char saved0=bytes[8252],saved1=bytes[8253];
        if(variant==0) bytes[8252]=bytes[8253]=0; // Seek-mode API suppresses this bad CRC.
        file=fopen(damaged,"wb"); assert(file);
        size_t count=sizeof(bytes)-(variant==1 ? 1u : 0u);
        assert(fwrite(bytes,1,count,file)==count); fclose(file);
        assert(gui_playback_length_cache_snapshot(damaged,&key));
        ctx=start_test_tail_prepare(damaged,true);
        if(variant==0) assert(ctx->info_a.length_provisional && ctx->info_a.total_samples>0);
        assert(!gui_playback_length_cache_lookup(&key,&cached));
        atomic_store(&start_test_release_read,true); start_test_wait_index();
        assert(ctx->info_a.total_samples==0 && ctx->info_a.duration_seconds==0 && !ctx->info_a.length_provisional);
        assert(ctx->info_a.length_verification_failed && !gui_playback_is_indexing(&start_test_app,0));
        assert(!gui_playback_length_cache_lookup(&key,&cached));
        gui_playback_retire_context(); start_test_assert_clean();
        bytes[8252]=saved0; bytes[8253]=saved1;
    }
    puts("Playback bad-tail zeroCRC/truncation: complete reader retracts provisional or unknown endpoint, failed flag, no cache passed");
    start_test_cache_directory("");
    // Tail identity is tied to the actual held producer, not a reused path key.
    FILE *held=fopen(unknown,"rb"); assert(held);
    gui_playback_length_cache_key_t other_key;
    assert(gui_playback_length_cache_snapshot(other_unknown,&other_key));
    assert(fseek(held,37,SEEK_SET)==0); long held_position=ftell(held);
    assert(!gui_playback_tail_samples(other_unknown,held,&other_key,NULL,&cached) && cached==0);
    assert(ftell(held)==held_position); fclose(held);
    // No failed-bound callback may read bytes or seek outside its permitted window.
    held=fopen(unknown,"rb"); assert(held);
    playback_tail_io_t bounded={.file=held,.file_size=16u<<20,.deadline_us=gui_playback_clock_us()+1000000u};
    unsigned char buffer[8]; size_t request=sizeof(buffer);
    bounded.metadata_bytes_read=PLAYBACK_TAIL_METADATA_BYTES;
    assert(gui_playback_tail_read_cb(NULL,buffer,&request,&bounded)==FLAC__STREAM_DECODER_READ_STATUS_ABORT && request==0);
    bounded.failed=false; bounded.tail=true; bounded.tail_bytes_read=PLAYBACK_TAIL_MAX_BYTES; request=sizeof(buffer);
    assert(gui_playback_tail_read_cb(NULL,buffer,&request,&bounded)==FLAC__STREAM_DECODER_READ_STATUS_ABORT && request==0);
    bounded.failed=false; bounded.tail_start=8u<<20;
    assert(gui_playback_tail_seek_cb(NULL,bounded.tail_start-1,&bounded)==FLAC__STREAM_DECODER_SEEK_STATUS_ERROR);
    assert(ftell(held)==0); fclose(held);
    start_test_tail_nested_fixture(damaged);
    ctx=start_test_calloc(1,sizeof(*ctx)); assert(ctx);
    assert(gui_playback_prepare(ctx,&start_test_app,damaged,NULL,NULL)==0 && ctx->info_a.length_provisional);
    file=fopen(damaged,"r+b"); assert(file); assert(fseek(file,26,SEEK_SET)==0 && fputc(1,file)!=EOF); fclose(file);
    assert(gui_playback_commit(&start_test_app,ctx)==-1); start_test_assert_clean();
    puts("Playback bounded tail: read/seek budgets, held FILE mismatch without cursor changes, changed provisional source rejected before commit passed");

    // Actual elapsed time is independent of an underestimated provisional endpoint.
    ctx=start_test_calloc(1,sizeof(*ctx)); assert(ctx);
    assert(gui_playback_prepare(ctx,&start_test_app,unknown,NULL,NULL)==0);
    ctx->info_a.total_samples=20000000; ctx->info_a.duration_seconds=1;
    ctx->info_a.length_provisional=true; ctx->total_samples=20000000;
    ctx->total_duration_seconds=1; ctx->realtime_sample_rate_hz=20000000;
    atomic_store(&ctx->current_sample,40000000); s_playback=ctx;
    assert(gui_playback_get_position_seconds(&start_test_app)==2);
    assert(gui_playback_get_position_normalized(&start_test_app)==1);
    gui_playback_retire_context(); start_test_assert_clean();

    // Budget miss is optional, then the exact full reader still succeeds.
    atomic_store(&start_test_tail_delay_once,true);
    ctx=start_test_tail_prepare(unknown,true);
    assert(ctx->info_a.total_samples==0 && !ctx->info_a.length_provisional);
    atomic_store(&start_test_release_read,true); start_test_wait_index();
    assert(ctx->info_a.total_samples==START_TEST_SAMPLES && !ctx->info_a.length_provisional);
    gui_playback_retire_context(); start_test_assert_clean();

    // Cancellation while the private loader is inside tail API cannot commit.
    atomic_store(&start_test_tail_entered,false); atomic_store(&start_test_tail_release,false);
    atomic_store(&start_test_tail_hold_once,true);
    assert(gui_playback_start_async(&start_test_app,unknown,NULL)==0);
    start_test_wait_flag(&start_test_tail_entered);
    gui_playback_cancel_start(&start_test_app);
    atomic_store(&start_test_tail_release,true);
    assert(start_test_poll_complete()==-1);
    gui_playback_finish_start(&start_test_app); start_test_assert_clean();

    // Real producer A/B starts while full scan is held, even though both have a
    // provisional endpoint; B remains queued and tail discovery is not repeated.
    atomic_store(&start_test_channel_mask,3); atomic_store(&start_test_sink_writes,0);
    start_test_arm_read(); atomic_store(&start_test_slow_read_count,PLAYBACK_FRAME_SCAN_CHUNK_BYTES);
    assert(gui_playback_start_async(&start_test_app,unknown,other_unknown)==0);
    assert(start_test_poll_complete()==1); start_test_wait_flag(&start_test_read_entered);
    playback_file_info_t a,b; playback_index_progress_t progress;
    assert(gui_playback_get_file_info_a(&start_test_app,&a) && a.length_provisional);
    assert(gui_playback_get_file_info_b(&start_test_app,&b) && b.length_provisional);
    assert(gui_playback_get_index_progress(&start_test_app,1,&progress) && progress.queued);
    start_test_wait_writes(0);
    atomic_store(&start_test_release_read,true); start_test_wait_index();
    assert(gui_playback_get_file_info_a(&start_test_app,&a) && a.total_samples==START_TEST_SAMPLES && !a.length_provisional);
    assert(gui_playback_get_file_info_b(&start_test_app,&b) && b.total_samples==START_TEST_OTHER_SAMPLES && !b.length_provisional);
    start_test_stop_clean();
    start_test_tail_producer_seek(unknown,false);
    start_test_tail_producer_seek(unknown,true);
    // Stop a provisional session before UI publication; reconnect gets only its own result.
    atomic_store(&start_test_channel_mask,1); atomic_store(&start_test_sink_writes,0);
    start_test_arm_read(); atomic_store(&start_test_slow_read_count,PLAYBACK_FRAME_SCAN_CHUNK_BYTES);
    assert(gui_playback_start_async(&start_test_app,unknown,NULL)==0);
    assert(start_test_poll_complete()==1); start_test_wait_flag(&start_test_read_entered);
    assert(s_playback->info_a.length_provisional);
    atomic_store(&start_test_release_read,true); start_test_stop_clean();
    assert(gui_playback_start_async(&start_test_app,other_unknown,NULL)==0);
    assert(start_test_poll_complete()==1); start_test_wait_index();
    assert(s_playback->info_a.total_samples==START_TEST_OTHER_SAMPLES && !s_playback->info_a.length_provisional);
    start_test_stop_clean();
    atomic_store(&start_test_tail_disabled,true);
    remove(nested); remove(damaged);
    puts("Playback bounded tail: deadline miss/Unknown fallback, pending cancel, A/B queued first raw, stop/reconnect isolation and elapsed beyond provisional total passed");
}

static void start_test_rf_rate_without_length(const char *directory) {
    char normal[512],unknown[512];
    snprintf(normal,sizeof(normal),"%s/start-rate-known.flac",directory);
    snprintf(unknown,sizeof(unknown),"%s/start-rate-unknown.flac",directory);
    const unsigned rates[]={20000000u,40000000u,80000000u};
    start_test_cache_directory("");
    atomic_store(&start_test_tail_disabled,false); // Use the real bounded tail reader.
    for(unsigned n=0;n<sizeof(rates)/sizeof(rates[0]);++n) {
        // Legal padding exceeds the tail metadata budget, independently of I/O timing.
        start_test_encode_rate_fixture(normal,START_TEST_SAMPLES,rates[n],PLAYBACK_TAIL_METADATA_BYTES*2u);
        start_test_zero_total_copy(normal,unknown);
        atomic_store(&start_test_source_rate,rates[n]);
        for(unsigned missing_length=0;missing_length<2;++missing_length) {
            const char *fixture=missing_length?unknown:normal;
            for(int channel=0;channel<2;++channel) {
                atomic_store(&start_test_sink_writes,0);
                atomic_store(&start_test_channel_mask,1u<<channel);
                assert(gui_playback_start_async(&start_test_app,channel==0?fixture:NULL,channel==1?fixture:NULL)==0);
                assert(start_test_poll_complete()==1);
                gui_playback_set_loop(&start_test_app,false);
                playback_file_info_t info=channel==0?s_playback->info_a:s_playback->info_b;
                assert(info.total_samples==(missing_length?0u:START_TEST_SAMPLES));
                assert(!info.length_provisional);
                assert(gui_playback_get_channel_rate_hz(&start_test_app,channel)==rates[n]);
                assert(atomic_load(&start_test_app.rf_source_rate_hz[channel])==rates[n]);
                assert(s_playback->realtime_sample_rate_hz==rates[n]);
                start_test_wait_writes(0);
                start_test_wait_index();
                info=channel==0?s_playback->info_a:s_playback->info_b;
                assert(info.total_samples==START_TEST_SAMPLES && !info.length_verification_failed);
                assert(fabs(info.duration_seconds-(double)START_TEST_SAMPLES/rates[n])<1e-12);
                assert(gui_playback_get_channel_rate_hz(&start_test_app,channel)==rates[n]);
                assert(fabs(gui_playback_get_duration_seconds(&start_test_app)-info.duration_seconds)<1e-12);
                start_test_stop_clean();
            }
        }
    }
    atomic_store(&start_test_source_rate,20000000u);
    atomic_store(&start_test_tail_disabled,true);
    remove(normal); remove(unknown);
    puts("Playback RF_SAMPLE_RATE 20/40/80 MHz A/B: known/unknown lengths, real padded tail miss, raw stamps and index rate/duration preserved passed");
}

int main(int argc,char **argv) {
    const char *directory=argc>1?argv[1]:".";
    char normal[512],unknown[512],bad[512],missing[512];
    char short_normal[512],short_unknown[512],embedded_headers[512];
    char other_normal[512],other_unknown[512];
    snprintf(normal,sizeof(normal),"%s/start-normal.flac",directory);
    snprintf(unknown,sizeof(unknown),"%s/start-total-zero.flac",directory);
    snprintf(bad,sizeof(bad),"%s/start-bad.flac",directory);
    snprintf(missing,sizeof(missing),"%s/start-missing.flac",directory);
    snprintf(short_normal,sizeof(short_normal),"%s/start-short-normal.flac",directory);
    snprintf(short_unknown,sizeof(short_unknown),"%s/start-short-total-zero.flac",directory);
    snprintf(embedded_headers,sizeof(embedded_headers),"%s/start-embedded-fake-headers.flac",directory);
    snprintf(other_normal,sizeof(other_normal),"%s/start-other-normal.flac",directory);
    snprintf(other_unknown,sizeof(other_unknown),"%s/start-other-total-zero.flac",directory);
    remove(missing);
    start_test_make_fixture(normal,unknown,bad);
    start_test_encode_fixture(short_normal,START_TEST_SAMPLES+137u);
    start_test_zero_total_copy(short_normal,short_unknown);
    start_test_encode_fixture(other_normal,START_TEST_OTHER_SAMPLES);
    start_test_zero_total_copy(other_normal,other_unknown);
    start_test_cache_directory(""); // Existing held-read cases must always exercise the real scanner.
    gui_playback_length_cache_key_t disabled_key; uint64_t disabled_samples=99;
    assert(gui_playback_length_cache_snapshot(unknown,&disabled_key));
    assert(!gui_playback_length_cache_store(&disabled_key,START_TEST_SAMPLES));
    assert(!gui_playback_length_cache_lookup(&disabled_key,&disabled_samples) && disabled_samples==0);
    start_test_stale_stop_request(normal);
    start_test_play(normal,false,1u);
    start_test_play(normal,false,2u);
    start_test_play(normal,false,3u);
    start_test_play(unknown,true,1u);
    assert(gui_playback_start_async(&start_test_app,missing,NULL)==0);
    assert(start_test_poll_complete()==-1);
    gui_playback_finish_start(&start_test_app); start_test_assert_clean();
    atomic_store(&start_test_init_failure_once,true);
    assert(gui_playback_start_async(&start_test_app,normal,NULL)==0);
    assert(start_test_poll_complete()==-1);
    assert(!atomic_load(&start_test_init_failure_once));
    gui_playback_finish_start(&start_test_app); start_test_assert_clean();
    start_test_play(normal,false,1u);
    puts("Playback private init_stream simulated OOM: caller-owned file closed once, resources zero, retry passed");
    assert(gui_playback_start_async(&start_test_app,normal,bad)==0);
    assert(start_test_poll_complete()==-1);
    gui_playback_finish_start(&start_test_app); start_test_assert_clean();
    atomic_store(&start_test_extract_failure,true);
    assert(gui_playback_start_async(&start_test_app,normal,NULL)==0);
    assert(start_test_poll_complete()==-1);
    gui_playback_finish_start(&start_test_app); start_test_assert_clean();
    atomic_store(&start_test_extract_failure,false);
    puts("Playback missing A, damaged B and extraction-start failure cleanup passed");
    start_test_slow_heartbeat(normal);
    start_test_cancel_retry(normal);
    start_test_exit_pending(normal);
    start_test_index_publish(NULL,unknown,START_TEST_SAMPLES);
    start_test_index_publish(normal,other_unknown,START_TEST_OTHER_SAMPLES);
    start_test_index_stop_reopen(unknown,other_normal);
    start_test_index_eof(unknown);
    start_test_index_failure(unknown,0u);
    start_test_index_failure(unknown,1u);
    start_test_index_failure(unknown,2u);
    start_test_pressure_policy();
    start_test_index_pressure_cancel(unknown);
    start_test_index_both_queued(unknown,other_unknown);
    start_test_scan_exact(short_unknown);
    start_test_scan_cancel(short_unknown);
    start_test_embedded_headers(embedded_headers);
    start_test_scan_reject_partial(embedded_headers,bad);
    start_test_grammar_cache_boundary(directory);
    start_test_cache_flow(directory,normal,unknown,other_normal,bad,short_unknown,other_unknown,embedded_headers);
    start_test_tail_flow(directory,unknown,other_unknown);
    start_test_rf_rate_without_length(directory);
    remove(normal); remove(unknown); remove(bad);
    remove(short_normal); remove(short_unknown);
    remove(embedded_headers);
    remove(other_normal); remove(other_unknown);
    puts("Production async playback startup/background indexing regression passed (headless; no hardware/UI claims)");
    return 0;
}
