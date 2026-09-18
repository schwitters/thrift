/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/transport/thrift_memory_buffer.h>
#include <string.h>

static enum thrift_status memory_read(void *context, void *data, size_t size)
{
    struct thrift_memory *memory = context;
    if (!memory || (!data && size) || memory->position > memory->size ||
        memory->size > memory->capacity)
        return THRIFT_INVALID;
    if (size > memory->size - memory->position)
        return THRIFT_EOF;
    if (size)
        memmove(data, memory->data + memory->position, size);
    memory->position += size;
    return THRIFT_OK;
}

static enum thrift_status memory_write(void *context, const void *data, size_t size)
{
    struct thrift_memory *memory = context;
    if (!memory || (!data && size) || memory->size > memory->capacity)
        return THRIFT_INVALID;
    if (size > memory->capacity - memory->size)
        return THRIFT_LIMIT;
    if (size)
        memmove(memory->data + memory->size, data, size);
    memory->size += size;
    return THRIFT_OK;
}

enum thrift_status thrift_memory_init(struct thrift_memory *memory,
                                             void *data, size_t capacity, size_t read_size,
                                             struct thrift_transport *transport)
{
    if (!memory || !transport || (!data && capacity) || read_size > capacity)
        return THRIFT_INVALID;
    memory->data = data;
    memory->capacity = capacity;
    memory->size = read_size;
    memory->position = 0;
    transport->context = memory;
    transport->read = memory_read;
    transport->write = memory_write;
    transport->flush = NULL;
    return THRIFT_OK;
}
