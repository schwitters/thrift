/* SPDX-License-Identifier: Apache-2.0 */
#ifndef THRIFT_INTERNAL_H
#define THRIFT_INTERNAL_H
#include <thrift/c11/protocol/thrift_protocol.h>
enum { C11_BYTE_BYTES = 1, C11_I16_BYTES = 2, C11_I32_BYTES = 4, C11_I64_BYTES = 8 };
enum { C11_I16_BITS = 16, C11_I32_BITS = 32, C11_I64_BITS = 64 };
enum thrift_message { C11_CALL = 1, C11_REPLY = 2, C11_EXCEPTION = 3, C11_ONEWAY = 4 };
enum thrift_status thrift_transfer(struct thrift_protocol *p, void *data, size_t size, bool read);
enum thrift_status thrift_number(struct thrift_protocol *p, uint64_t *value, size_t size, bool read);
enum thrift_status thrift_skip(struct thrift_protocol *p, enum thrift_wire wire, unsigned depth);
enum thrift_status thrift_message_write(struct thrift_protocol *p, const char *name,
                                        enum thrift_message kind, int32_t sequence);
enum thrift_status thrift_message_read(struct thrift_protocol *p, struct thrift_bytes *name,
                                       enum thrift_message *kind, int32_t *sequence);
enum thrift_status thrift_allocate(struct thrift_protocol *p, size_t count, size_t size, void **out);
/* Scalar bits use the corresponding unsigned width; bool is normalized to 0/1.
 * Field bool_inline is 0 for a separate value, 1 for true and 2 for false.
 * Field IDs are uint16_t bit patterns; last_id is local to each struct. */
struct thrift_protocol_ops {
    enum thrift_status (*scalar)(struct thrift_protocol *, enum thrift_wire, uint64_t *, bool);
    enum thrift_status (*field)(struct thrift_protocol *, uint64_t *, uint64_t *, int16_t *, int *, bool);
    enum thrift_status (*container)(struct thrift_protocol *, enum thrift_wire,
                                       uint64_t *, uint64_t *, uint64_t *, bool);
    enum thrift_status (*length)(struct thrift_protocol *, uint64_t *, bool);
    enum thrift_status (*message)(struct thrift_protocol *, struct thrift_bytes *,
                                     enum thrift_message *, int32_t *, bool);
};
extern const struct thrift_protocol_ops thrift_binary_ops;
extern const struct thrift_protocol_ops thrift_compact_ops;
enum thrift_status thrift_bytes_io(struct thrift_protocol *, struct thrift_bytes *, bool);
bool thrift_valid_wire(uint64_t wire);
enum thrift_status thrift_message_name(struct thrift_protocol *, const char *,
    enum thrift_message, struct thrift_bytes *, bool *);
#endif
