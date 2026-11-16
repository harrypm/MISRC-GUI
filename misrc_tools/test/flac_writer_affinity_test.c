/*
 * Exercise the real FLAC writer and inspect the Win32 threads it creates.
 * The test process has no capture, preview, or UI threads.
 */
#include "../common/flac_writer.h"

#include <windows.h>
#include <tlhelp32.h>
#include <process.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <FLAC/stream_decoder.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum {
    SAMPLES_PER_CHUNK = 4096,
    WARMUP_SAMPLES = SAMPLES_PER_CHUNK * 16,
    SAMPLE_COUNT = SAMPLES_PER_CHUNK * 32 + 37,
    MAX_TEST_THREADS = 64,
    RECORDINGS = 2
};

typedef struct {
    DWORD ids[MAX_TEST_THREADS];
    size_t count;
} thread_list_t;

typedef struct {
    const char *cpu_list;
    GROUP_AFFINITY expected_affinity;
    bool affinity_enabled;
    uint32_t num_threads;
    uint32_t sample_offset;
    flac_writer_t *writer;
    FILE *file;
    HANDLE thread;
    HANDLE ready;
    HANDLE start;
    HANDLE resume;
} encoding_test_t;

typedef struct {
    uint32_t position;
    uint32_t sample_offset;
} decoding_test_t;

static thread_list_t snapshot_threads(void) {
    thread_list_t list = {0};
    DWORD process_id = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    CHECK(snapshot != INVALID_HANDLE_VALUE);
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    CHECK(Thread32First(snapshot, &entry));
    do {
        if (entry.th32OwnerProcessID == process_id) {
            CHECK(list.count < MAX_TEST_THREADS);
            list.ids[list.count++] = entry.th32ThreadID;
        }
        entry.dwSize = sizeof(entry);
    } while (Thread32Next(snapshot, &entry));
    CHECK(GetLastError() == ERROR_NO_MORE_FILES);
    CloseHandle(snapshot);
    return list;
}

static bool already_running(const thread_list_t *list, DWORD id) {
    for (size_t i = 0; i < list->count; ++i) {
        if (list->ids[i] == id) return true;
    }
    return false;
}

static GROUP_AFFINITY read_affinity(HANDLE thread) {
    GROUP_AFFINITY affinity;
    CHECK(GetThreadGroupAffinity(thread, &affinity));
    return affinity;
}

static GROUP_AFFINITY read_worker_affinity(DWORD id) {
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    CHECK(thread != NULL);
    GROUP_AFFINITY affinity = read_affinity(thread);
    CloseHandle(thread);
    return affinity;
}

static bool same_affinity(GROUP_AFFINITY a, GROUP_AFFINITY b) {
    return a.Group == b.Group && a.Mask == b.Mask;
}

static int16_t fixture_sample(uint32_t index) {
    uint32_t value = index * 1664525u + 1013904223u;
    return (int16_t)((int32_t)(value >> 16) - 32768);
}

static thread_list_t check_new_workers(const encoding_test_t *test, thread_list_t *baseline) {
    /* Warmup has returned, so worker creation and the affinity handoff are done.
     * The workers remain live until flac_writer_finish(). */
    thread_list_t current = snapshot_threads();
    thread_list_t workers = {0};
    size_t matching = 0;
    for (size_t i = 0; i < current.count; ++i) {
        DWORD id = current.ids[i];
        if (already_running(baseline, id)) continue;
        workers.ids[workers.count++] = id;
        if (same_affinity(read_worker_affinity(id), test->expected_affinity)) ++matching;
    }
    if (workers.count != test->num_threads - 1 || matching != workers.count) {
        fprintf(stderr, "Mask 0x%llx: expected %u libFLAC workers, saw %zu with %zu matching affinity\n",
            (unsigned long long)test->expected_affinity.Mask,
            test->num_threads - 1, workers.count, matching);
        exit(EXIT_FAILURE);
    }
    *baseline = current;
    return workers;
}

static void wait_for(HANDLE event) {
    CHECK(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0);
}

static void encode_samples(encoding_test_t *test, uint32_t start, uint32_t end) {
    int16_t samples[SAMPLES_PER_CHUNK];
    while (start < end) {
        uint32_t count = end - start;
        if (count > SAMPLES_PER_CHUNK) count = SAMPLES_PER_CHUNK;
        for (uint32_t i = 0; i < count; ++i) samples[i] = fixture_sample(test->sample_offset + start + i);
        CHECK(flac_writer_process_int16(test->writer, samples, count) == (int)count);
        start += count;
    }
}

static unsigned int __stdcall encode(void *context) {
    encoding_test_t *test = context;
    CHECK(flac_writer_apply_thread_affinity(test->writer) == FLAC_WRITER_OK);
    CHECK(SetEvent(test->ready));
    wait_for(test->start);
    encode_samples(test, 0, WARMUP_SAMPLES);
    CHECK(SetEvent(test->ready));
    wait_for(test->resume);
    encode_samples(test, WARMUP_SAMPLES, SAMPLE_COUNT);
    CHECK(same_affinity(read_affinity(GetCurrentThread()), test->expected_affinity));
    return 0;
}

static FLAC__StreamDecoderWriteStatus check_decoded_samples(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const samples[], void *context
) {
    (void)decoder;
    decoding_test_t *test = context;
    CHECK(frame->header.channels == 1);
    CHECK(frame->header.bits_per_sample == 16);
    CHECK(frame->header.sample_rate == 40000);
    CHECK(frame->header.blocksize <= SAMPLE_COUNT - test->position);
    for (uint32_t i = 0; i < frame->header.blocksize; ++i) {
        CHECK(samples[0][i] == fixture_sample(test->sample_offset + test->position));
        ++test->position;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void fail_decode(const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus status, void *context
) {
    (void)decoder;
    (void)context;
    fprintf(stderr, "FLAC decoder error: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
    exit(EXIT_FAILURE);
}

static void fail_encode(void *context, flac_writer_error_t error, const char *message) {
    (void)context;
    fprintf(stderr, "FLAC writer error %d: %s\n", (int)error, message);
    exit(EXIT_FAILURE);
}

static FILE *temporary_flac(void) {
    wchar_t directory[MAX_PATH];
    wchar_t path[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, directory);
    CHECK(length > 0 && length < MAX_PATH);
    CHECK(GetTempFileNameW(directory, L"mfa", 0, path) != 0);
    FILE *file = _wfopen(path, L"w+bD");
    if (!file) DeleteFileW(path);
    CHECK(file != NULL);
    return file;
}

static void start_writer(encoding_test_t *test, HANDLE resume, uint32_t sample_offset) {
    flac_writer_config_t config = flac_writer_default_config();
    config.num_threads = test->num_threads;
    config.affinity_enabled = test->affinity_enabled;
    config.error_cb = fail_encode;
    snprintf(config.affinity_cpu_list, sizeof(config.affinity_cpu_list), "%s", test->cpu_list);
    test->file = temporary_flac();
    test->writer = flac_writer_create_stream(test->file, &config);
    CHECK(test->writer != NULL);
    test->sample_offset = sample_offset;
    test->ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    test->start = CreateEventW(NULL, FALSE, FALSE, NULL);
    test->resume = resume;
    CHECK(test->ready != NULL && test->start != NULL);
    test->thread = (HANDLE)_beginthreadex(NULL, 0, encode, test, 0, NULL);
    CHECK(test->thread != NULL);
    wait_for(test->ready);
    CHECK(same_affinity(read_affinity(test->thread), test->expected_affinity));
}

static void check_flac(const encoding_test_t *test) {
    CHECK(flac_writer_get_samples_written(test->writer) == SAMPLE_COUNT);
    CHECK(flac_writer_finish(test->writer) == FLAC_WRITER_OK);
    CHECK(fflush(test->file) == 0);
    CHECK(fseek(test->file, 0, SEEK_SET) == 0);
    FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
    CHECK(decoder != NULL);
    decoding_test_t decoded = {.sample_offset = test->sample_offset};
    CHECK(FLAC__stream_decoder_set_md5_checking(decoder, true));
    CHECK(FLAC__stream_decoder_init_FILE(decoder, test->file, check_decoded_samples,
        NULL, fail_decode, &decoded) == FLAC__STREAM_DECODER_INIT_STATUS_OK);
    CHECK(FLAC__stream_decoder_process_until_end_of_stream(decoder));
    CHECK(FLAC__stream_decoder_finish(decoder));
    FLAC__stream_decoder_delete(decoder);
    CHECK(decoded.position == SAMPLE_COUNT);
}

static void check_recordings(encoding_test_t tests[RECORDINGS], GROUP_AFFINITY original) {
    HANDLE resume = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(resume != NULL);
    for (uint32_t i = 0; i < RECORDINGS; ++i)
        start_writer(&tests[i], resume, i * SAMPLE_COUNT);

    /* Warm up each encoder separately to identify its workers while both
     * writer threads remain live. Then submit the rest from both writers. */
    thread_list_t baseline = snapshot_threads();
    thread_list_t workers[RECORDINGS];
    for (size_t i = 0; i < RECORDINGS; ++i) {
        CHECK(SetEvent(tests[i].start));
        wait_for(tests[i].ready);
        workers[i] = check_new_workers(&tests[i], &baseline);
    }
    CHECK(same_affinity(read_affinity(GetCurrentThread()), original));
    CHECK(SetEvent(resume));
    for (size_t i = 0; i < RECORDINGS; ++i) wait_for(tests[i].thread);
    CloseHandle(resume);

    thread_list_t current = snapshot_threads();
    for (size_t i = 0; i < current.count; ++i) CHECK(already_running(&baseline, current.ids[i]));
    for (size_t i = 0; i < RECORDINGS; ++i) {
        for (size_t n = 0; n < workers[i].count; ++n) {
            CHECK(same_affinity(read_worker_affinity(workers[i].ids[n]), tests[i].expected_affinity));
        }
        printf("Verified writer %c and %zu libFLAC workers in group %u with mask 0x%llx\n",
            (int)('A' + i), workers[i].count, (unsigned int)tests[i].expected_affinity.Group,
            (unsigned long long)tests[i].expected_affinity.Mask);
        CloseHandle(tests[i].thread);
        CloseHandle(tests[i].ready);
        CloseHandle(tests[i].start);
        check_flac(&tests[i]);
        CHECK(same_affinity(read_affinity(GetCurrentThread()), original));
    }
}

int main(void) {
    char error[256];
    if (GetActiveProcessorGroupCount() != 1) {
        CHECK(!flac_writer_affinity_supported());
        const char *reason = flac_writer_affinity_unavailable_reason();
        CHECK(reason != NULL);
        CHECK(!flac_writer_validate_affinity_cpu_list("0", error, sizeof(error)));
        CHECK(strcmp(error, reason) == 0);
        puts(reason);
        return 77;
    }
    CHECK(flac_writer_affinity_supported());
    CHECK(flac_writer_affinity_unavailable_reason() == NULL);
    const char *invalid_lists[] = {"", "   ", "-1", "2-1", "0,", "0;1",
        "0-99999999999999999999", "64"};
    for (size_t i = 0; i < sizeof(invalid_lists) / sizeof(invalid_lists[0]); ++i) {
        error[0] = '\0';
        CHECK(!flac_writer_validate_affinity_cpu_list(invalid_lists[i], error, sizeof(error)));
        CHECK(error[0] != '\0');
    }

    DWORD_PTR allowed, active;
    CHECK(GetProcessAffinityMask(GetCurrentProcess(), &allowed, &active));
    CHECK(allowed != 0);
    if ((allowed & (allowed - 1)) == 0) {
        puts("Affinity runtime test requires at least two allowed logical processors.");
        return 77;
    }

    unsigned int cpu = 0;
    while ((allowed & ((DWORD_PTR)1 << cpu)) == 0) ++cpu;
    const KAFFINITY target = (KAFFINITY)1 << cpu;
    char cpu_list[64];
    snprintf(cpu_list, sizeof(cpu_list), " %u - %u, %u ", cpu, cpu, cpu);
    CHECK(flac_writer_validate_affinity_cpu_list(cpu_list, error, sizeof(error)));

    /* Check that a group-local CPU unavailable to the process fails validation. */
    for (unsigned int index = 0; index < sizeof(allowed) * CHAR_BIT; ++index) {
        if (allowed & ((DWORD_PTR)1 << index)) continue;
        char unavailable[16];
        snprintf(unavailable, sizeof(unavailable), "%u", index);
        CHECK(!flac_writer_validate_affinity_cpu_list(unavailable, error, sizeof(error)));
        break;
    }

    unsigned int other_cpu = cpu + 1;
    while ((allowed & ((DWORD_PTR)1 << other_cpu)) == 0) ++other_cpu;
    char other_cpu_list[16];
    snprintf(other_cpu_list, sizeof(other_cpu_list), "%u", other_cpu);
    const KAFFINITY other_target = (KAFFINITY)1 << other_cpu;

    GROUP_AFFINITY original = read_affinity(GetCurrentThread());
    GROUP_AFFINITY unrestricted = {.Group = original.Group, .Mask = allowed};
    GROUP_AFFINITY first = {.Group = original.Group, .Mask = target};
    GROUP_AFFINITY second = {.Group = original.Group, .Mask = other_target};
    encoding_test_t mixed[RECORDINGS] = {
        {.cpu_list = cpu_list, .expected_affinity = unrestricted, .num_threads = 3},
        {.cpu_list = other_cpu_list, .expected_affinity = second, .affinity_enabled = true, .num_threads = 1}
    };
    check_recordings(mixed, original);

    /* Include a multi-CPU mask when it can be disjoint from the second mask. */
    KAFFINITY remaining = allowed & ~(target | other_target);
    if (remaining) {
        unsigned int extra_cpu = sizeof(allowed) * CHAR_BIT - 1;
        while ((remaining & ((KAFFINITY)1 << extra_cpu)) == 0) --extra_cpu;
        first.Mask |= (KAFFINITY)1 << extra_cpu;
        snprintf(cpu_list, sizeof(cpu_list), "%u,%u", cpu, extra_cpu);
    }
    encoding_test_t both_enabled[RECORDINGS] = {
        {.cpu_list = cpu_list, .expected_affinity = first, .affinity_enabled = true, .num_threads = 3},
        {.cpu_list = other_cpu_list, .expected_affinity = second, .affinity_enabled = true, .num_threads = 4}
    };
    check_recordings(both_enabled, original);
    puts("Windows FLAC affinity and lossless round-trip passed.");
    return 0;
}
