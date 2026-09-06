/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "esp_br_web_cli_ring.h"

#include <string.h>

static void copy_from_ring(const cli_ring_t *ring, size_t offset, void *dest, size_t length)
{
    size_t first = ring->capacity - offset;
    if (first > length) {
        first = length;
    }
    memcpy(dest, ring->data + offset, first);
    memcpy((uint8_t *)dest + first, ring->data, length - first);
}

static void copy_to_ring(cli_ring_t *ring, size_t offset, const void *src, size_t length)
{
    size_t first = ring->capacity - offset;
    if (first > length) {
        first = length;
    }
    memcpy(ring->data + offset, src, first);
    memcpy(ring->data, (const uint8_t *)src + first, length - first);
}

static uint16_t record_length(const cli_ring_t *ring, size_t offset)
{
    uint16_t length;
    copy_from_ring(ring, (offset + sizeof(uint64_t)) % ring->capacity, &length, sizeof(length));
    return length;
}

bool cli_ring_append(cli_ring_t *ring, char source, const char *text, size_t length)
{
    if (!text || length == 0 || length >= CLI_RING_TEXT_SIZE ||
        CLI_RING_HEADER_SIZE + length > ring->capacity) {
        return false;
    }
    size_t size = CLI_RING_HEADER_SIZE + length;
    while (ring->capacity - ring->used < size) {
        size_t oldest_size = CLI_RING_HEADER_SIZE + record_length(ring, ring->head);
        ring->head = (ring->head + oldest_size) % ring->capacity;
        ring->used -= oldest_size;
        ring->first_seq++;
    }

    uint8_t header[CLI_RING_HEADER_SIZE] = {0};
    uint16_t text_length = (uint16_t)length;
    memcpy(header, &ring->next_seq, sizeof(ring->next_seq));
    memcpy(header + sizeof(uint64_t), &text_length, sizeof(text_length));
    header[10] = (uint8_t)source;
    copy_to_ring(ring, ring->tail, header, sizeof(header));
    copy_to_ring(ring, (ring->tail + sizeof(header)) % ring->capacity, text, length);
    ring->tail = (ring->tail + size) % ring->capacity;
    ring->used += size;
    ring->next_seq++;
    return true;
}

cli_ring_reader_t cli_ring_begin(const cli_ring_t *ring)
{
    return (cli_ring_reader_t){.offset = ring->head, .seq = ring->first_seq};
}

bool cli_ring_read(const cli_ring_t *ring, cli_ring_reader_t *reader, cli_ring_record_t *record)
{
    if (reader->seq < ring->first_seq || reader->seq >= ring->next_seq) {
        return false;
    }
    uint8_t header[CLI_RING_HEADER_SIZE];
    uint64_t seq;
    uint16_t length;
    copy_from_ring(ring, reader->offset, header, sizeof(header));
    memcpy(&seq, header, sizeof(seq));
    memcpy(&length, header + sizeof(seq), sizeof(length));
    if (seq != reader->seq || length >= CLI_RING_TEXT_SIZE) {
        return false;
    }
    if (record) {
        record->seq = seq;
        record->source = (char)header[10];
        copy_from_ring(ring, (reader->offset + sizeof(header)) % ring->capacity, record->text, length);
        record->text[length] = 0;
    }
    reader->offset = (reader->offset + sizeof(header) + length) % ring->capacity;
    reader->seq++;
    return true;
}
