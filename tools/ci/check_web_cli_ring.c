/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
/* Host tests: see docs/webui-web-cli.md for the compile/run command. */
#include "esp_br_web_cli_ring.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    cli_ring_record_t record;
    size_t length;
} expected_t;

static expected_t expected[65536 / (CLI_RING_HEADER_SIZE + 1) + 1];
static uint32_t random_state = 1;

static uint32_t random_value(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static void check_record(const cli_ring_record_t *actual, const expected_t *wanted)
{
    assert(actual->seq == wanted->record.seq);
    assert(actual->source == wanted->record.source);
    assert(memcmp(actual->text, wanted->record.text, wanted->length + 1) == 0);
}

static void check_contents(const cli_ring_t *ring, size_t count, size_t used, uint64_t next)
{
    assert(ring->used == used);
    assert(ring->next_seq == next);
    assert(ring->first_seq == next - count);
    assert(ring->head < ring->capacity && ring->tail < ring->capacity);
    cli_ring_reader_t a = cli_ring_begin(ring);
    cli_ring_reader_t b = cli_ring_begin(ring);
    cli_ring_record_t record;
    for (size_t i = 0; i < count; i++) {
        assert(cli_ring_read(ring, &a, &record));
        check_record(&record, &expected[i]);
        /* One client's reads never consume another client's records. Also
         * exercise skipping metadata to an HTTP client's requested cursor. */
        assert(cli_ring_read(ring, &b, i % 2 ? NULL : &record));
        if (i % 2 == 0) {
            check_record(&record, &expected[i]);
        }
    }
    assert(!cli_ring_read(ring, &a, &record));
    assert(!cli_ring_read(ring, &b, &record));
    assert(ring->used == used);
}

static void test_capacity(size_t capacity)
{
    uint8_t *allocation = malloc(capacity + 32);
    assert(allocation);
    memset(allocation, 0xa5, capacity + 32);
    cli_ring_t ring = {.data = allocation + 16, .capacity = capacity, .first_seq = 1, .next_seq = 1};
    size_t count = 0, used = 0;
    uint64_t next = 1;
    bool header_wrapped = false, text_wrapped = false, evicted_multiple = false;
    check_contents(&ring, count, used, next);
    cli_ring_reader_t slow = cli_ring_begin(&ring);

    for (int step = 0; step < 20000; step++) {
        /* Include maximum-sized fragments, tiny fragments and mixed lengths. */
        size_t length = step < 2000 ? 1 : step % 3 == 0 ? 255 : 1 + random_value() % 255;
        char text[CLI_RING_TEXT_SIZE];
        char source = "LCI"[random_value() % 3];
        for (size_t i = 0; i < length; i++) {
            text[i] = (char)(1 + random_value() % 255);
        }
        text[length] = 0;
        size_t size = CLI_RING_HEADER_SIZE + length;
        bool accepted = cli_ring_append(&ring, source, text, length);
        assert(accepted == (size <= capacity));
        if (accepted) {
            size_t evicted = 0;
            while (used + size > capacity) {
                used -= CLI_RING_HEADER_SIZE + expected[evicted++].length;
            }
            if (evicted > 1) {
                evicted_multiple = true;
            }
            memmove(expected, expected + evicted, (count - evicted) * sizeof(*expected));
            count -= evicted;
            expected[count].record.seq = next++;
            expected[count].record.source = source;
            memcpy(expected[count].record.text, text, length + 1);
            expected[count++].length = length;
            used += size;
            size_t start = (ring.tail + capacity - size) % capacity;
            header_wrapped |= start + CLI_RING_HEADER_SIZE > capacity;
            text_wrapped |= (start + CLI_RING_HEADER_SIZE) % capacity + length > capacity;
        }
        assert(ring.used == used && ring.first_seq == next - count);
        if (slow.seq < ring.first_seq) {
            cli_ring_reader_t stale = slow;
            assert(!cli_ring_read(&ring, &slow, NULL));
            assert(slow.seq == stale.seq && slow.offset == stale.offset);
            slow = cli_ring_begin(&ring);
        } else if (slow.seq < ring.next_seq && step % 7 == 0) {
            cli_ring_record_t record;
            size_t index = (size_t)(slow.seq - ring.first_seq);
            assert(cli_ring_read(&ring, &slow, &record));
            check_record(&record, &expected[index]);
        }
        if (step % 127 == 0) {
            check_contents(&ring, count, used, next);
        }
        for (size_t i = 0; i < 16; i++) {
            assert(allocation[i] == 0xa5 && allocation[capacity + 16 + i] == 0xa5);
        }
    }
    check_contents(&ring, count, used, next);
    assert(!cli_ring_append(&ring, 'L', "", 0));
    assert(!cli_ring_append(&ring, 'L', NULL, 1));
    assert(!cli_ring_append(&ring, 'L', "x", CLI_RING_TEXT_SIZE));
    check_contents(&ring, count, used, next);
    if (capacity >= 267) {
        assert(header_wrapped && text_wrapped && evicted_multiple);
    }
    free(allocation);
}

static void test_exact_fit_and_resume(void)
{
    uint8_t storage[2 * (CLI_RING_HEADER_SIZE + 2)];
    cli_ring_t ring = CLI_RING_INITIALIZER(storage);
    assert(cli_ring_append(&ring, 'C', "> ", 2));
    cli_ring_reader_t a = cli_ring_begin(&ring);
    cli_ring_record_t record;
    assert(cli_ring_read(&ring, &a, &record));
    assert(!cli_ring_read(&ring, &a, &record));
    assert(cli_ring_append(&ring, 'L', "\r\n", 2));
    assert(ring.used == sizeof(storage) && ring.head == ring.tail);
    assert(cli_ring_read(&ring, &a, &record));
    assert(record.seq == 2 && !strcmp(record.text, "\r\n"));
    cli_ring_reader_t stale = cli_ring_begin(&ring);
    assert(cli_ring_append(&ring, 'I', "ot", 2));
    assert(ring.first_seq == 2);
    assert(!cli_ring_read(&ring, &stale, &record));
    assert(cli_ring_read(&ring, &a, &record));
    assert(record.seq == 3 && !strcmp(record.text, "ot"));
}

int main(void)
{
    test_exact_fit_and_resume();
    const size_t capacities[] = {13, 14, 267, 269, 1024, 16384, 65536};
    for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++) {
        test_capacity(capacities[i]);
    }
    puts("Web CLI byte ring checks passed: wrap, eviction, exact fit, independent readers, stale cursors, invalid writes and randomized FIFO model.");
    return 0;
}
