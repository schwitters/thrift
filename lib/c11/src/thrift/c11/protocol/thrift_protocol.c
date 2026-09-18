/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_protocol_internal.h"
#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(CHAR_BIT == 8, "Thrift requires 8-bit bytes");
_Static_assert(sizeof(double) == sizeof(uint64_t) && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "Thrift requires IEEE 754 binary64 doubles");
_Static_assert(INT64_MIN == -INT64_MAX - 1, "Thrift requires two's complement integers");

enum { C11_DEFAULT_BYTES = 16 * 1024 * 1024, C11_DEFAULT_ALLOCATION = 32 * 1024 * 1024,
       C11_DEFAULT_CONTAINER = 1024 * 1024, C11_DEFAULT_DEPTH = 64, C11_WORD_BYTES = C11_I64_BYTES };

void thrift_protocol_reset(struct thrift_protocol *p)
{
    if (p) {
        p->remaining_bytes = p->limits.max_bytes;
        p->remaining_allocation = p->limits.max_allocation;
    }
}

enum thrift_status thrift_protocol_init_kind(struct thrift_protocol *p,
    struct thrift_transport transport, enum thrift_protocol_kind kind)
{
    if (!p || (!transport.read && !transport.write) ||
        (kind != THRIFT_BINARY && kind != THRIFT_COMPACT))
        return THRIFT_INVALID;
    p->ops = kind == THRIFT_BINARY ? &thrift_binary_ops : &thrift_compact_ops;
    p->kind = kind;
    p->service_name = NULL;
    p->transport = transport;
    p->limits.max_bytes = C11_DEFAULT_BYTES;
    p->limits.max_allocation = C11_DEFAULT_ALLOCATION;
    p->limits.max_container = C11_DEFAULT_CONTAINER;
    p->limits.max_depth = C11_DEFAULT_DEPTH;
    thrift_protocol_reset(p);
    return THRIFT_OK;
}

enum thrift_status thrift_protocol_init(struct thrift_protocol *p,
                                               struct thrift_transport transport)
{
    return thrift_protocol_init_kind(p, transport, THRIFT_BINARY);
}

enum thrift_status thrift_transfer(struct thrift_protocol *p, void *data, size_t size, bool read)
{
    if (!p || (!data && size) || (read ? !p->transport.read : !p->transport.write))
        return THRIFT_INVALID;
    if (size > p->remaining_bytes)
        return THRIFT_LIMIT;
    p->remaining_bytes -= size;
    if (!size)
        return THRIFT_OK;
    if (read)
        return p->transport.read(p->transport.context, data, size);
    return p->transport.write(p->transport.context, data, size);
}

enum thrift_status thrift_number(struct thrift_protocol *p, uint64_t *value, size_t size, bool read)
{
    uint8_t bytes[C11_WORD_BYTES];
    enum thrift_status status;
    size_t i;
    if (!value || !size || size > sizeof(bytes))
        return THRIFT_INVALID;
    if (!read) {
        for (i = 0; i < size; ++i)
            bytes[size - i - 1] = (uint8_t)(*value >> (i * CHAR_BIT));
    }
    status = thrift_transfer(p, bytes, size, read);
    if (status != THRIFT_OK)
        return status;
    if (read) {
        *value = 0;
        for (i = 0; i < size; ++i)
            *value = (*value << CHAR_BIT) | bytes[i];
    }
    return THRIFT_OK;
}

enum thrift_status thrift_allocate(struct thrift_protocol *p, size_t count, size_t size, void **out)
{
    if (!p)
        return THRIFT_INVALID;
    return thrift_alloc(count, size, &p->remaining_allocation, out);
}

static enum thrift_status value_io(struct thrift_protocol *p,
                                      const struct thrift_type *type, void *value,
                                      unsigned depth, bool read);

static enum thrift_status record_io(struct thrift_protocol *p,
                                       const struct thrift_record *record, void *value,
                                       unsigned depth, bool read)
{
    enum thrift_status status = THRIFT_OK;
    bool *seen = NULL;
    size_t i, active = 0;
    uint64_t wire = THRIFT_STOP, id = 0;
    int16_t last_id = 0;
    int bool_inline = 0;
    if (depth >= p->limits.max_depth)
        return THRIFT_LIMIT;
    if (read) {
        void *allocation = NULL;
        status = thrift_allocate(p, record->field_count, sizeof(*seen), &allocation);
        if (status != THRIFT_OK)
            return status;
        seen = allocation;
        for (;;) {
            const struct thrift_field *field = NULL;
            status = p->ops->field(p, &wire, &id, &last_id, &bool_inline, true);
            if (status != THRIFT_OK || wire == THRIFT_STOP)
                break;
            for (i = 0; i < record->field_count; ++i) {
                if ((uint16_t)record->fields[i].id == id) {
                    field = &record->fields[i];
                    break;
                }
            }
            if (!field || (uint64_t)field->type->wire != wire) {
                status = bool_inline ? (depth + 1 >= p->limits.max_depth ? THRIFT_LIMIT : THRIFT_OK)
                                     : thrift_skip(p, (enum thrift_wire)wire, depth + 1);
            } else if (seen[i]) {
                status = THRIFT_PROTOCOL;
            } else {
                void *member = (uint8_t *)value + field->offset;
                seen[i] = true;
                ++active;
                if (record->exclusive && active > 1) {
                    status = THRIFT_PROTOCOL;
                    break;
                }
                if (record->exclusive && active == 1)
                    thrift_record_clear(record, value);
                thrift_value_clear(field->type, member);
                if (bool_inline) {
                    *(bool *)member = bool_inline == 1;
                    status = depth + 1 >= p->limits.max_depth ? THRIFT_LIMIT : THRIFT_OK;
                } else {
                    status = value_io(p, field->type, member, depth + 1, true);
                }
                if (status == THRIFT_OK)
                    *(bool *)((uint8_t *)value + field->present_offset) = true;
            }
            if (status != THRIFT_OK)
                break;
        }
        if (status == THRIFT_OK) {
            for (i = 0; i < record->field_count; ++i) {
                if (record->fields[i].required && !seen[i]) {
                    status = THRIFT_REQUIRED;
                    break;
                }
            }
        }
        free(seen);
        return status;
    }
    /* Check exclusive records before emitting any of their fields. */
    for (i = 0; i < record->field_count; ++i) {
        const struct thrift_field *field = &record->fields[i];
        bool present = *(const bool *)((const uint8_t *)value + field->present_offset);
        if (!field->optional || present)
            ++active;
    }
    if (record->exclusive && active > 1)
        return THRIFT_INVALID;
    for (i = 0; i < record->field_count; ++i) {
        const struct thrift_field *field = &record->fields[i];
        bool present = *(const bool *)((const uint8_t *)value + field->present_offset);
        if (field->optional && !present)
            continue;
        wire = field->type->wire;
        id = (uint16_t)field->id;
        bool_inline = field->type->wire == THRIFT_BOOL
            ? (*(const bool *)((const uint8_t *)value + field->offset) ? 1 : 2) : 0;
        status = p->ops->field(p, &wire, &id, &last_id, &bool_inline, false);
        if (status == THRIFT_OK && bool_inline && depth + 1 >= p->limits.max_depth)
            status = THRIFT_LIMIT;
        if (status == THRIFT_OK && !bool_inline)
            status = value_io(p, field->type, (uint8_t *)value + field->offset, depth + 1, false);
        if (status != THRIFT_OK)
            return status;
    }
    wire = THRIFT_STOP;
    return p->ops->field(p, &wire, &id, &last_id, &bool_inline, false);
}

enum thrift_status thrift_bytes_io(struct thrift_protocol *p,
                                      struct thrift_bytes *bytes, bool read)
{
    uint64_t length = bytes->size;
    enum thrift_status status;
    if (!read && (bytes->size > INT32_MAX || (!bytes->data && bytes->size)))
        return THRIFT_INVALID;
    status = p->ops->length(p, &length, read);
    if (status != THRIFT_OK)
        return status;
    if (length > INT32_MAX || length > p->remaining_bytes || length >= SIZE_MAX)
        return THRIFT_LIMIT;
    if (read) {
        void *allocation = NULL;
        status = thrift_allocate(p, (size_t)length + 1, 1, &allocation);
        if (status != THRIFT_OK)
            return status;
        bytes->data = allocation;
        bytes->size = (size_t)length;
    }
    return thrift_transfer(p, bytes->data, bytes->size, read);
}

static enum thrift_status container_io(struct thrift_protocol *p,
                                          const struct thrift_type *type, void *value,
                                          unsigned depth, bool read)
{
    struct thrift_map *map = type->wire == THRIFT_MAP ? value : NULL;
    struct thrift_list *list = map ? NULL : value;
    size_t count = map ? map->size : list->size, i;
    void *keys = map ? map->keys : NULL;
    void *elements = map ? map->values : list->data;
    uint64_t key_wire = map ? (uint64_t)type->key->wire : 0;
    uint64_t element_wire = type->element->wire, length = count;
    enum thrift_status status = THRIFT_OK;
    if (!read && (count > INT32_MAX || count > p->limits.max_container ||
                  count > SIZE_MAX / type->element->size ||
                  (map && count > SIZE_MAX / type->key->size) ||
                  (count && (!elements || (map && !keys)))))
        return THRIFT_INVALID;
    status = p->ops->container(p, type->wire, &key_wire, &element_wire, &length, read);
    if (status != THRIFT_OK)
        return status;
    if (element_wire != (uint64_t)type->element->wire ||
        (map && key_wire != (uint64_t)type->key->wire))
        return THRIFT_PROTOCOL;
    if (length > INT32_MAX || length > p->limits.max_container || length > p->remaining_bytes)
        return THRIFT_LIMIT;
    count = (size_t)length;
    if (read) {
        if (map) {
            status = thrift_allocate(p, count, type->key->size, &map->keys);
            keys = map->keys;
            map->size = count;
        } else {
            list->size = count;
        }
        if (status == THRIFT_OK)
            status = thrift_allocate(p, count, type->element->size, map ? &map->values : &list->data);
        if (status != THRIFT_OK)
            return status;
        if (map) map->capacity = count; else list->capacity = count;
        elements = map ? map->values : list->data;
    }
    for (i = 0; i < count; ++i) {
        if (map)
            status = value_io(p, type->key, (uint8_t *)keys + i * type->key->size, depth + 1, read);
        if (status == THRIFT_OK)
            status = value_io(p, type->element, (uint8_t *)elements + i * type->element->size, depth + 1, read);
        if (status != THRIFT_OK)
            return status;
    }
    return THRIFT_OK;
}

static enum thrift_status value_io(struct thrift_protocol *p,
                                      const struct thrift_type *type, void *value,
                                      unsigned depth, bool read)
{
    uint64_t number = 0;
    enum thrift_status status;
    if (depth >= p->limits.max_depth)
        return THRIFT_LIMIT;
    switch (type->wire) {
    case THRIFT_BOOL:
        number = *(bool *)value ? 1 : 0;
        status = p->ops->scalar(p, THRIFT_BOOL, &number, read);
        if (status == THRIFT_OK && read) {
            if (number > 1)
                return THRIFT_PROTOCOL;
            *(bool *)value = number != 0;
        }
        return status;
    case THRIFT_BYTE: number = (uint8_t)*(int8_t *)value; break;
    case THRIFT_I16: number = (uint16_t)*(int16_t *)value; break;
    case THRIFT_I32: {
        uint32_t bits;
        /* Native C enums share the i32 wire descriptor; avoid typed aliasing. */
        memcpy(&bits, value, sizeof(bits));
        number = bits;
        break;
    }
    case THRIFT_I64: number = (uint64_t)*(int64_t *)value; break;
    case THRIFT_DOUBLE: memcpy(&number, value, sizeof(number)); break;
    case THRIFT_UUID: return thrift_transfer(p, value, sizeof(struct thrift_uuid), read);
    case THRIFT_STRING: return thrift_bytes_io(p, value, read);
    case THRIFT_STRUCT: {
        void *object = NULL;
        memcpy(&object, value, sizeof(object));
        if (read) {
            status = thrift_allocate(p, 1, type->record->size, &object);
            if (status != THRIFT_OK)
                return status;
            memcpy(value, &object, sizeof(object));
            status = thrift_record_init_limited(type->record, object, &p->remaining_allocation);
            if (status != THRIFT_OK)
                return status;
        }
        if (!object)
            return THRIFT_INVALID;
        return record_io(p, type->record, object, depth, read);
    }
    case THRIFT_MAP:
    case THRIFT_LIST:
    case THRIFT_SET: return container_io(p, type, value, depth, read);
    default: return THRIFT_PROTOCOL;
    }
    status = p->ops->scalar(p, type->wire, &number, read);
    if (status == THRIFT_OK && read) {
        /* Convert unsigned bits to signed values without out-of-range casts. */
        switch (type->wire) {
        case THRIFT_BYTE:
            *(int8_t *)value = number <= INT8_MAX ? (int8_t)number : (int8_t)(-1 - (int)(UINT8_MAX - number));
            break;
        case THRIFT_I16:
            *(int16_t *)value = number <= INT16_MAX ? (int16_t)number : (int16_t)(-1 - (int)(UINT16_MAX - number));
            break;
        case THRIFT_I32: {
            uint32_t bits = (uint32_t)number;
            memcpy(value, &bits, sizeof(bits));
            break;
        }
        case THRIFT_I64:
            *(int64_t *)value = number <= INT64_MAX ? (int64_t)number : -1 - (int64_t)(UINT64_MAX - number);
            break;
        case THRIFT_DOUBLE: memcpy(value, &number, sizeof(number)); break;
        default: return THRIFT_PROTOCOL;
        }
    }
    return status;
}

enum thrift_status thrift_record_read(struct thrift_protocol *p,
                                             const struct thrift_record *record, void *value)
{
    void *temporary = NULL;
    enum thrift_status status;
    if (!p || !p->ops || !record || !value)
        return THRIFT_INVALID;
    status = thrift_allocate(p, 1, record->size, &temporary);
    if (status != THRIFT_OK)
        return status;
    status = thrift_record_init_limited(record, temporary, &p->remaining_allocation);
    if (status == THRIFT_OK)
        status = record_io(p, record, temporary, 0, true);
    if (status == THRIFT_OK) {
        thrift_record_clear(record, value);
        memcpy(value, temporary, record->size);
    } else {
        thrift_record_clear(record, temporary);
    }
    free(temporary);
    return status;
}

enum thrift_status thrift_record_write(struct thrift_protocol *p,
                                              const struct thrift_record *record,
                                              const void *value)
{
    if (!p || !p->ops || !record || !value)
        return THRIFT_INVALID;
    return record_io(p, record, (void *)value, 0, false);
}

bool thrift_valid_wire(uint64_t wire)
{
    switch (wire) {
    case THRIFT_BOOL: case THRIFT_BYTE: case THRIFT_DOUBLE:
    case THRIFT_I16: case THRIFT_I32: case THRIFT_I64:
    case THRIFT_STRING: case THRIFT_STRUCT: case THRIFT_MAP:
    case THRIFT_SET: case THRIFT_LIST: case THRIFT_UUID: return true;
    default: return false;
    }
}

enum thrift_status thrift_skip(struct thrift_protocol *p, enum thrift_wire wire, unsigned depth)
{
    enum { DISCARD_SIZE = 256 };
    uint8_t discard[DISCARD_SIZE];
    uint64_t length = 0, element = 0, key = 0, ignored = 0, i;
    int16_t last_id = 0;
    int bool_inline = 0;
    enum thrift_status status;
    if (depth >= p->limits.max_depth)
        return THRIFT_LIMIT;
    switch (wire) {
    case THRIFT_BOOL: case THRIFT_BYTE: case THRIFT_I16:
    case THRIFT_I32: case THRIFT_I64: case THRIFT_DOUBLE:
        return p->ops->scalar(p, wire, &ignored, true);
    case THRIFT_UUID:
        return thrift_transfer(p, discard, THRIFT_UUID_SIZE, true);
    case THRIFT_STRING:
        status = p->ops->length(p, &length, true);
        if (status != THRIFT_OK)
            return status;
        if (length > INT32_MAX || length > p->remaining_bytes)
            return THRIFT_LIMIT;
        while (length) {
            size_t chunk = length > sizeof(discard) ? sizeof(discard) : (size_t)length;
            status = thrift_transfer(p, discard, chunk, true);
            if (status != THRIFT_OK)
                return status;
            length -= chunk;
        }
        return THRIFT_OK;
    case THRIFT_STRUCT:
        for (;;) {
            status = p->ops->field(p, &element, &ignored, &last_id, &bool_inline, true);
            if (status != THRIFT_OK || element == THRIFT_STOP)
                return status;
            if (bool_inline)
                status = depth + 1 >= p->limits.max_depth ? THRIFT_LIMIT : THRIFT_OK;
            else
                status = thrift_skip(p, (enum thrift_wire)element, depth + 1);
            if (status != THRIFT_OK)
                return status;
        }
    case THRIFT_MAP: case THRIFT_LIST: case THRIFT_SET:
        status = p->ops->container(p, wire, &key, &element, &length, true);
        if (status != THRIFT_OK)
            return status;
        if (wire == THRIFT_MAP && !length && p->kind == THRIFT_COMPACT)
            return THRIFT_OK;
        if (!thrift_valid_wire(element) || (wire == THRIFT_MAP && !thrift_valid_wire(key)))
            return THRIFT_PROTOCOL;
        if (length > INT32_MAX || length > p->limits.max_container || length > p->remaining_bytes)
            return THRIFT_LIMIT;
        for (i = 0; i < length; ++i) {
            if (wire == THRIFT_MAP) {
                status = thrift_skip(p, (enum thrift_wire)key, depth + 1);
                if (status != THRIFT_OK)
                    return status;
            }
            status = thrift_skip(p, (enum thrift_wire)element, depth + 1);
            if (status != THRIFT_OK)
                return status;
        }
        return THRIFT_OK;
    default: return THRIFT_PROTOCOL;
    }
}

enum thrift_status thrift_message_write(struct thrift_protocol *p, const char *name,
                                        enum thrift_message kind, int32_t sequence)
{
    struct thrift_bytes bytes = {0};
    bool owned = false;
    enum thrift_status status;
    if (!p || !p->ops || !name)
        return THRIFT_INVALID;
    thrift_protocol_reset(p);
    status = thrift_message_name(p, name, kind, &bytes, &owned);
    if (status == THRIFT_OK)
        status = p->ops->message(p, &bytes, &kind, &sequence, false);
    if (owned)
        free(bytes.data);
    return status;
}

enum thrift_status thrift_message_read(struct thrift_protocol *p, struct thrift_bytes *name,
                                       enum thrift_message *kind, int32_t *sequence)
{
    if (!p || !p->ops || !name || !kind || !sequence)
        return THRIFT_INVALID;
    thrift_protocol_reset(p);
    return p->ops->message(p, name, kind, sequence, true);
}
