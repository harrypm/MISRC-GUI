#include "ringbuffer.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s failed\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int advance_empty(ringbuffer_t *rb, size_t size) {
    CHECK(rb_fill_level(rb) == 0);
    CHECK(rb_write_ptr(rb, size) != NULL);
    CHECK(rb_write_finished(rb, size) == 0);
    CHECK(rb_fill_level(rb) == size);
    CHECK(rb_read_finished(rb, size) == 0);
    CHECK(rb_fill_level(rb) == 0);
    return 0;
}

static int exercise_wrap(ringbuffer_t *rb) {
    uint8_t crossing[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const uint8_t pending[] = {21, 22, 23, 24, 25, 26, 27, 28};

    /* Start eight bytes before the mapping seam. The producer can prepare a
     * write, then be preempted while the consumer passes the seam. */
    CHECK(rb_put(rb, crossing, sizeof(crossing)) == 0);
    CHECK(rb_fill_level(rb) == sizeof(crossing));
    void *write = rb_write_ptr(rb, sizeof(pending));
    CHECK(write != NULL);
    memcpy(write, pending, sizeof(pending));

    const void *read = rb_read_ptr(rb, sizeof(crossing));
    CHECK(read == rb->buffer + rb->buffer_size - 8);
    CHECK(memcmp(read, crossing, sizeof(crossing)) == 0);
    size_t tail_before = atomic_load(&rb->tail);
    CHECK(rb_read_finished(rb, sizeof(crossing)) == 0);
    CHECK(atomic_load(&rb->tail) == tail_before);
    CHECK(rb_fill_level(rb) == 0);
    CHECK(rb_read_ptr(rb, sizeof(pending)) == NULL);

    CHECK(rb_write_finished(rb, sizeof(pending)) == 0);
    CHECK(rb_fill_level(rb) == sizeof(pending));
    read = rb_read_ptr(rb, sizeof(pending));
    CHECK(read == rb->buffer + 8);
    CHECK(memcmp(read, pending, sizeof(pending)) == 0);
    CHECK(rb_read_finished(rb, sizeof(pending)) == 0);
    CHECK(rb_fill_level(rb) == 0);
    return 0;
}

static int test_wrap(ringbuffer_t *rb) {
    size_t capacity = rb->buffer_size;
    CHECK(rb_write_ptr(rb, capacity + 1) == NULL);
    CHECK(rb_read_finished(rb, 1) != 0);

    CHECK(advance_empty(rb, capacity - 8) == 0);
    CHECK(exercise_wrap(rb) == 0);

    /* At the next physical seam both cursors also cross their logical cycle. */
    CHECK(advance_empty(rb, capacity - 24) == 0);
    CHECK(atomic_load(&rb->head) == 2 * capacity - 8);
    CHECK(exercise_wrap(rb) == 0);
    CHECK(atomic_load(&rb->head) == 16);
    CHECK(atomic_load(&rb->tail) == 16);

    CHECK(rb_write_ptr(rb, capacity) != NULL);
    CHECK(rb_write_finished(rb, capacity) == 0);
    CHECK(rb_fill_level(rb) == capacity);
    CHECK(rb_write_ptr(rb, 1) == NULL);
    CHECK(rb_write_finished(rb, 1) != 0);
    CHECK(rb_read_finished(rb, capacity) == 0);
    CHECK(rb_fill_level(rb) == 0);
    return 0;
}

static int test_invalid_observer_pair(size_t capacity) {
    ringbuffer_t rb = {.buffer_size = capacity};
    /* Model mixed observations when a third thread spans a cursor update. */
    atomic_store(&rb.head, capacity);
    atomic_store(&rb.tail, capacity - 1);
    CHECK(rb_fill_level(&rb) <= capacity);
    atomic_store(&rb.head, 0);
    atomic_store(&rb.tail, capacity + 1);
    CHECK(rb_fill_level(&rb) <= capacity);
    return 0;
}

int main(void) {
    /* Use a non-power-of-two capacity aligned for the Windows mappings. */
    const size_t capacity = 3 * 65536;
    ringbuffer_t rb;
    int init_result = rb_init(&rb, "wrap-test", capacity);
    if (init_result != 0) {
        fprintf(stderr, "rb_init failed (%d)\n", init_result);
        return 1;
    }

    int failed = test_wrap(&rb);
    rb_close(&rb);
    if (failed || test_invalid_observer_pair(capacity)) return 1;
    puts("PASS: ringbuffer wrap and fill observations");
    return 0;
}
