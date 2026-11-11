/* Real libFLAC seek regression against the production playback helper and
 * write callbacks. FLAC seek_absolute may call write_cb synchronously; clearing
 * its buffers afterwards loses the exact target and silently skips a frame.
 * LTO discards unrelated GUI/capture paths from this included implementation. */
#include "../misrc_gui/input/gui_playback.c"
#include <FLAC/stream_encoder.h>
#include <assert.h>

#define SEEK_TEST_SAMPLES 24576u
#define SEEK_TEST_BLOCK 4096u

static int16_t seek_test_sample(uint64_t position) {
    return (int16_t)((int)(position%4096u)-2048);
}

static FLAC__StreamEncoderWriteStatus seek_test_encoder_write(
    const FLAC__StreamEncoder *encoder, const FLAC__byte bytes[], size_t count,
    unsigned samples, unsigned frame, void *context) {
    (void)encoder; (void)samples; (void)frame;
    return fwrite(bytes,1,count,(FILE *)context)==count
        ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK
        : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
}

static FILE *seek_test_file(void) {
    /* tmpfile removes the fixture on close. A custom encoder write callback
     * retains FILE ownership, so the actual decoder can read and seek it. */
    FILE *file=tmpfile(); assert(file);
    FLAC__StreamEncoder *encoder=FLAC__stream_encoder_new(); assert(encoder);
    assert(FLAC__stream_encoder_set_channels(encoder,1));
    assert(FLAC__stream_encoder_set_bits_per_sample(encoder,16));
    assert(FLAC__stream_encoder_set_sample_rate(encoder,20000));
    assert(FLAC__stream_encoder_set_compression_level(encoder,0));
    assert(FLAC__stream_encoder_set_blocksize(encoder,SEEK_TEST_BLOCK));
    assert(FLAC__stream_encoder_set_total_samples_estimate(encoder,SEEK_TEST_SAMPLES));
    assert(FLAC__stream_encoder_init_stream(encoder,seek_test_encoder_write,
        NULL,NULL,NULL,file)==FLAC__STREAM_ENCODER_INIT_STATUS_OK);
    FLAC__int32 block[SEEK_TEST_BLOCK];
    for (unsigned offset=0; offset<SEEK_TEST_SAMPLES; offset+=SEEK_TEST_BLOCK) {
        for (unsigned n=0; n<SEEK_TEST_BLOCK; ++n)
            block[n]=(FLAC__int32)seek_test_sample(offset+n)*16;
        assert(FLAC__stream_encoder_process_interleaved(encoder,block,SEEK_TEST_BLOCK));
    }
    assert(FLAC__stream_encoder_finish(encoder));
    FLAC__stream_encoder_delete(encoder);
    assert(fflush(file)==0); rewind(file);
    return file;
}

static void seek_test_decoder_error(const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus error, void *context) {
    (void)decoder; (void)context;
    fprintf(stderr,"unexpected FLAC fixture error %d\n",(int)error);
    assert(false);
}

static void seek_test_channel(int channel) {
    playback_ctx_t ctx={0};
    ctx.decode_buf_size=SEEK_TEST_SAMPLES*2u;
    ctx.decode_buf_a=calloc(ctx.decode_buf_size,sizeof(int16_t));
    ctx.decode_buf_b=calloc(ctx.decode_buf_size,sizeof(int16_t));
    assert(ctx.decode_buf_a && ctx.decode_buf_b);
    FLAC__StreamDecoder *decoder=FLAC__stream_decoder_new(); assert(decoder);
    if (channel==0) ctx.decoder_a=decoder; else ctx.decoder_b=decoder;
    FILE *file=seek_test_file();
    assert(FLAC__stream_decoder_init_FILE(decoder,file,
        channel==0 ? decoder_write_cb_a : decoder_write_cb_b,
        NULL,seek_test_decoder_error,&ctx)==FLAC__STREAM_DECODER_INIT_STATUS_OK);
    assert(FLAC__stream_decoder_process_until_end_of_metadata(decoder));
    assert(FLAC__stream_decoder_process_single(decoder));
    size_t *available=channel==0 ? &ctx.decode_available_a : &ctx.decode_available_b;
    size_t *position=channel==0 ? &ctx.decode_pos_a : &ctx.decode_pos_b;
    int16_t *samples=channel==0 ? ctx.decode_buf_a : ctx.decode_buf_b;
    size_t *other_available=channel==0 ? &ctx.decode_available_b : &ctx.decode_available_a;
    size_t *other_position=channel==0 ? &ctx.decode_pos_b : &ctx.decode_pos_a;
    assert(*available==SEEK_TEST_BLOCK && samples[0]==seek_test_sample(0));
    /* Both nonzero stale buffer coordinates and unread samples must be cleared
     * before seek's internal callback; only the new target slice survives. */
    *position=73; *other_available=123; *other_position=17;
    assert(gui_playback_seek_decoder(&ctx,channel,10000,10000));
    assert(*position==0 && *available==SEEK_TEST_BLOCK-(10000u%SEEK_TEST_BLOCK));
    assert(samples[*position]==seek_test_sample(10000));
    for (size_t n=0; n<*available; ++n)
        assert(samples[n]==seek_test_sample(10000+n));
    assert(*other_available==123 && *other_position==17);

    /* Exercise the caller-visible slice without another process_single, then
     * reach real EOF and rewind with the same helper used by loop playback. */
    *position=*available;
    assert(FLAC__stream_decoder_process_until_end_of_stream(decoder));
    assert(FLAC__stream_decoder_get_state(decoder)==FLAC__STREAM_DECODER_END_OF_STREAM);
    *position=*available;
    assert(gui_playback_seek_decoder(&ctx,channel,0,0));
    assert(*position==0 && *available==SEEK_TEST_BLOCK);
    for (size_t n=0; n<*available; ++n) assert(samples[n]==seek_test_sample(n));
    assert(gui_playback_seek_decoder(&ctx,channel,SEEK_TEST_SAMPLES-1u,SEEK_TEST_SAMPLES-1u));
    assert(*position==0 && *available==1 && samples[0]==seek_test_sample(SEEK_TEST_SAMPLES-1u));

    atomic_store(&ctx.current_sample,321);
    atomic_store(&ctx.current_sample_a,123);
    atomic_store(&ctx.current_sample_b,456);
    assert(!gui_playback_seek_decoder(&ctx,channel,SEEK_TEST_SAMPLES+10000u,SEEK_TEST_SAMPLES+10000u));
    assert(!ctx.seek_recovery_failed[channel]);
    assert(*position==0 && *available==SEEK_TEST_BLOCK-(channel==0 ? 123u : 456u));
    assert(samples[0]==seek_test_sample(channel==0 ? 123u : 456u));
    assert(atomic_load(&ctx.current_sample)==321);
    assert(atomic_load(&ctx.current_sample_a)==123 && atomic_load(&ctx.current_sample_b)==456);
    assert(*other_available==123 && *other_position==17);
    FLAC__stream_decoder_finish(decoder); /* Also closes/removes tmpfile. */
    FLAC__stream_decoder_delete(decoder);
    free(ctx.decode_buf_a); free(ctx.decode_buf_b);
    printf("Playback channel %c: exact seek 10000, EOF loop 0, last sample, failed seek passed\n",
           channel==0 ? 'A' : 'B');
}

static void seek_test_unknown_recovery(int channel, bool paused, unsigned endpoint) {
    bool eof=endpoint!=0;
    playback_ctx_t ctx={0};
    ctx.decode_buf_size=SEEK_TEST_SAMPLES*2u;
    ctx.decode_buf_a=calloc(ctx.decode_buf_size,sizeof(int16_t));
    ctx.decode_buf_b=calloc(ctx.decode_buf_size,sizeof(int16_t));
    assert(ctx.decode_buf_a && ctx.decode_buf_b);
    FILE *file=seek_test_file();
    assert(fseek(file,21,SEEK_SET)==0);
    int high=fgetc(file); assert(high>=0);
    assert(fseek(file,21,SEEK_SET)==0 && fputc(high&0xf0,file)!=EOF);
    const unsigned char zeros[4]={0}; assert(fwrite(zeros,1,4,file)==4); rewind(file);
    FLAC__StreamDecoder *decoder=FLAC__stream_decoder_new(); assert(decoder);
    if(channel==0) ctx.decoder_a=decoder; else ctx.decoder_b=decoder;
    assert(FLAC__stream_decoder_init_FILE(decoder,file,
        channel==0 ? decoder_write_cb_a : decoder_write_cb_b,NULL,
        seek_test_decoder_error,&ctx)==FLAC__STREAM_DECODER_INIT_STATUS_OK);
    assert(FLAC__stream_decoder_process_until_end_of_metadata(decoder));
    assert(FLAC__stream_decoder_get_total_samples(decoder)==0);
    uint64_t previous=eof ? SEEK_TEST_SAMPLES : 1234u;
    size_t *available=channel==0 ? &ctx.decode_available_a : &ctx.decode_available_b;
    size_t *position=channel==0 ? &ctx.decode_pos_a : &ctx.decode_pos_b;
    int16_t *samples=channel==0 ? ctx.decode_buf_a : ctx.decode_buf_b;
    if(eof) {
        if(endpoint==1) assert(FLAC__stream_decoder_process_until_end_of_stream(decoder));
        else {
            while(*available<SEEK_TEST_SAMPLES) assert(FLAC__stream_decoder_process_single(decoder));
            assert(FLAC__stream_decoder_get_state(decoder)==FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC);
            FLAC__uint64 boundary=0; assert(FLAC__stream_decoder_get_decode_position(decoder,&boundary));
            ctx.loaded_key_valid[channel]=true; ctx.loaded_key[channel].file_size=boundary;
        }
        *position=*available;
    } else {
        assert(FLAC__stream_decoder_process_single(decoder)); *position=(size_t)previous;
    }
    atomic_store(&ctx.current_sample_a,previous); atomic_store(&ctx.current_sample_b,previous);
    atomic_store(&ctx.current_sample,previous);
    atomic_store(&ctx.state,paused ? PLAYBACK_STATE_PAUSED : PLAYBACK_STATE_PLAYING);
    atomic_store(&ctx.speed,PLAYBACK_SPEED_0_5X); atomic_store(&ctx.loop_enabled,!eof);
    ctx.info_a.length_provisional=true; ctx.info_b.length_provisional=true;
    ctx.info_a.total_samples=ctx.info_b.total_samples=SEEK_TEST_SAMPLES*100u;
    assert(!gui_playback_seek_decoder(&ctx,channel,SEEK_TEST_SAMPLES+10000u,SEEK_TEST_SAMPLES+10000u));
    assert(!ctx.seek_recovery_failed[channel]);
    assert(atomic_load(&ctx.current_sample)==previous);
    assert(atomic_load(&ctx.current_sample_a)==previous && atomic_load(&ctx.current_sample_b)==previous);
    assert(atomic_load(&ctx.state)==(paused ? PLAYBACK_STATE_PAUSED : PLAYBACK_STATE_PLAYING));
    assert(atomic_load(&ctx.speed)==PLAYBACK_SPEED_0_5X && atomic_load(&ctx.loop_enabled)==!eof);
    if(eof) {
        assert(*available==0 && *position==0);
        assert(FLAC__stream_decoder_get_state(decoder)==FLAC__STREAM_DECODER_END_OF_STREAM);
    } else {
        assert(*position==0 && *available==SEEK_TEST_BLOCK-previous);
        for(size_t n=0;n<*available;++n) assert(samples[n]==seek_test_sample(previous+n));
        *position=*available;
        assert(FLAC__stream_decoder_process_single(decoder));
        assert(samples[*position]==seek_test_sample(SEEK_TEST_BLOCK));
    }
    FLAC__stream_decoder_finish(decoder); FLAC__stream_decoder_delete(decoder);
    free(ctx.decode_buf_a); free(ctx.decode_buf_b);
    printf("Playback provisional bad seek channel %c %s: actual decoder cursor and exact next samples restored\n",
        channel==0 ? 'A' : 'B',endpoint==2 ? "final-frame-before-EOS" : eof ? "EOF" : paused ? "paused" : "playing");
}

int main(void) {
    seek_test_channel(0); seek_test_channel(1);
    for(int channel=0;channel<2;++channel) {
        seek_test_unknown_recovery(channel,false,false);
        seek_test_unknown_recovery(channel,true,false);
        seek_test_unknown_recovery(channel,true,1);
        seek_test_unknown_recovery(channel,true,2);
    }
    return 0;
}
