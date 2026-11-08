/*
 * MISRC Common - Ringbuffer to File Writer Implementation
 */

#include "ringbuffer_writer.h"
#include "threading.h"

#include <string.h>

void rb_writer_config_init(rb_writer_config_t *config,
                           ringbuffer_t *rb,
                           FILE *file,
                           size_t read_size)
{
    memset(config, 0, sizeof(*config));
    config->rb = rb;
    config->file = file;
    config->read_size = read_size;
    config->sleep_ms = 1;  /* Default 1ms sleep */
}

static bool should_exit(rb_writer_config_t *config)
{
    /* Check callback first if provided */
    if (config->should_exit_cb) {
        return config->should_exit_cb(config->user_ctx);
    }

    /* Otherwise check atomic flag */
    if (config->exit_flag) {
        return atomic_load(config->exit_flag);
    }

    /* No exit condition configured - never exit */
    return false;
}

size_t rb_writer_run(rb_writer_config_t *config)
{
    size_t total_written = 0;
    size_t len = config->read_size;
    void *buf;

    while (1) {
        buf = rb_read_ptr(config->rb, len);

        if (!buf) {
            /* No data available - check if we should exit */
            if (should_exit(config)) {
                /* Drain any remaining partial data before exiting */
                size_t remaining = rb_fill_level(config->rb);
                if (remaining > 0 && remaining < len) {
                    buf = rb_read_ptr(config->rb, remaining);
                    if (buf) {
                        size_t written = fwrite(buf, 1, remaining, config->file);
                        rb_read_finished(config->rb, remaining);
                        total_written += written;

                        if (config->progress_cb) {
                            config->progress_cb(config->user_ctx, written);
                        }
                    }
                }
                break;
            }

            /* Sleep and retry */
            thrd_sleep_ms(config->sleep_ms);
            continue;
        }

        /* Write data to file */
        size_t written = fwrite(buf, 1, len, config->file);
        rb_read_finished(config->rb, len);
        total_written += written;

        if (config->progress_cb) {
            config->progress_cb(config->user_ctx, written);
        }
    }

    return total_written;
}

int rb_writer_thread(void *ctx)
{
    rb_writer_config_t *config = (rb_writer_config_t *)ctx;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);
    rb_writer_run(config);
    return 0;
}
