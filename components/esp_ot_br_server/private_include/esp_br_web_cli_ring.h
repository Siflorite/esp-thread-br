/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLI_RING_TEXT_SIZE 256
/* Serialized header: uint64_t sequence, uint16_t length, source, reserved.
 * No structure padding or alignment is stored in the byte ring. */
#define CLI_RING_HEADER_SIZE 12

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t used;
    uint64_t first_seq;
    uint64_t next_seq;
} cli_ring_t;

#define CLI_RING_INITIALIZER(storage) \
    {.data = (storage), .capacity = sizeof(storage), .first_seq = 1, .next_seq = 1}

typedef struct {
    size_t offset;
    uint64_t seq;
} cli_ring_reader_t;

typedef struct {
    uint64_t seq;
    char source;
    char text[CLI_RING_TEXT_SIZE];
} cli_ring_record_t;

/* Caller serializes ALL access, including reader creation. No allocation or I/O.
 * Append evicts whole oldest records as needed. Invalid/oversize writes leave
 * the ring unchanged. Text is stored without a terminating NUL. */
bool cli_ring_append(cli_ring_t *ring, char source, const char *text, size_t length);
cli_ring_reader_t cli_ring_begin(const cli_ring_t *ring);
/* Each reader is independent; reads do not consume the shared transcript.
 * Returns false at the current end or if unread data was overwritten. A NULL
 * record skips one entry without copying its payload. The caller can release
 * its lock between entries, including while skipping to a requested sequence. */
bool cli_ring_read(const cli_ring_t *ring, cli_ring_reader_t *reader, cli_ring_record_t *record);
