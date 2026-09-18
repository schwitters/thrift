/* SPDX-License-Identifier: Apache-2.0 */
#ifndef THRIFT_BUFFER_INTERNAL_H
#define THRIFT_BUFFER_INTERNAL_H
#include <thrift/c11/thrift.h>

/* Owned, bounded buffer; initialize to zero and set limit before use. */
struct thrift_buffer {
    uint8_t *data;
    size_t size, capacity, position, limit;
};
enum thrift_status thrift_buffer_append(struct thrift_buffer *buffer,
                                               const void *data, size_t size);
enum thrift_status thrift_buffer_read(struct thrift_buffer *buffer,
                                             void *data, size_t size);
void thrift_buffer_clear(struct thrift_buffer *buffer);
#endif
