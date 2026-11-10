/* Exercise the capture loop with scripted USB reads and no attached device. */
#include <assert.h>

#include "../misrc_gui/input/gui_rtlsdr.c"

#define TEST_CENTER_HZ 1500000u
#define TEST_RATE_HZ 1024000u
#define TEST_REQUEST_RATE_HZ 1023999u

struct rtlsdr_dev {
    uint32_t rate;
    uint32_t center;
};

static struct rtlsdr_dev device;
static gui_app_t app;
static unsigned reads, pushes, gaps, source_losses, previews;
static bool fail_sample_rate;
#ifdef ENABLE_RTLSDR_BLOG_V4_VGA
static bool opened_blog_v4;
static bool manual_gain_applied;
static unsigned vga_calls;
static int vga_index_applied;
static int vga_result;
#endif
static uint32_t packed[RTL_READ_BYTES / 2u];
atomic_int do_exit;

static bool source_ready(uint64_t expected) {
    uint32_t rate;
    uint64_t tuning;
    if (!gui_rtlsdr_get_rf_source(&app, &rate, &tuning)) return false;
    assert(rate == TEST_RATE_HZ && tuning == expected);
    return true;
}

int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read) {
    assert(dev == &device && len == RTL_READ_BYTES);
    const uint64_t initial = RTL_TUNE_STEP | TEST_CENTER_HZ;
    const uint64_t retuned = (2 * RTL_TUNE_STEP) | TEST_CENTER_HZ;
    unsigned read = reads++;
    *n_read = len;
    assert(atomic_load(&app.sample_rate) == TEST_RATE_HZ);
    if (fail_sample_rate) {
        assert(rtlsdr_get_sample_rate(dev) == TEST_RATE_HZ);
        assert(!source_ready(initial));
        if (read == 2) {
            atomic_store(&s_rtlsdr_running, false);
            *n_read = 0;
        }
        memset(buf, 0x31, (size_t)*n_read);
        return 0;
    }
    switch (read) {
    case 0:
        assert(!source_ready(initial));
        break;
    case 1:
        assert(source_ready(initial) && previews == 1 && pushes == 0);
        // Retune while this read still holds the prior generation.
        assert(gui_rtlsdr_set_frequency(&app, TEST_CENTER_HZ) == 0);
        assert(!source_ready(retuned));
        break;
    case 2:
        assert(!source_ready(retuned) && previews == 1);
        *n_read = len / 2 - 1;
        break;
    case 3:
        assert(!source_ready(retuned) && previews == 1);
        *n_read = len / 2;
        break;
    case 4:
        assert(!source_ready(retuned) && previews == 2);
        *n_read = 2;
        break;
    case 5:
        assert(source_ready(retuned) && pushes == 0);
        *n_read = 1;
        break;
    case 6:
        assert(source_ready(retuned) && pushes == 1);
        *n_read = 0;
        return -1;
    case 7:
        assert(!source_ready(retuned) && gaps == 2 && source_losses == 1);
        break;
    default:
        assert(source_ready(retuned) && pushes == 1);
        atomic_store(&s_rtlsdr_running, false);
        *n_read = 0;
        return 0;
    }
    memset(buf, 0x31, (size_t)*n_read);
    if (read == 4) ((uint8_t *)buf)[1] = 0x9d; // Trailing I after the settling batch.
    if (read == 5) ((uint8_t *)buf)[0] = 0x6e; // Q arrives in the next read.
    return 0;
}

void gui_record_rtlsdr_push(gui_app_t *owner, const uint8_t *iq, size_t pairs, uint64_t tuning) {
    assert(owner == &app && reads == 6);
    assert(tuning == ((2 * RTL_TUNE_STEP) | TEST_CENTER_HZ));
    assert(pairs == 1 && iq[0] == 0x9d && iq[1] == 0x6e);
    pushes++;
}

bool gui_record_rtlsdr_active(void) { return false; }
void gui_record_rtlsdr_gap(gui_app_t *owner) {
    assert(owner == &app);
    assert(!source_ready((2 * RTL_TUNE_STEP) | TEST_CENTER_HZ));
    gaps++;
}

void gui_record_mark_source_loss(void) { source_losses++; }

void *bufmgr_write_begin(buffer_manager_t *mgr, buffer_id_t id, size_t bytes,
                        const backpressure_policy_t *policy) {
    (void)policy;
    assert(mgr == &app.buffers && id == BUF_CAPTURE_RF && bytes <= sizeof(packed));
    return reads == 6 ? NULL : packed; // Recording must precede display FIFO admission.
}

void bufmgr_write_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes) {
    assert(mgr == &app.buffers && id == BUF_CAPTURE_RF && bytes <= sizeof(packed));
}

void gui_video_submit_rtlsdr(gui_app_t *owner, const uint8_t *iq, size_t count,
                             const gui_rf_block_info_t *info) {
    assert(owner == &app && iq && count && info->iq_center_hz == TEST_CENTER_HZ);
    if (previews == 0 || (!fail_sample_rate && previews == 1)) assert(info->discontinuity);
    previews++;
}

void gui_video_reset_rtlsdr(gui_app_t *owner) { assert(owner == &app); }

int rtlsdr_open(rtlsdr_dev_t **out, uint32_t index) {
    assert(index == 0);
    *out = &device;
#ifdef ENABLE_RTLSDR_BLOG_V4_VGA
    manual_gain_applied = false;
#endif
    return 0;
}

int rtlsdr_close(rtlsdr_dev_t *dev) { assert(dev == &device); return 0; }
int rtlsdr_reset_buffer(rtlsdr_dev_t *dev) { assert(dev == &device); return 0; }
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev) { return dev->center; }
uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev) { return dev->rate; }
int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t center) { dev->center = center; return 0; }
int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t rate) {
    assert(rate == TEST_REQUEST_RATE_HZ);
    // Match the real library: its cache is set before USB writes can fail.
    dev->rate = TEST_RATE_HZ;
    return fail_sample_rate ? -1 : 0;
}
int rtlsdr_set_agc_mode(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int mode) { (void)dev; (void)mode; return 0; }
int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain) {
    (void)dev;
    (void)gain;
#ifdef ENABLE_RTLSDR_BLOG_V4_VGA
    manual_gain_applied = true;
#endif
    return 0;
}

#ifdef ENABLE_RTLSDR_BLOG_V4_VGA
int rtlsdr_is_blog_v4(rtlsdr_dev_t *dev) {
    assert(dev == &device);
    return opened_blog_v4;
}

int rtlsdr_set_blog_v4_vga_index(rtlsdr_dev_t *dev, int index) {
    assert(dev == &device && manual_gain_applied);
    vga_calls++;
    vga_index_applied = index;
    return vga_result;
}
#endif
int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }

uint32_t rtlsdr_get_device_count(void) { return 1; }
const char *rtlsdr_get_device_name(uint32_t index) { assert(index == 0); return "RTL-SDR"; }
int rtlsdr_get_device_usb_strings(uint32_t index, char *manufacturer, char *product, char *serial) {
    (void)index; (void)manufacturer; (void)product; (void)serial;
    return -1;
}

int bufmgr_ensure_init(buffer_manager_t *mgr, buffer_id_t id) { (void)mgr; (void)id; return 0; }
void bufmgr_reset(buffer_manager_t *mgr, buffer_id_t id) { (void)mgr; (void)id; }
void bufmgr_reset_stats(buffer_manager_t *mgr, buffer_id_t id) { (void)mgr; (void)id; }
int gui_extract_start(gui_app_t *owner) { (void)owner; return 0; }
void gui_extract_stop(void) {}
int gui_display_thread_start(display_thread_t *dt, gui_app_t *owner, buffer_manager_t *mgr) {
    (void)dt; (void)owner; (void)mgr; return 0;
}
void gui_display_thread_stop(display_thread_t *dt) { (void)dt; }
void gui_app_set_status(gui_app_t *owner, const char *message) { (void)owner; (void)message; }
void gui_settings_save(const gui_settings_t *settings) { (void)settings; }

static void run_capture(bool fail_rate) {
    fail_sample_rate = fail_rate;
    reads = pushes = gaps = source_losses = previews = 0;
    assert(gui_rtlsdr_open(&app, 0) == 0);
    assert(atomic_load(&s_rtlsdr_record_tuning) == 0);
    assert(gui_rtlsdr_start(&app) == 0);
    const uint64_t deadline = get_time_ms() + 2000u;
    while (gui_rtlsdr_is_running(&app) && get_time_ms() < deadline)
        thrd_sleep_ms(1);
    assert(!gui_rtlsdr_is_running(&app));
    gui_rtlsdr_stop(&app);
    assert(atomic_load(&s_rtlsdr_record_tuning) == 0);
    assert(atomic_load(&s_rtlsdr_actual_rate) == 0);
}

static void test_read_timeout_gap(void) {
    const int timeout = -7; // LIBUSB_ERROR_TIMEOUT
    const uint64_t tuning = (2 * RTL_TUNE_STEP) | TEST_CENTER_HZ;
    atomic_store(&app.rf_source_rtlsdr_tuning, tuning);
    atomic_store(&s_rtlsdr_running, true);
    atomic_store(&s_rtlsdr_actual_rate, TEST_RATE_HZ);
    atomic_store(&s_rtlsdr_record_tuning, tuning);
    gaps = 0;

    assert(!rtlsdr_interrupt_recording_on_read_gap(&app, timeout, 17, true));
    assert(source_ready(tuning) && gaps == 0);
    assert(rtlsdr_interrupt_recording_on_read_gap(&app, timeout, 0, true));
    assert(!source_ready(tuning) && gaps == 1);

    atomic_store(&s_rtlsdr_running, false);
    atomic_store(&s_rtlsdr_actual_rate, 0);
}

#if defined(ENABLE_RTLSDR) && defined(ENABLE_RTLSDR_BLOG_V4_VGA)
static void test_blog_v4_vga(void) {
    rtlsdr_device_info_t info;
    // Descriptor reads can fail during enumeration while succeeding on open.
    assert(gui_rtlsdr_enumerate(&info, 1) == 1 && !info.blog_v4);
    opened_blog_v4 = true;
    app.settings.rtlsdr_gain_mode = 1;
    app.settings.rtlsdr_vga_index = 0;
    assert(gui_rtlsdr_open(&app, 0) == 0);
    assert(vga_calls == 1 && vga_index_applied == 0);
    gui_rtlsdr_close(&app);

    app.settings.rtlsdr_vga_index = 8;
    assert(gui_rtlsdr_open(&app, 0) == 0);
    assert(vga_calls == 2 && vga_index_applied == 8);
    gui_rtlsdr_close(&app);

    opened_blog_v4 = false;
    assert(gui_rtlsdr_open(&app, 0) == 0);
    assert(vga_calls == 2);
    gui_rtlsdr_close(&app);

    opened_blog_v4 = true;
    app.settings.rtlsdr_gain_mode = 0;
    assert(gui_rtlsdr_open(&app, 0) == 0);
    assert(vga_calls == 2);
    gui_rtlsdr_close(&app);

    app.settings.rtlsdr_gain_mode = 1;
    app.settings.rtlsdr_vga_index = 5;
    vga_result = -1;
    assert(gui_rtlsdr_open(&app, 0) == GUI_RTLSDR_VGA_FAILED);
    assert(vga_calls == 3 && vga_index_applied == 5 && !s_rtlsdr_dev);
    vga_result = 0;
    opened_blog_v4 = false;
    app.settings.rtlsdr_gain_mode = 0;
}
#endif

int main(void) {
    app.settings.rtlsdr_freq_hz = TEST_CENTER_HZ;
    app.settings.rtlsdr_sample_rate_hz = TEST_REQUEST_RATE_HZ;
    test_read_timeout_gap();
#if defined(ENABLE_RTLSDR) && defined(ENABLE_RTLSDR_BLOG_V4_VGA)
    test_blog_v4_vga();
#endif

    run_capture(false);
    assert(reads == 9 && pushes == 1 && gaps == 2 && source_losses == 1);

    run_capture(true);
    assert(reads == 3 && previews == 2 && pushes == 0 && gaps == 0 && source_losses == 0);

    run_capture(false);
    assert(reads == 9 && pushes == 1 && gaps == 2 && source_losses == 1);
    return 0;
}
