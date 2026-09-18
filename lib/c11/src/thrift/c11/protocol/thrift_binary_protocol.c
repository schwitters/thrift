/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/protocol/thrift_binary_protocol.h>
#include "thrift_protocol_internal.h"
#include <limits.h>

static const uint32_t binary_version = UINT32_C(0x80010000);

static enum thrift_status binary_scalar(struct thrift_protocol *p,
    enum thrift_wire wire, uint64_t *bits, bool read)
{
    size_t width;
    enum thrift_status status;
    switch (wire) {
    case THRIFT_BOOL: case THRIFT_BYTE: width = C11_BYTE_BYTES; break;
    case THRIFT_I16: width = C11_I16_BYTES; break;
    case THRIFT_I32: width = C11_I32_BYTES; break;
    case THRIFT_I64: case THRIFT_DOUBLE: width = C11_I64_BYTES; break;
    default: return THRIFT_PROTOCOL;
    }
    status = thrift_number(p, bits, width, read);
    if (status == THRIFT_OK && wire == THRIFT_BOOL && *bits > 1)
        return THRIFT_PROTOCOL;
    return status;
}

static enum thrift_status binary_field(struct thrift_protocol *p,
    uint64_t *wire, uint64_t *id, int16_t *last_id, int *bool_inline, bool read)
{
    enum thrift_status status;
    (void)last_id;
    *bool_inline = 0;
    status = thrift_number(p, wire, C11_BYTE_BYTES, read);
    if (status != THRIFT_OK || *wire == THRIFT_STOP)
        return status;
    if (!thrift_valid_wire(*wire))
        return THRIFT_PROTOCOL;
    return thrift_number(p, id, C11_I16_BYTES, read);
}

static enum thrift_status binary_length(struct thrift_protocol *p, uint64_t *length, bool read)
{
    return thrift_number(p, length, C11_I32_BYTES, read);
}

static enum thrift_status binary_container(struct thrift_protocol *p,
    enum thrift_wire wire, uint64_t *key, uint64_t *element, uint64_t *count, bool read)
{
    enum thrift_status status = THRIFT_OK;
    if (wire == THRIFT_MAP)
        status = thrift_number(p, key, C11_BYTE_BYTES, read);
    if (status == THRIFT_OK)
        status = thrift_number(p, element, C11_BYTE_BYTES, read);
    if (status == THRIFT_OK)
        status = binary_length(p, count, read);
    return status;
}

static enum thrift_status binary_message(struct thrift_protocol *p,
    struct thrift_bytes *name, enum thrift_message *kind, int32_t *sequence, bool read)
{
    uint64_t number = binary_version | (uint32_t)*kind;
    enum thrift_status status = thrift_number(p, &number, C11_I32_BYTES, read);
    if (status != THRIFT_OK)
        return status;
    if ((number & UINT32_C(0xffffff00)) != binary_version ||
        (number & 0xff) < C11_CALL || (number & 0xff) > C11_ONEWAY)
        return THRIFT_PROTOCOL;
    *kind = (enum thrift_message)(number & 0xff);
    status = thrift_bytes_io(p, name, read);
    number = (uint32_t)*sequence;
    if (status == THRIFT_OK)
        status = thrift_number(p, &number, C11_I32_BYTES, read);
    if (status == THRIFT_OK && read)
        *sequence = number <= INT32_MAX ? (int32_t)number : -1 - (int32_t)(UINT32_MAX - number);
    return status;
}

const struct thrift_protocol_ops thrift_binary_ops = {
    binary_scalar, binary_field, binary_container, binary_length, binary_message
};

enum thrift_status thrift_binary_protocol_init(struct thrift_protocol *p,
                                                      struct thrift_transport transport)
{
    return thrift_protocol_init_kind(p, transport, THRIFT_BINARY);
}
