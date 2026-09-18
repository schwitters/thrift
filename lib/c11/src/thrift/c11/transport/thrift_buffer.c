/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_buffer_internal.h"
#include <stdlib.h>
#include <string.h>

enum { BUFFER_INITIAL_CAPACITY = 256 };

enum thrift_status thrift_buffer_append(struct thrift_buffer *buffer,
                                               const void *data, size_t size)
{
    size_t needed, capacity;
    uint8_t *next;
    if (!buffer || (!data && size) || buffer->size > buffer->limit)
        return THRIFT_INVALID;
    if (size > buffer->limit - buffer->size)
        return THRIFT_LIMIT;
    needed = buffer->size + size;
    if (needed > buffer->capacity) {
        capacity = buffer->capacity ? buffer->capacity : BUFFER_INITIAL_CAPACITY;
        while (capacity < needed && capacity <= buffer->limit / 2)
            capacity *= 2;
        if (capacity < needed || capacity > buffer->limit)
            capacity = needed;
        next = realloc(buffer->data, capacity);
        if (!next)
            return THRIFT_NOMEM;
        buffer->data = next;
        buffer->capacity = capacity;
    }
    if (size)
        memcpy(buffer->data + buffer->size, data, size);
    buffer->size = needed;
    return THRIFT_OK;
}

enum thrift_status thrift_buffer_read(struct thrift_buffer *buffer,
                                             void *data, size_t size)
{
    if (!buffer || (!data && size) || buffer->position > buffer->size)
        return THRIFT_INVALID;
    if (size > buffer->size - buffer->position)
        return THRIFT_EOF;
    if (size)
        memcpy(data, buffer->data + buffer->position, size);
    buffer->position += size;
    return THRIFT_OK;
}

void thrift_buffer_clear(struct thrift_buffer *buffer)
{
    if (!buffer)
        return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}
