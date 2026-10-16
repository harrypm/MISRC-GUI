/*
* MISRC capture
* Copyright (C) 2024-2025  vrunk11, stefan_o
* 
* based on:
* hsdaoh - High Speed Data Acquisition over MS213x USB3 HDMI capture sticks
* Copyright (C) 2024 by Steve Markgraf <steve@steve-m.de>
* 
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#if defined(__linux__)
#define _GNU_SOURCE
#include <sched.h>
#elif defined(__APPLE__) || defined(__MACH__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#if __STDC_VERSION__ >= 201112L && ! __STDC_NO_THREADS__ && ! _WIN32
#include <threads.h>
#else
#include "cthreads.h"
#warning "No C threads, fallback to pthreads/winthreads"
#endif
#include <time.h>

#include "../common/buffer.h"
#include "../common/threading.h"

#ifndef _WIN32
	#if defined(__APPLE__) || defined(__MACH__)
		#include <libkern/OSByteOrder.h>
		#define le32toh(x) OSSwapLittleToHostInt32(x)
		#define le16toh(x) OSSwapLittleToHostInt16(x)
	#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
		#include <sys/endian.h>
		#define le32toh(x) letoh32(x)
		#define le16toh(x) letoh16(x)
	#else
		#include <endian.h>
	#endif
	#include <getopt.h>
#else
	#include <windows.h>
	#include <io.h>
	#include <fcntl.h>
	#if defined(__MINGW32__)
		#include <getopt.h>
	#else
		#include "../getopt/getopt.h"
	#endif
	#define F_OK 0
	#define access _access
	#define le32toh(x) (x)
	#define le16toh(x) (x)
#endif

#include <hsdaoh.h>
#include <hsdaoh_raw.h>
#include <hsdaoh_crc.h>

#if LIBFLAC_ENABLED == 1
#include "FLAC/metadata.h"
#include "FLAC/stream_encoder.h"
# if defined(FLAC_API_VERSION_CURRENT) && FLAC_API_VERSION_CURRENT >= 14
static const char* const _FLAC_StreamEncoderSetNumThreadsStatusString[] = {
	"FLAC__STREAM_ENCODER_SET_NUM_THREADS_OK",
	"FLAC__STREAM_ENCODER_SET_NUM_THREADS_NOT_COMPILED_WITH_MULTITHREADING_ENABLED",
	"FLAC__STREAM_ENCODER_SET_NUM_THREADS_ALREADY_INITIALIZED",
	"FLAC__STREAM_ENCODER_SET_NUM_THREADS_TOO_MANY_THREADS"
};
# endif
#endif

#if LIBSOXR_ENABLED == 1
#include <soxr.h>
#endif

#include "simple_capture/simple_capture.h"

#include "../version.h"
#include "../common/ringbuffer.h"
#include "../common/hsdaoh_compat.h"
#include "../common/flac_writer.h"
#include "../common/frame_parser.h"
#include "../common/device_enum.h"
#include "../common/extract.h"
#include "../common/wave.h"
#include "../common/file_utils.h"
#include "../common/utf8_argv.h"
#include "misrc_capture_cli.h"

#if LIBFLAC_ENABLED == 1 && defined(FLAC_API_VERSION_CURRENT) && FLAC_API_VERSION_CURRENT >= 14
#include "numcores.h"
#endif

// Ringbuffer capacities (platform-agnostic byte sizing)
#define BUFFER_AUDIO_TOTAL_SIZE ((size_t)256 * 1024 * 1024)
#define BUFFER_AUDIO_READ_SIZE 65536*3
#define BUFFER_TOTAL_SIZE ((size_t)256 * 1024 * 1024)
#define BUFFER_READ_SIZE 65536*32
#define CALLBACK_WAIT_SLEEP_MS 1
#define CALLBACK_WAIT_MAX_RETRIES 8

#define _FILE_OFFSET_BITS 64

#define OPT_RESAMPLE_A       256
#define OPT_RESAMPLE_B       257
#define OPT_RF_FLAC_12BIT    258
#define OPT_AUDIO_4CH_OUT    259
#define OPT_AUDIO_2CH_12_OUT 260
#define OPT_AUDIO_2CH_34_OUT 261
#define OPT_AUDIO_1CH_1_OUT  262
#define OPT_AUDIO_1CH_2_OUT  263
#define OPT_AUDIO_1CH_3_OUT  264
#define OPT_AUDIO_1CH_4_OUT  265
#define OPT_LIST_DEVICES     266
#define OPT_RESAMPLE_QUAL_A  267
#define OPT_RESAMPLE_QUAL_B  268
#define OPT_RESAMPLE_GAIN_A  269
#define OPT_RESAMPLE_GAIN_B  270
#define OPT_8BIT_A           271
#define OPT_8BIT_B           272
#if defined(__GNUC__)
# define UNUSED(x) x __attribute__((unused))
#else
# define UNUSED(x) x
#endif

/* CLI capture context - extends capture_handler with ringbuffers */
typedef struct {
	capture_handler_ctx_t handler;    /* Shared capture handler context */
	ringbuffer_t rb;                  /* RF ringbuffer (handler.rb_rf points here) */
	ringbuffer_t rb_audio;            /* Audio ringbuffer (handler.rb_audio points here) */
	atomic_uint_fast32_t rb_wait_count;
	atomic_uint_fast32_t rb_drop_count;
	atomic_uint_fast32_t rb_audio_drop_count;
} cli_capture_ctx_t;


typedef struct {
	ringbuffer_t rb;
	FILE *f;
#if LIBSOXR_ENABLED == 1
	conv_16to32_t conv_func;
	double init_scale;
	double resample_rate;
	uint32_t resample_qual;
	float resample_gain;
	bool reduce_8bit;
#endif
#if LIBFLAC_ENABLED == 1
	uint32_t flac_level;
	bool flac_verify;
	uint32_t flac_threads;
	uint8_t flac_bits;
#endif
} filewriter_ctx_t;

typedef struct {
	ringbuffer_t *rb;
	FILE *f_4ch;
	FILE *f_2ch[2];
	FILE *f_1ch[4];
	uint64_t total_bytes;
	bool non_4ch;
} audiowriter_ctx_t;


static int do_exit;
static int new_line = 1;
static hsdaoh_dev_t *hs_dev = NULL;
static sc_handle_t *sc_dev = NULL;
static conv_16to32_t conv_16to32 = NULL;
static conv_16to32_t conv_16to8to32 = NULL;
static conv_16to32_t conv_16to12to32 = NULL;
static conv_16to8_t conv_16to8 = NULL;
#if defined(_MSC_VER) && !defined(__clang__)
#define MISRC_THREAD_LOCAL __declspec(thread)
#else
#define MISRC_THREAD_LOCAL _Thread_local
#endif
static MISRC_THREAD_LOCAL bool s_capture_callback_priority_set = false;

static struct option getopt_long_options[] =
{
  {"device",               required_argument, 0, 'd'},
  {"devices",              no_argument,       0, OPT_LIST_DEVICES},
  {"device-list",          no_argument,       0, OPT_LIST_DEVICES},
  {"count",                required_argument, 0, 'n'},
  {"time",                 required_argument, 0, 't'},
  {"overwrite",            no_argument,       0, 'w'},
  {"rf-adc-a",             required_argument, 0, 'a'},
  {"rf-adc-b",             required_argument, 0, 'b'},
  {"aux",                  required_argument, 0, 'x'},
  {"raw",                  required_argument, 0, 'r'},
  {"pad",                  no_argument,       0, 'p'},
  {"level",                no_argument,       0, 'L'},
  {"suppress-clip-rf-a",   no_argument,       0, 'A'},
  {"suppress-clip-rf-b",   no_argument,       0, 'B'},
#if LIBSOXR_ENABLED == 1
  {"8bit-a",               no_argument,       0, OPT_8BIT_A},
  {"8bit-b",               no_argument,       0, OPT_8BIT_B},
  {"resample-rf-a",        required_argument, 0, OPT_RESAMPLE_A},
  {"resample-rf-b",        required_argument, 0, OPT_RESAMPLE_B},
  {"resample-rf-quality-a",required_argument, 0, OPT_RESAMPLE_QUAL_A},
  {"resample-rf-quality-b",required_argument, 0, OPT_RESAMPLE_QUAL_B},
  {"resample-rf-gain-a",   required_argument, 0, OPT_RESAMPLE_GAIN_A},
  {"resample-rf-gain-b",   required_argument, 0, OPT_RESAMPLE_GAIN_B},
#endif
#if LIBFLAC_ENABLED == 1
  {"rf-flac",              no_argument,       0, 'f'},
  {"rf-flac-12bit",        no_argument,       0, OPT_RF_FLAC_12BIT},
  {"rf-flac-level",        required_argument, 0, 'l'},
  {"rf-flac-verification", no_argument,       0, 'v'},
#if defined(FLAC_API_VERSION_CURRENT) && FLAC_API_VERSION_CURRENT >= 14
  {"rf-flac-threads",      required_argument, 0, 'c'},
#endif
#endif
  {"audio-4ch",            required_argument, 0, OPT_AUDIO_4CH_OUT},
  {"audio-2ch-12",         required_argument, 0, OPT_AUDIO_2CH_12_OUT},
  {"audio-2ch-34",         required_argument, 0, OPT_AUDIO_2CH_34_OUT},
  {"audio-1ch-1",          required_argument, 0, OPT_AUDIO_1CH_1_OUT},
  {"audio-1ch-2",          required_argument, 0, OPT_AUDIO_1CH_2_OUT},
  {"audio-1ch-3",          required_argument, 0, OPT_AUDIO_1CH_3_OUT},
  {"audio-1ch-4",          required_argument, 0, OPT_AUDIO_1CH_4_OUT},
  {0, 0, 0, 0}
};

static char* yesno[] = {"no", "yes"};

static char* usage_options[][2] =
{
  { "device index (default: 0)", "[device index]" },
  { "list available devices", NULL },
  { "list available devices (alias for --devices)", NULL },
  { "number of samples to read (default: 0, infinite)", "[samples]" },
  { "time to capture (seconds, m:s or h:m:s; -n takes priority, assumes 40msps)", "[time]" },
  { "overwrite any files without asking", NULL },
  { "ADC A output file (use '-' to write on stdout)", "[filename]" },
  { "ADC B output file (use '-' to write on stdout)", "[filename]" },
  { "AUX output file (use '-' to write on stdout)", "[filename]" },
  { "raw data output file (use '-' to write on stdout)", "[filename]" },
  { "pad lower 4 bits of 16 bit output with 0 instead of upper 4", NULL },
  { "display peak level of RF ADCs", NULL },
  { "suppress clipping messages for ADC A (need to specify -a or -r as well)", NULL },
  { "suppress clipping messages for ADC B (need to specify -b or -r as well)", NULL },
#if LIBSOXR_ENABLED == 1
  { "reduce output from 12 bit to 8 bit for ADC A", NULL },
  { "reduce output from 12 bit to 8 bit for ADC B", NULL },
  { "resample ADC A signal to given sample rate (in kHz)", "[samplerate]" },
  { "resample ADC B signal to given sample rate (in kHz)", "[samplerate]" },
  { "resample ADC A quality (0=quick ... 4=very high quality, default: 3)", "[quality]" },
  { "resample ADC B quality (0=quick ... 4=very high quality, default: 3)", "[quality]" },
  { "apply gain during resampling of ADC A (in dB)", "[gain]" },
  { "apply gain during resampling of ADC B (in dB)", "[gain]" },
#endif
#if LIBFLAC_ENABLED == 1
  { "compress RF ADC output as FLAC", NULL },
  { "set RF FLAC sample width to 12 instead of 16 bit", NULL },
  { "set RF flac compression level (0-8, default: 1)", "[level]" },
  { "enable verification of RF flac encoder output", NULL },
#if defined(FLAC_API_VERSION_CURRENT) && FLAC_API_VERSION_CURRENT >= 14
  { "number of RF flac encoding threads per file (default: auto)", "[threads]" },
#endif
#endif
  { "4 channel audio output (use '-' to write on stdout)", "[filename]" },
  { "stereo audio output of input 1/2 (use '-' to write on stdout)", "[filename]" },
  { "stereo audio output of input 3/4 (use '-' to write on stdout)", "[filename]" },
  { "mono audio output of input 1 (use '-' to write on stdout)", "[filename]" },
  { "mono audio output of input 2 (use '-' to write on stdout)", "[filename]" },
  { "mono audio output of input 3 (use '-' to write on stdout)", "[filename]" },
  { "mono audio output of input 4 (use '-' to write on stdout)", "[filename]" },
  { 0, 0 }
};

void create_getopt_string(char *getopt_string)
{
	char* s = getopt_string;
	int i = 0;
	while (1) {
		if (getopt_long_options[i].name == 0) break;
		if(getopt_long_options[i].val < 256) {
			*s = getopt_long_options[i].val;
			s++;
			if(getopt_long_options[i].has_arg != no_argument) {
				*s = ':';
				s++;
			}
		}
		i++;
	}
	*s = 0;
}

void misrc_capture_print_usage(void)
{
	fprintf(stderr,
		"A simple program to capture from MISRC using hsdaoh\n\n"
		"Usage:\n"
	);
	int i = 0;
	while (1) {
		if (getopt_long_options[i].name == 0) break;
		if (getopt_long_options[i].val < 256) {
			fprintf(stderr," -%c, --%s", getopt_long_options[i].val, getopt_long_options[i].name);
		} else {
			fprintf(stderr," --%s", getopt_long_options[i].name);
		}
		if (getopt_long_options[i].has_arg == no_argument) {
			fprintf(stderr,":\n");
		}
		else {
			fprintf(stderr," %s:\n", usage_options[i][1]);
		}
		fprintf(stderr,"         %s\n", usage_options[i][0]);
		i++;
	}
}

void usage(void)
{
	misrc_capture_print_usage();
	exit(1);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		if (hs_dev) { hsdaoh_close(hs_dev); hs_dev = NULL; }
		if (sc_dev) { sc_stop_capture(sc_dev); sc_dev = NULL; }
		return true;
	}
	return FALSE;
}
#else
static void sighandler(int UNUSED(signum))
{
	signal(SIGPIPE, SIG_IGN);
	fprintf(stderr, "Signal caught, exiting!\n");
	do_exit = 1;
	if (hs_dev) { hsdaoh_close(hs_dev); hs_dev = NULL; }
	if (sc_dev) { sc_stop_capture(sc_dev); sc_dev = NULL; }
}
#endif

static void print_capture_message(void UNUSED(*ctx), enum hsdaoh_msg_level UNUSED(level), const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	new_line = 1;
}

/* CLI sync progress callback - extends default with simple_capture warning */
static void cli_sync_progress_cb(void *user_ctx, unsigned int non_sync_cnt)
{
	(void)user_ctx;
	capture_handler_default_progress(NULL, non_sync_cnt);
	if (non_sync_cnt == 500 && sc_dev) {
		print_capture_message(NULL, HSDAOH_ERROR, "Verify that your device does not modify the video data!\n");
	}
}

/* CLI sync event callback - handles sync state changes with CLI-specific messages */
static void cli_sync_event_cb(void *user_ctx, frame_sync_result_t result,
                               const metadata_t *meta, bool was_synced)
{
	cli_capture_ctx_t *ctx = (cli_capture_ctx_t *)user_ctx;

	switch (result) {
		case FRAME_SYNC_LOST:
			if (was_synced)
				print_capture_message(NULL, HSDAOH_ERROR, "Lost sync to HDMI input stream\n");
			break;
		case FRAME_SYNC_DUPLICATE:
			break;
		case FRAME_SYNC_MISSED:
			print_capture_message(NULL, HSDAOH_ERROR, "Missed at least one frame, fcnt %d, expected %d!\n",
			                      meta->framecounter, ((ctx->handler.frame_state.sync.last_frame_cnt) & 0xffff));
			break;
		case FRAME_SYNC_ACQUIRED:
			print_capture_message(NULL, HSDAOH_INFO, "Syncronized to HDMI input stream\n MISRC uses CRC: %s\n MISRC uses stream ids: %s\n",
			                      yesno[((meta->crc_config == CRC_NONE) ? 0 : 1)],
			                      yesno[((meta->flags & FLAG_STREAM_ID_PRESENT) ? 1 : 0)]);
			if (atomic_load(&ctx->handler.capture_audio)) {
				if ((meta->flags & FLAG_STREAM_ID_PRESENT)) {
					print_capture_message(NULL, HSDAOH_INFO, "Wait for RF and audio syncronisation...\n");
				} else {
					print_capture_message(NULL, HSDAOH_CRITICAL, "MISRC does not transmit audio, cannot capture audio!\n");
					do_exit = 1;
				}
			}
			break;
		case FRAME_SYNC_OK:
			break;
	}
}

/* CLI audio sync callback - notifies when audio is synced */
static void cli_audio_sync_cb(void *user_ctx, bool synced)
{
	(void)user_ctx;
	if (synced) {
		print_capture_message(NULL, HSDAOH_INFO, "Audio and RF now in sync\n");
	}
}

static void hsdaoh_callback(hsdaoh_data_info_t *data_info)
{
	if (!data_info)
		return;
	cli_capture_ctx_t *ctx = data_info->ctx;
	if (do_exit || !ctx)
		return;

	if (!s_capture_callback_priority_set) {
		/* Callback thread is the capture-ingest hot path. */
		thrd_set_priority(THRD_PRIORITY_CRITICAL);
		s_capture_callback_priority_set = true;
	}

	capture_handler_ctx_t *handler = &ctx->handler;

	metadata_t meta;
	hsdaoh_extract_metadata(data_info->buf, &meta, data_info->width);

	bool was_synced = handler->frame_state.sync.stream_synced;
	if (!was_synced && handler->progress_cb)
		handler->progress_cb(handler->user_ctx, handler->frame_state.sync.non_sync_cnt);

	/* Process frame */
	frame_process_result_t result = frame_process(&handler->frame_state,
	                                               data_info->buf,
	                                               data_info->width,
	                                               data_info->height,
	                                               &meta, 4);

	/* Handle sync events using shared module */
	if (!capture_handler_process_sync_event(handler, result.sync_result, &meta, was_synced))
		return;

	/* Handle errors */
	if (result.error_count > 0 && result.report_errors) {
		print_capture_message(NULL, HSDAOH_ERROR, "%d frame errors, %d frames since last error\n",
		                      result.error_count, handler->frame_state.frames_since_error);
		return;
	}

	if (!result.valid)
		return;

	/* Allocate ringbuffer space */
	uint8_t *buf_out = NULL;
	uint8_t *buf_out_audio = NULL;
	bool capture_rf = atomic_load(&handler->capture_rf);
	bool capture_audio = atomic_load(&handler->capture_audio);
	bool capture_audio_this_frame = capture_audio && handler->audio_sync_stage2;

	if (capture_rf && result.stream0_bytes > 0) {
		unsigned int tries = 0;
		while ((buf_out = rb_write_ptr(&ctx->rb, result.stream0_bytes)) == NULL) {
			if (do_exit) return;
			atomic_fetch_add(&ctx->rb_wait_count, 1);
			tries++;
			if (tries >= CALLBACK_WAIT_MAX_RETRIES) {
				uint32_t drops = atomic_fetch_add(&ctx->rb_drop_count, 1) + 1;
				if (drops <= 5) {
					print_capture_message(NULL, HSDAOH_WARNING, "Dropped frame due to ringbuffer backpressure (RF)\n");
				}
				return;
			}
			sleep_ms(CALLBACK_WAIT_SLEEP_MS);
		}
	}

	if (capture_audio_this_frame && result.stream1_bytes > 0) {
		unsigned int tries = 0;
		while ((buf_out_audio = rb_write_ptr(&ctx->rb_audio, result.stream1_bytes)) == NULL) {
			if (do_exit) return;
			atomic_fetch_add(&ctx->rb_wait_count, 1);
			tries++;
			if (tries >= CALLBACK_WAIT_MAX_RETRIES) {
				uint32_t drops = atomic_fetch_add(&ctx->rb_audio_drop_count, 1) + 1;
				if (drops <= 5) {
					print_capture_message(NULL, HSDAOH_WARNING, "Dropped frame due to ringbuffer backpressure (audio)\n");
				}
				buf_out_audio = NULL;
				break;
			}
			sleep_ms(CALLBACK_WAIT_SLEEP_MS);
		}
	}

	/* Copy payloads with audio sync filtering using shared module */
	frame_copy_payloads_cb(data_info->buf, data_info->width, data_info->height,
	                        &meta, buf_out, buf_out_audio,
	                        capture_handler_audio_filter, handler);

	/* Commit to ringbuffers */
	if (capture_rf && buf_out)
		rb_write_finished(&ctx->rb, result.stream0_bytes);
	if (capture_audio_this_frame && buf_out_audio)
		rb_write_finished(&ctx->rb_audio, result.stream1_bytes);
}

int audio_file_writer(void *ctx)
{
	audiowriter_ctx_t *audio_ctx = ctx;
	size_t len = BUFFER_AUDIO_READ_SIZE;
	void *buf;
	wave_header_t h;
	bool convert_1ch = false;
	bool convert_2ch = false;
	uint8_t* buffer_1ch[4];
	uint8_t* buffer_2ch[2];
	thrd_set_priority(THRD_PRIORITY_CRITICAL);
	memset(&h,0,sizeof(wave_header_t));
	audio_ctx->total_bytes = 0;
	if (audio_ctx->f_4ch != NULL && audio_ctx->f_4ch != stdout) fwrite(&h, 1, sizeof(wave_header_t), audio_ctx->f_4ch);
	for (int i=0; i<2; i++) {
		if (audio_ctx->f_2ch[i] != NULL) {
			if (audio_ctx->f_2ch[i] != stdout) fwrite(&h, 1, sizeof(wave_header_t), audio_ctx->f_2ch[i]);
			convert_2ch = true;
		}
	}
	for (int i=0; i<4; i++) {
		if (audio_ctx->f_1ch[i] != NULL) {
			if (audio_ctx->f_1ch[i] != stdout) fwrite(&h, 1, sizeof(wave_header_t), audio_ctx->f_1ch[i]);
			convert_1ch = true;
		}
	}
	if (convert_1ch) {
		if ((buffer_1ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE)) == NULL) {
			do_exit = 1;
			return -1;
		}
		for (int i=1; i<4; i++) buffer_1ch[i] = buffer_1ch[0] + (BUFFER_AUDIO_READ_SIZE/4)*i;
	}
	if (convert_2ch) {
		if ((buffer_2ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE)) == NULL) {
			do_exit = 1;
			return -1;
		}
		buffer_2ch[1] = buffer_2ch[0] + (BUFFER_AUDIO_READ_SIZE/2);
	}
	while(true) {
		while(((buf = rb_read_ptr(audio_ctx->rb, len)) == NULL) && !do_exit) {
			thrd_sleep(&(struct timespec){.tv_nsec=10000000}, NULL);
		}
		if (do_exit) {
			len = audio_ctx->rb->tail - audio_ctx->rb->head;
			if (len == 0) break;
			buf = rb_read_ptr(audio_ctx->rb, len);
		}
		if (audio_ctx->f_4ch != NULL) fwrite(buf, 1, len, audio_ctx->f_4ch);
		if (convert_1ch) extract_audio_1ch_C(buf, len, buffer_1ch[0], buffer_1ch[1], buffer_1ch[2], buffer_1ch[3]);
		if (convert_2ch) extract_audio_2ch_C(buf, len, (uint16_t*)buffer_2ch[0], (uint16_t*)buffer_2ch[1]);
		rb_read_finished(audio_ctx->rb, len);
		for (int i=0; i<2; i++) if (audio_ctx->f_2ch[i] != NULL) fwrite(buffer_2ch[i], 1, len/2, audio_ctx->f_2ch[i]);
		for (int i=0; i<4; i++) if (audio_ctx->f_1ch[i] != NULL) fwrite(buffer_1ch[i], 1, len/4, audio_ctx->f_1ch[i]);
		audio_ctx->total_bytes += len;
	}
	if (audio_ctx->f_4ch != NULL && audio_ctx->f_4ch != stdout) {
		fseek(audio_ctx->f_4ch, 0, SEEK_SET);
		create_wave_header(&h, audio_ctx->total_bytes/12, 78125, 4, 24);
		fwrite(&h, 1, sizeof(wave_header_t), audio_ctx->f_4ch);
		fclose(audio_ctx->f_4ch);
	}
	for (int i=0; i<2; i++) {
		if (audio_ctx->f_2ch[i] != NULL && audio_ctx->f_2ch[i] != stdout) {
			fseek(audio_ctx->f_2ch[i], 0, SEEK_SET);
			create_wave_header(&h, audio_ctx->total_bytes/12, 78125, 2, 24);
			fwrite(&h, 1, sizeof(wave_header_t), audio_ctx->f_2ch[i]);
			fclose(audio_ctx->f_2ch[i]);
		}
	}
	for (int i=0; i<4; i++) {
		if (audio_ctx->f_1ch[i] != NULL && audio_ctx->f_1ch[i] != stdout) {
			fseek(audio_ctx->f_1ch[i], 0, SEEK_SET);
			create_wave_header(&h, audio_ctx->total_bytes/12, 78125, 1, 24);
			fwrite(&h, 1, sizeof(wave_header_t), audio_ctx->f_1ch[i]);
			fclose(audio_ctx->f_1ch[i]);
		}
	}
	if (convert_1ch) aligned_free(buffer_1ch[0]);
	if (convert_2ch) aligned_free(buffer_2ch[0]);
	return 0;
}


int raw_file_writer(void *ctx)
{
	filewriter_ctx_t *file_ctx = ctx;
	size_t len = BUFFER_READ_SIZE;
	void *buf;
	thrd_set_priority(THRD_PRIORITY_CRITICAL);
#if LIBSOXR_ENABLED == 1
	/* setup resampling */
	uint8_t *resample_buffer;
	uint8_t *resample_buffer_b;
	soxr_t resampler = NULL;
	soxr_error_t soxr_err;
	if (file_ctx->resample_rate!=0.0) {
		soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT16_S, SOXR_INT16_S);
		soxr_quality_spec_t qual_spec = soxr_quality_spec(file_ctx->resample_qual, 0);
		io_spec.scale = file_ctx->init_scale;
		io_spec.scale *= pow(10.0,file_ctx->resample_gain/20.0);
		resample_buffer = aligned_alloc(32, BUFFER_READ_SIZE);
		resample_buffer_b = aligned_alloc(32, BUFFER_READ_SIZE);
		if (!resample_buffer || !resample_buffer_b) {
			fprintf(stderr, "ERROR: failed allocating resampling buffer\n");
			do_exit = 1;
			return 0;
		}
		resampler = soxr_create(40000.0, file_ctx->resample_rate, 1, &soxr_err, &io_spec, &qual_spec, NULL);
		if (!resampler || soxr_err!=0) {
			fprintf(stderr, "ERROR: failed allocating resampling context: %s\n", soxr_err);
			do_exit = 1;
			return 0;
		}
	}
#endif
	while(true) {
		while(((buf = rb_read_ptr(&file_ctx->rb, len)) == NULL) && !do_exit) {
			//ms_sleep(10);
			thrd_sleep(&(struct timespec){.tv_nsec=10000000}, NULL);
		}
		if (do_exit) {
			len = file_ctx->rb.tail - file_ctx->rb.head;
			if (len == 0) break;
			buf = rb_read_ptr(&file_ctx->rb, len);
		}
#if LIBSOXR_ENABLED == 1
		if (file_ctx->resample_rate!=0) {
			size_t out_len;
			soxr_err = soxr_process(resampler, &buf, len>>1, &len, &resample_buffer, len>>1, &out_len);
			len<<=1;
			if (soxr_err != 0) {
				fprintf(stderr, "Error while converting: %s\n", soxr_err);
				do_exit = 1;
				return 0;
			}
			if (file_ctx->reduce_8bit) {
				conv_16to8((int16_t*)resample_buffer, (int8_t*)resample_buffer_b, out_len);
				fwrite(resample_buffer_b, 1, out_len, file_ctx->f);
			}
			else {
				fwrite(resample_buffer, 1, out_len<<1, file_ctx->f);
			}
		} else {
			fwrite(buf, 1, len, file_ctx->f);
		}
#else
		fwrite(buf, 1, len, file_ctx->f);
#endif
		rb_read_finished(&file_ctx->rb, len);
	}
	if (file_ctx->f != stdout) fclose(file_ctx->f);
#if LIBSOXR_ENABLED == 1
	if (file_ctx->resample_rate!=0) {
		aligned_free(resample_buffer);
		aligned_free(resample_buffer_b);
		soxr_delete(resampler);
	}
#endif
	return 0;
}

#if LIBFLAC_ENABLED == 1
// Error callback for CLI FLAC writer
static void cli_flac_error_callback(void *user_data, flac_writer_error_t error, const char *message) {
	(void)user_data;
	(void)error;
	fprintf(stderr, "FLAC ERROR: %s\n", message);
}

int flac_file_writer(void *ctx)
{
	filewriter_ctx_t *file_ctx = ctx;
	size_t len = BUFFER_READ_SIZE;
	void *buf;
	uint32_t srate = 40000;
	int result;
	thrd_set_priority(THRD_PRIORITY_CRITICAL);
#if LIBSOXR_ENABLED == 1
	uint8_t *resample_buffer = NULL;
	uint8_t *resample_buffer_b = NULL;
	soxr_t resampler = NULL;
	soxr_error_t soxr_err;
	if (file_ctx->resample_rate!=0.0) {
		srate = (uint32_t)(file_ctx->resample_rate);
		resample_buffer = aligned_alloc(32, BUFFER_READ_SIZE);
		resample_buffer_b = aligned_alloc(32, BUFFER_READ_SIZE);
		soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT32_S, SOXR_INT16_S);
		soxr_quality_spec_t qual_spec = soxr_quality_spec(file_ctx->resample_qual, 0);
		io_spec.scale = file_ctx->init_scale;
		io_spec.scale *= pow(10.0,file_ctx->resample_gain/20.0);
		if (!resample_buffer || !resample_buffer_b) {
			fprintf(stderr, "ERROR: failed allocating resampling buffer\n");
			do_exit = 1;
			return 0;
		}
		resampler = soxr_create(40000.0, file_ctx->resample_rate, 1, &soxr_err, &io_spec, &qual_spec, NULL);
		if (!resampler || soxr_err!=0) {
			fprintf(stderr, "ERROR: failed allocating resampling context: %s\n", soxr_err);
			do_exit = 1;
			return 0;
		}
	}
#endif

	// Configure FLAC writer using shared library
	flac_writer_config_t config = flac_writer_default_config();
	config.sample_rate = srate;
	config.bits_per_sample = file_ctx->flac_bits;
	config.compression_level = file_ctx->flac_level;
	config.verify = file_ctx->flac_verify;
	config.num_threads = file_ctx->flac_threads;
	config.enable_seektable = true;
	config.error_cb = cli_flac_error_callback;
	config.callback_user_data = file_ctx;

	flac_writer_t *writer = flac_writer_create_file(file_ctx->f, &config);
	if (!writer) {
		fprintf(stderr, "ERROR: failed to create FLAC writer\n");
		do_exit = 1;
		return 0;
	}

	while(true) {
		while(((buf = rb_read_ptr(&file_ctx->rb, len)) == NULL) && !do_exit) {
			thrd_sleep(&(struct timespec){.tv_nsec=10000000}, NULL);
		}
		if (do_exit) {
			len = file_ctx->rb.tail - file_ctx->rb.head;
			if (len == 0) break;
			buf = rb_read_ptr(&file_ctx->rb, len);
		}
#if LIBSOXR_ENABLED == 1
		if (file_ctx->resample_rate!=0) {
			size_t out_len;
			soxr_err = soxr_process(resampler, &buf, len>>2, &len, &resample_buffer, len>>1, &out_len);
			len<<=2;
			if (soxr_err != 0) {
				fprintf(stderr, "Error while converting: %s\n", soxr_err);
				do_exit = 1;
				flac_writer_abort(writer);
				return 0;
			}
			file_ctx->conv_func((int16_t*)resample_buffer, (int32_t*)resample_buffer_b, out_len);
			result = flac_writer_process(writer, (const int32_t*)resample_buffer_b, out_len);
		} else {
			result = flac_writer_process(writer, (const int32_t*)buf, len>>2);
		}
#else
		result = flac_writer_process(writer, (const int32_t*)buf, len>>2);
#endif
		if (result < 0) {
			fprintf(stderr, "ERROR: (%p) FLAC encoder could not process data\n", (void*)file_ctx->f);
			new_line = 1;
		}
		rb_read_finished(&file_ctx->rb, len);
	}

	flac_writer_error_t err = flac_writer_finish(writer);
	if (err != FLAC_WRITER_OK) {
		fprintf(stderr, "ERROR: FLAC encoder did not finish correctly\n");
		new_line = 1;
	}

#if LIBSOXR_ENABLED == 1
	if (file_ctx->resample_rate!=0) {
		aligned_free(resample_buffer);
		aligned_free(resample_buffer_b);
		soxr_delete(resampler);
	}
#endif
	return 0;
}
#endif


static bool str_starts_with(const char *restrict prefixA, const char *restrict prefixB, size_t *prefixLen, const char *restrict string)
{
	while(*prefixA) {
		if(prefixLen) (*prefixLen)++;
		if(*prefixA++ != *string++)
			return false;
	}
	if (prefixB) while(*prefixB) {
		if(prefixLen) (*prefixLen)++;
		if(*prefixB++ != *string++)
			return false;
	}
	return true;
}

void list_devices() {
	misrc_device_list_t devices;
	misrc_device_list_init(&devices);
	misrc_device_enumerate(&devices, true, true);
	misrc_device_list_print(&devices);
	misrc_device_list_free(&devices);
	exit(1);
}

void print_level(char ch, uint16_t level) {
	float db_level = 20.0f * log10((float)level / 2048.0f);
	// the idea is a non-linear scale similar to vu meters
	uint8_t count = (uint8_t) lroundf( 70.0f/(1.0f + exp(0.163f*(-15.0f - db_level))));
	char full[] = "################################################################";
	char none[] = "                                                                ";
	full[count] = 0;
	none[64-count] = 0;
	fprintf(stderr, "\33[2K\r RF %c [%s%s] %5.1f dB\n", ch, full, none, db_level);
}

int main(int argc, char **argv)
{
    if (!file_init_utf8_argv(&argc, &argv)) return 1;
//set pipe mode to binary in windows
#if defined(_WIN32) || defined(_WIN64)
	_setmode(_fileno(stdout), O_BINARY);
	_setmode(_fileno(stdin), O_BINARY);
#else
	struct sigaction sigact;
#endif

	int r, opt, pad=0, plevel=0, dev_index=0, out_size = 2;
#if LIBFLAC_ENABLED == 1
	int flac_level = 1;
	bool flac_verify = false;
	bool flac_12bit = false;
	uint32_t flac_threads = 0;
#endif
#if LIBSOXR_ENABLED == 1
	double resample_rate[] = {0.0,0.0};
	uint32_t resample_qual[] = {3,3};
	float resample_gain[] = {.0f,.0f};
	bool reduce_8bit[] = {false, false};
#endif
	thrd_start_t output_thread_func = (thrd_start_t)raw_file_writer;
	cli_capture_ctx_t cap_ctx;
	memset(&cap_ctx, 0, sizeof(cap_ctx));
	atomic_init(&cap_ctx.rb_wait_count, 0);
	atomic_init(&cap_ctx.rb_drop_count, 0);
	atomic_init(&cap_ctx.rb_audio_drop_count, 0);
	capture_handler_init(&cap_ctx.handler);
	/* Set up CLI-specific callbacks */
	cap_ctx.handler.progress_cb = cli_sync_progress_cb;
	cap_ctx.handler.sync_event_cb = cli_sync_event_cb;
	cap_ctx.handler.audio_sync_cb = cli_audio_sync_cb;
	cap_ctx.handler.user_ctx = &cap_ctx;

	// getopt string
	char getopt_string[256];

	// device names
	char dev_manufact[256];
	char dev_product[256];
	char dev_serial[256];

	char *sc_dev_name = NULL;

	//output threads
	// out 1, 2
	thrd_t thread_out[2] = { 0, 0 };
	thrd_t thread_audio = 0;
	filewriter_ctx_t thread_out_ctx[2];
	audiowriter_ctx_t thread_audio_ctx;
	char outbuffer_name[] = "outX_ringbuffer";

	//file adress
	// out 1, 2
	char *output_names[2] = { NULL, NULL };
	char *output_name_aux = NULL;
	char *output_name_raw = NULL;

	char *output_name_4ch_audio = NULL;
	char *output_names_2ch_audio[2] = { NULL, NULL };
	char *output_names_1ch_audio[4] = { NULL, NULL, NULL, NULL };

	//show clipping messages
	bool suppress_a_clipping = false;
	bool suppress_b_clipping = false;

	//overwrite option
	bool overwrite_files = false;

	//number of samples to take
	uint64_t total_samples_before_exit = 0;

	//output files
	FILE *output_aux = NULL;
	FILE *output_raw = NULL;

	//buffer
	uint8_t  *buf_aux = aligned_alloc(16,sizeof(uint8_t) *BUFFER_READ_SIZE);

	uint64_t total_samples = 0;

	//clipping state
	size_t clip[2] = {0, 0};

	//peak level
	uint16_t peak_level[2] = {0, 0};

	// conversion function
	conv_function_t conv_function;

	memset(&thread_audio_ctx, 0, sizeof(audiowriter_ctx_t));

	fprintf(stderr,
		"MISRC capture " MIRSC_TOOLS_VERSION"\n"
		MIRSC_TOOLS_COPYRIGHT "\n\n"
	);

	create_getopt_string(getopt_string);

	int index_ptr;
	size_t str_cnt = 0;
	while ((opt = getopt_long(argc, argv, getopt_string, getopt_long_options, &index_ptr)) != -1) {
		switch (opt) {
		case 'd':
			if (str_starts_with(sc_get_impl_name_short(), "://", &str_cnt, optarg)) {
				sc_dev_name = strdup(&(optarg[str_cnt]));
			}
			else
				dev_index = (uint32_t)atoi(optarg);
			break;
		case 'a':
			output_names[0] = optarg;
			break;
		case 'b':
			output_names[1] = optarg;
			break;
#if LIBFLAC_ENABLED == 1
#if defined(FLAC_API_VERSION_CURRENT) && FLAC_API_VERSION_CURRENT >= 14
		case 'c':
			flac_threads = (uint32_t)atoi(optarg);
			break;
#endif
		case OPT_RF_FLAC_12BIT:
			flac_12bit = true;
			break;
		case 'f':
			output_thread_func = (thrd_start_t)flac_file_writer;
			out_size = 4;
			break;
		case 'l':
			flac_level = (uint32_t)atoi(optarg);
			break;
		case 'v':
			flac_verify = true;
			break;
#endif
		case 'x':
			output_name_aux = optarg;
			break;
		case 'r':
			output_name_raw = optarg;
			break;
		case 'p':
			pad = 1;
			break;
		case 'w':
			overwrite_files = true;
			break;
		case 'n':
			total_samples_before_exit = (uint64_t)strtoull(optarg,NULL,0);
			break;
		case 't':
			if(total_samples_before_exit == 0) {
				char *tp;
				tp = strtok(optarg, ":");
				while (tp != NULL) {
					total_samples_before_exit *= 60;
					total_samples_before_exit += (uint64_t)strtoull(tp,NULL,10);
					tp = strtok(NULL, ":");
				}
				total_samples_before_exit *= 40000000;
			}
			break;
		case 'A':
			suppress_a_clipping = true;
			break;
		case 'B':
			suppress_b_clipping = true;
			break;
		case 'L':
			plevel = 1;
			break;
#if LIBSOXR_ENABLED == 1
		case OPT_RESAMPLE_A:
			resample_rate[0] = atof(optarg);
			break;
		case OPT_RESAMPLE_B:
			resample_rate[1] = atof(optarg);
			break;
		case OPT_RESAMPLE_QUAL_A:
			resample_qual[0] = (uint32_t)atoi(optarg);
			break;
		case OPT_RESAMPLE_QUAL_B:
			resample_qual[1] = (uint32_t)atoi(optarg);
			break;
		case OPT_RESAMPLE_GAIN_A:
			resample_gain[0] = atof(optarg);
			break;
		case OPT_RESAMPLE_GAIN_B:
			resample_gain[1] = atof(optarg);
			break;
		case OPT_8BIT_A:
			reduce_8bit[0] = true;
			break;
		case OPT_8BIT_B:
			reduce_8bit[1] = true;
			break;
#endif
		case OPT_AUDIO_4CH_OUT:
			output_name_4ch_audio = optarg;
			break;
		case OPT_AUDIO_2CH_12_OUT:
			output_names_2ch_audio[0] = optarg;
			break;
		case OPT_AUDIO_2CH_34_OUT:
			output_names_2ch_audio[1] = optarg;
			break;
		case OPT_AUDIO_1CH_1_OUT:
			output_names_1ch_audio[0] = optarg;
			break;
		case OPT_AUDIO_1CH_2_OUT:
			output_names_1ch_audio[1] = optarg;
			break;
		case OPT_AUDIO_1CH_3_OUT:
			output_names_1ch_audio[2] = optarg;
			break;
		case OPT_AUDIO_1CH_4_OUT:
			output_names_1ch_audio[3] = optarg;
			break;
		case OPT_LIST_DEVICES:
			list_devices();
			break;
		case 'h':
		default:
			usage();
			break;
		}
	}

	if(output_names[0] == NULL && output_names[1] == NULL && output_name_aux == NULL && output_name_raw == NULL && plevel == 0) {
		usage();
	}
	else {
		atomic_store(&cap_ctx.handler.capture_rf, true);
	}

#if LIBSOXR_ENABLED == 1
	for(int i=0; i<2; i++) {
		switch (resample_qual[i]) {
			case 0:
				resample_qual[i] = SOXR_QQ;
				break;
			case 1:
				resample_qual[i] = SOXR_LQ;
				break;
			case 2:
				resample_qual[i] = SOXR_MQ;
				break;
			case 3:
				resample_qual[i] = SOXR_HQ;
				break;
			case 4:
				resample_qual[i] = SOXR_VHQ;
				break;
			default:
				fprintf(stderr, "ERROR: Invalid resampling quality option!\n");
				usage();
		}
	}
	if(resample_rate[0] >= 40000.0 || resample_rate[1] >= 40000.0) {
		fprintf(stderr, "ERROR: Resampling to rates higher than 40 MHz is not supported!\n");
		usage();
	}
	if((resample_rate[0] != 0.0 || resample_rate[1] != 0.0) && out_size == 4) {
		if (flac_12bit) {
			conv_16to12to32 = get_16to12to32_function();
		} else {
			conv_16to32 = get_16to32_function();
		}
	}
	if(reduce_8bit[0] || reduce_8bit[1]) {
		if (out_size == 4) 
			conv_16to8to32 = get_16to8to32_function();
		else 
			conv_16to8 = get_16to8_function();
		if(reduce_8bit[0] && resample_rate[0]==0.0) resample_rate[0] = 40000.0;
		if(reduce_8bit[1] && resample_rate[1]==0.0) resample_rate[1] = 40000.0;
	}
#endif

#if LIBFLAC_ENABLED == 1
	if(flac_12bit && pad == 1) {
		fprintf(stderr, "Warning: You enabled padding the lower 4 bits, but requested 12 bit flac output, this is not possible, will output 16 bit flac.\n");
		flac_12bit = false;
	}
/*#if LIBSOXR_ENABLED == 1
	if(flac_12bit && (resample_rate[0] != 0.0 || resample_rate[1] != 0.0)) {
		fprintf(stderr, "Warning: You use resampling, this cannot be combined with 12 bit flac output, will output 16 bit flac.\n");
		flac_12bit = false;
	}
#endif*/
# if defined(FLAC_API_VERSION_CURRENT) && FLAC_API_VERSION_CURRENT >= 14
	if (flac_threads == 0) {
		int out_cnt = ((output_names[0] == NULL) ? 0 : 1) + ((output_names[1] == NULL) ? 0 : 1);
		if (out_cnt != 0) {
			flac_threads = get_num_cores();
			fprintf(stderr,"Detected %d cores in the system available to the process\n",flac_threads);
			flac_threads = (flac_threads - 2 - out_cnt) / out_cnt;
			if (flac_threads == 0) flac_threads = 1;
			if (flac_threads > 128) flac_threads = 128;
		}
	}
# endif
#endif

	if(suppress_a_clipping) {
		fprintf(stderr, "Suppressing clipping messages from ADC A\n");
	}
	if(suppress_b_clipping) {
		fprintf(stderr, "Suppressing clipping messages from ADC B\n");
	}
	if(total_samples_before_exit > 0) {
		fprintf(stderr, "Capturing %" PRIu64 " samples before exiting\n", total_samples_before_exit);
	}


#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, true );
#endif
	for(int i=0; i<2; i++) {
		if (output_names[i] != NULL) {
			if (file_open_write(&(thread_out_ctx[i].f),output_names[i],overwrite_files,true)) return -ENOENT;
#if LIBSOXR_ENABLED == 1
			thread_out_ctx[i].reduce_8bit = reduce_8bit[i];
			if (out_size == 4) {
				thread_out_ctx[i].init_scale = (reduce_8bit[i]) ? ((pad==1) ? 256.0 : 4096.0) : 65536.0;
			} else {
				thread_out_ctx[i].init_scale = (reduce_8bit[i]) ? ((pad==1) ? 0.00390625 : 0.0625) : 1.0;
			}
#endif
#if LIBFLAC_ENABLED == 1
			thread_out_ctx[i].flac_level = flac_level;
			thread_out_ctx[i].flac_verify = flac_verify;
			thread_out_ctx[i].flac_threads = flac_threads;
#if LIBSOXR_ENABLED == 1
			thread_out_ctx[i].flac_bits = reduce_8bit[i] ? 8 : (flac_12bit ? 12 : 16);
			thread_out_ctx[i].conv_func = reduce_8bit[i] ? conv_16to8to32 : (flac_12bit ? conv_16to12to32 : conv_16to32);
#else
			thread_out_ctx[i].flac_bits = flac_12bit ? 12 : 16;
#endif
#endif
#if LIBSOXR_ENABLED == 1
			thread_out_ctx[i].resample_rate = resample_rate[i];
			thread_out_ctx[i].resample_qual = resample_qual[i];
			thread_out_ctx[i].resample_gain = resample_gain[i];
#endif
			outbuffer_name[3] = (char)(i+48);
			rb_init(&thread_out_ctx[i].rb, outbuffer_name, BUFFER_TOTAL_SIZE);
			r = thrd_create(&thread_out[i], output_thread_func, &thread_out_ctx[i]);
			if (r != thrd_success) {
				fprintf(stderr, "Failed to create thread for output processing\n");
				return -ENOENT;
			}
		}
	}

	if(output_name_4ch_audio != NULL)
	{
		//opening output file audio
		if (file_open_write(&(thread_audio_ctx.f_4ch), output_name_4ch_audio, overwrite_files, true)) return -ENOENT;
		atomic_store(&cap_ctx.handler.capture_audio, true);
	}

	for(int i=0; i<2; i++) {
		if(output_names_2ch_audio[i] != NULL)
		{
			//opening output file audio
			if (file_open_write(&(thread_audio_ctx.f_2ch[i]), output_names_2ch_audio[i], overwrite_files, true)) return -ENOENT;
			atomic_store(&cap_ctx.handler.capture_audio, true);
		}
	}

	for(int i=0; i<4; i++) {
		if(output_names_1ch_audio[i] != NULL)
		{
			//opening output file audio
			if (file_open_write(&(thread_audio_ctx.f_1ch[i]), output_names_1ch_audio[i], overwrite_files, true)) return -ENOENT;
			atomic_store(&cap_ctx.handler.capture_audio, true);
		}
	}

	if(output_name_aux != NULL)
	{
		//opening output file aux
		if (file_open_write(&output_aux, output_name_aux, overwrite_files, true)) return -ENOENT;
	}

	if(output_name_raw != NULL)
	{
		//opening output file raw
		if (file_open_write(&output_raw, output_name_raw, overwrite_files, true)) return -ENOENT;
	}

	if(atomic_load(&cap_ctx.handler.capture_audio)) {
		rb_init(&cap_ctx.rb_audio,"capture_audio_ringbuffer",BUFFER_AUDIO_TOTAL_SIZE);
		cap_ctx.handler.rb_audio = &cap_ctx.rb_audio;
		thread_audio_ctx.rb = &cap_ctx.rb_audio;
		r = thrd_create(&thread_audio, &audio_file_writer, &thread_audio_ctx);
		if (r != thrd_success) {
			fprintf(stderr, "Failed to create thread for output processing\n");
			return -ENOENT;
		}
	}

	conv_function = get_conv_function(0, pad, (out_size==2) ? 0 : 1, plevel, output_names[0], output_names[1]);

	rb_init(&cap_ctx.rb,"capture_ringbuffer",BUFFER_TOTAL_SIZE);
	cap_ctx.handler.rb_rf = &cap_ctx.rb;

	if (sc_dev_name) {
		proc_set_priority(PROC_PRIORITY_ABOVE);
		r = sc_start_capture(sc_dev_name, 1920, 1080, SC_CODEC_YUYV, 60, 1, (sc_frame_callback_t)hsdaoh_callback, &cap_ctx, &sc_dev);
		if (r < 0) {
			fprintf(stderr, "Failed to open %s device %s.\n", sc_get_impl_name(), sc_dev_name);
			proc_set_priority(PROC_PRIORITY_NORMAL);
			exit(1);
		}
		fprintf(stderr, "Opened %s device %s.\n", sc_get_impl_name(), sc_dev_name);
	}
	else {
		proc_set_priority(PROC_PRIORITY_ABOVE);

		r = hsdaoh_open(&hs_dev, (uint32_t)dev_index);
		if (r < 0) {
			fprintf(stderr, "Failed to open hsdaoh device #%d.\n", dev_index);
			proc_set_priority(PROC_PRIORITY_NORMAL);
			exit(1);
		}
		hsdaoh_set_msg_callback(hs_dev, &print_capture_message, NULL);
		hsdaoh_raw_callback(hs_dev, true);

		dev_manufact[0] = 0;
		dev_product[0] = 0;
		dev_serial[0] = 0;
		r = hsdaoh_get_usb_strings(hs_dev, dev_manufact, dev_product, dev_serial);
		if (r < 0)
			fprintf(stderr, "Failed to identify hsdaoh device #%d.\n", dev_index);
		else
			fprintf(stderr, "Opened device #%d: %s %s, serial: %s\n", dev_index, dev_manufact, dev_product, dev_serial);

		fprintf(stderr, "Reading samples...\n");
		r = MISRC_HSDAOH_START_STREAM(hs_dev, hsdaoh_callback, &cap_ctx);
		if (r < 0) {
			fprintf(stderr, "Failed to start hsdaoh stream on device #%d.\\n", dev_index);
			proc_set_priority(PROC_PRIORITY_NORMAL);
			hsdaoh_close(hs_dev);
			hs_dev = NULL;
			exit(1);
		}
	}
	/* Main conversion loop is latency-sensitive but not ingest-critical. */
	thrd_set_priority(THRD_PRIORITY_HIGH);


	while (!do_exit) {
		void *buf, *buf_out1 = NULL, *buf_out2 = NULL;
		while((((buf = rb_read_ptr(&cap_ctx.rb, BUFFER_READ_SIZE*4)) == NULL) || 
			  (output_names[0] != NULL && ((buf_out1 = rb_write_ptr(&thread_out_ctx[0].rb, BUFFER_READ_SIZE*out_size)) == NULL)) ||
			  (output_names[1] != NULL && ((buf_out2 = rb_write_ptr(&thread_out_ctx[1].rb, BUFFER_READ_SIZE*out_size)) == NULL))) && 
			  !do_exit)
		{
			sleep_ms(10);
		}
		if (do_exit) break;
		conv_function((uint32_t*)buf, BUFFER_READ_SIZE, clip, buf_aux, buf_out1, buf_out2, peak_level);
		if(output_raw != NULL){fwrite(buf,4,BUFFER_READ_SIZE,output_raw);}
		rb_read_finished(&cap_ctx.rb, BUFFER_READ_SIZE*4);
		if(output_aux != NULL){fwrite(buf_aux,1,BUFFER_READ_SIZE,output_aux);}
		if(output_names[0] != NULL) rb_write_finished(&thread_out_ctx[0].rb, BUFFER_READ_SIZE*out_size);
		if(output_names[1] != NULL) rb_write_finished(&thread_out_ctx[1].rb, BUFFER_READ_SIZE*out_size);

		total_samples += BUFFER_READ_SIZE;

		if(clip[0] > 0 && !suppress_a_clipping)
		{
			fprintf(stderr,"ADC A : %zu samples clipped\n",clip[0]);
			clip[0] = 0;
			new_line = 1;
		}

		if(clip[1] > 0 && !suppress_b_clipping)
		{
			fprintf(stderr,"ADC B : %zu samples clipped\n",clip[1]);
			clip[1] = 0;
			new_line = 1;
		}
		if (total_samples % (BUFFER_READ_SIZE<<(2 - plevel)) == 0) {
			if(new_line) {
				fprintf(stderr,"\n");
				if(plevel) fprintf(stderr,"\n\n");
			}
			new_line = 0;
			if(plevel) {
				fprintf(stderr,"\033[A\033[A\033[A");
				
				print_level('A', peak_level[0]);
				print_level('B', peak_level[1]);
			}
			else {
				fprintf(stderr,"\033[A");
			}
			// \033[A = move cursor up
			// \33[2K = erase line
			
			
			fprintf(stderr,"\33[2K\r Progress: %13" PRIu64 " samples, %2uh %2um %2us\n", total_samples, (uint32_t)(total_samples/(144000000000)), (uint32_t)((total_samples/(2400000000)) % 60), (uint32_t)((total_samples/(40000000)) % 60));
			fflush(stderr);
		}
		if (total_samples >= total_samples_before_exit && total_samples_before_exit != 0) {
			fprintf(stderr, "%" PRIu64 " total samples have been collected, exiting early!\n", total_samples);
			do_exit = true;
		}
	}

	if (do_exit)
		fprintf(stderr, "\nUser cancel, exiting...\n");
	else
		fprintf(stderr, "\nLibrary error %d, exiting...\n", r);

	if (hs_dev) { hsdaoh_close(hs_dev); hs_dev = NULL; }
	if (sc_dev) { sc_stop_capture(sc_dev); sc_dev = NULL; }

////ending of the program

	aligned_free(buf_aux);

	if (output_aux && (output_aux != stdout)) fclose(output_aux);
	if (output_raw && (output_raw != stdout)) fclose(output_raw);

	for(int i=0;i<2;i++) {
		if (thread_out[i]!=0) {
			r = thrd_join(thread_out[i], NULL);
			if (r != thrd_success) fprintf(stderr, "Failed to join thread %d.\n", i);
		}
	}

	if (thread_audio!=0) {
		r = thrd_join(thread_audio, NULL);
		if (r != thrd_success) fprintf(stderr, "Failed to join audio thread.\n");
	}

	fprintf(stderr, "[CAP] Backpressure summary: waits=%u rf_drops=%u audio_drops=%u\n",
	        (unsigned)atomic_load(&cap_ctx.rb_wait_count),
	        (unsigned)atomic_load(&cap_ctx.rb_drop_count),
	        (unsigned)atomic_load(&cap_ctx.rb_audio_drop_count));
	proc_set_priority(PROC_PRIORITY_NORMAL);

	return 0;
}
