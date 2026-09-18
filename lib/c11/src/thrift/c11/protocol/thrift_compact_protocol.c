/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/protocol/thrift_compact_protocol.h>
#include "thrift_protocol_internal.h"
#include <limits.h>

enum {
    COMPACT_PROTOCOL_ID = 0x82, COMPACT_VERSION = 1,
    COMPACT_STOP = 0, COMPACT_TRUE = 1, COMPACT_FALSE = 2, COMPACT_BYTE = 3,
    COMPACT_I16 = 4, COMPACT_I32 = 5, COMPACT_I64 = 6, COMPACT_DOUBLE = 7,
    COMPACT_BINARY = 8, COMPACT_LIST = 9, COMPACT_SET = 10, COMPACT_MAP = 11,
    COMPACT_STRUCT = 12, COMPACT_UUID = 13, COMPACT_EXTENDED_SIZE = 15,
    COMPACT_VARINT_BITS = 7
};

/* The iteration bound and final-byte mask reject overflowing and unterminated
 * varints before a shift can exceed the width of uint64_t. */
static enum thrift_status varint(struct thrift_protocol *p,
                                    uint64_t *value, unsigned bits, bool read)
{
    uint64_t remaining = *value, decoded = 0;
    unsigned shift;
    if (!read && bits < C11_I64_BITS && remaining >= (UINT64_C(1) << bits))
        return THRIFT_LIMIT;
    for (shift = 0; shift < bits; shift += COMPACT_VARINT_BITS) {
        uint64_t byte = remaining & 0x7f;
        unsigned available = bits - shift < COMPACT_VARINT_BITS ? bits - shift : COMPACT_VARINT_BITS;
        enum thrift_status status;
        if (!read) {
            remaining >>= COMPACT_VARINT_BITS;
            if (remaining)
                byte |= 0x80;
        }
        status = thrift_number(p, &byte, C11_BYTE_BYTES, read);
        if (status != THRIFT_OK)
            return status;
        if ((byte & 0x7f) >= (UINT64_C(1) << available))
            return THRIFT_PROTOCOL;
        decoded |= (byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            if (read)
                *value = decoded;
            return THRIFT_OK;
        }
    }
    return THRIFT_PROTOCOL;
}

static enum thrift_status zigzag(struct thrift_protocol *p,
                                    uint64_t *value, unsigned width, bool read)
{
    uint64_t mask = width == C11_I64_BITS ? UINT64_MAX : (UINT64_C(1) << width) - 1;
    uint64_t encoded = ((*value << 1) ^ (UINT64_C(0) - (*value >> (width - 1)))) & mask;
    enum thrift_status status = varint(p, &encoded, width, read);
    if (status == THRIFT_OK && read)
        *value = ((encoded >> 1) ^ (UINT64_C(0) - (encoded & 1))) & mask;
    return status;
}

static enum thrift_status compact_scalar(struct thrift_protocol *p,
    enum thrift_wire wire, uint64_t *bits, bool read)
{
    enum thrift_status status;
    switch (wire) {
    case THRIFT_BOOL: {
        uint64_t byte = *bits ? COMPACT_TRUE : COMPACT_FALSE;
        status = thrift_number(p, &byte, C11_BYTE_BYTES, read);
        if (status != THRIFT_OK)
            return status;
        if (byte != COMPACT_TRUE && byte != COMPACT_FALSE)
            return THRIFT_PROTOCOL;
        if (read)
            *bits = byte == COMPACT_TRUE;
        return THRIFT_OK;
    }
    case THRIFT_BYTE: return thrift_number(p, bits, C11_BYTE_BYTES, read);
    case THRIFT_I16: return zigzag(p, bits, C11_I16_BITS, read);
    case THRIFT_I32: return zigzag(p, bits, C11_I32_BITS, read);
    case THRIFT_I64: return zigzag(p, bits, C11_I64_BITS, read);
    case THRIFT_DOUBLE: {
        uint8_t bytes[C11_I64_BYTES];
        size_t i;
        if (!read)
            for (i = 0; i < sizeof(bytes); ++i)
                bytes[i] = (uint8_t)(*bits >> (i * CHAR_BIT));
        status = thrift_transfer(p, bytes, sizeof(bytes), read);
        if (status == THRIFT_OK && read) {
            *bits = 0;
            for (i = 0; i < sizeof(bytes); ++i)
                *bits |= (uint64_t)bytes[i] << (i * CHAR_BIT);
        }
        return status;
    }
    default: return THRIFT_PROTOCOL;
    }
}

static uint64_t compact_type(uint64_t wire)
{
    switch (wire) {
    case THRIFT_BOOL: return COMPACT_TRUE;
    case THRIFT_BYTE: return COMPACT_BYTE;
    case THRIFT_I16: return COMPACT_I16;
    case THRIFT_I32: return COMPACT_I32;
    case THRIFT_I64: return COMPACT_I64;
    case THRIFT_DOUBLE: return COMPACT_DOUBLE;
    case THRIFT_STRING: return COMPACT_BINARY;
    case THRIFT_LIST: return COMPACT_LIST;
    case THRIFT_SET: return COMPACT_SET;
    case THRIFT_MAP: return COMPACT_MAP;
    case THRIFT_STRUCT: return COMPACT_STRUCT;
    case THRIFT_UUID: return COMPACT_UUID;
    default: return COMPACT_STOP;
    }
}

static uint64_t wire_type(uint64_t compact)
{
    switch (compact) {
    case COMPACT_TRUE: case COMPACT_FALSE: return THRIFT_BOOL;
    case COMPACT_BYTE: return THRIFT_BYTE;
    case COMPACT_I16: return THRIFT_I16;
    case COMPACT_I32: return THRIFT_I32;
    case COMPACT_I64: return THRIFT_I64;
    case COMPACT_DOUBLE: return THRIFT_DOUBLE;
    case COMPACT_BINARY: return THRIFT_STRING;
    case COMPACT_LIST: return THRIFT_LIST;
    case COMPACT_SET: return THRIFT_SET;
    case COMPACT_MAP: return THRIFT_MAP;
    case COMPACT_STRUCT: return THRIFT_STRUCT;
    case COMPACT_UUID: return THRIFT_UUID;
    default: return THRIFT_STOP;
    }
}

static enum thrift_status compact_field(struct thrift_protocol *p,
    uint64_t *wire, uint64_t *id, int16_t *last_id, int *bool_inline, bool read)
{
    int32_t signed_id = *id <= INT16_MAX ? (int32_t)*id : -1 - (int32_t)(UINT16_MAX - *id);
    int32_t delta = signed_id - *last_id;
    uint64_t header = COMPACT_STOP;
    enum thrift_status status;
    if (!read && *wire != THRIFT_STOP) {
        header = compact_type(*wire);
        if (!header)
            return THRIFT_PROTOCOL;
        if (*wire == THRIFT_BOOL)
            header = *bool_inline == 1 ? COMPACT_TRUE : COMPACT_FALSE;
        if (delta > 0 && delta <= COMPACT_EXTENDED_SIZE)
            header |= (uint64_t)delta << 4;
    }
    status = thrift_number(p, &header, C11_BYTE_BYTES, read);
    if (status != THRIFT_OK)
        return status;
    if (!header) {
        *wire = THRIFT_STOP;
        *bool_inline = 0;
        return THRIFT_OK;
    }
    if (read) {
        *wire = wire_type(header & 0x0f);
        if (*wire == THRIFT_STOP)
            return THRIFT_PROTOCOL;
        *bool_inline = *wire == THRIFT_BOOL ? (int)(header & 0x0f) : 0;
    }
    if (header >> 4) {
        signed_id = *last_id + (int32_t)(header >> 4);
        if (signed_id > INT16_MAX)
            return THRIFT_PROTOCOL;
        *id = (uint16_t)signed_id;
    } else {
        status = zigzag(p, id, C11_I16_BITS, read);
        if (status != THRIFT_OK)
            return status;
        signed_id = *id <= INT16_MAX ? (int32_t)*id : -1 - (int32_t)(UINT16_MAX - *id);
    }
    *last_id = (int16_t)signed_id;
    return THRIFT_OK;
}

static enum thrift_status compact_length(struct thrift_protocol *p, uint64_t *length, bool read)
{
    return varint(p, length, C11_I32_BITS, read);
}

static enum thrift_status compact_container(struct thrift_protocol *p,
    enum thrift_wire wire, uint64_t *key, uint64_t *element, uint64_t *count, bool read)
{
    uint64_t header;
    enum thrift_status status;
    if (wire == THRIFT_MAP) {
        status = compact_length(p, count, read);
        if (status != THRIFT_OK || !*count)
            return status;
        header = (compact_type(*key) << 4) | compact_type(*element);
        status = thrift_number(p, &header, C11_BYTE_BYTES, read);
        if (status != THRIFT_OK)
            return status;
        *key = wire_type(header >> 4);
        *element = wire_type(header & 0x0f);
        return *key && *element ? THRIFT_OK : THRIFT_PROTOCOL;
    }
    header = ((*count < COMPACT_EXTENDED_SIZE ? *count : COMPACT_EXTENDED_SIZE) << 4) | compact_type(*element);
    status = thrift_number(p, &header, C11_BYTE_BYTES, read);
    if (status != THRIFT_OK)
        return status;
    *element = wire_type(header & 0x0f);
    if (!*element)
        return THRIFT_PROTOCOL;
    if ((header >> 4) == COMPACT_EXTENDED_SIZE)
        return compact_length(p, count, read);
    if (read)
        *count = header >> 4;
    return THRIFT_OK;
}

static enum thrift_status compact_message(struct thrift_protocol *p,
    struct thrift_bytes *name, enum thrift_message *kind, int32_t *sequence, bool read)
{
    uint64_t byte = COMPACT_PROTOCOL_ID, number;
    enum thrift_status status = thrift_number(p, &byte, C11_BYTE_BYTES, read);
    if (status != THRIFT_OK)
        return status;
    if (byte != COMPACT_PROTOCOL_ID)
        return THRIFT_PROTOCOL;
    byte = ((uint64_t)*kind << 5) | COMPACT_VERSION;
    status = thrift_number(p, &byte, C11_BYTE_BYTES, read);
    if (status != THRIFT_OK)
        return status;
    if ((byte & 0x1f) != COMPACT_VERSION || (byte >> 5) < C11_CALL || (byte >> 5) > C11_ONEWAY)
        return THRIFT_PROTOCOL;
    *kind = (enum thrift_message)(byte >> 5);
    number = (uint32_t)*sequence;
    status = varint(p, &number, C11_I32_BITS, read);
    if (status != THRIFT_OK)
        return status;
    if (read)
        *sequence = number <= INT32_MAX ? (int32_t)number : -1 - (int32_t)(UINT32_MAX - number);
    return thrift_bytes_io(p, name, read);
}

const struct thrift_protocol_ops thrift_compact_ops = {
    compact_scalar, compact_field, compact_container, compact_length, compact_message
};

enum thrift_status thrift_compact_protocol_init(struct thrift_protocol *p,
                                                       struct thrift_transport transport)
{
    return thrift_protocol_init_kind(p, transport, THRIFT_COMPACT);
}
