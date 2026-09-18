/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/thrift.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

const struct thrift_type thrift_type_bool = {THRIFT_BOOL, sizeof(bool), NULL, NULL, NULL};
const struct thrift_type thrift_type_byte = {THRIFT_BYTE, sizeof(int8_t), NULL, NULL, NULL};
const struct thrift_type thrift_type_i16 = {THRIFT_I16, sizeof(int16_t), NULL, NULL, NULL};
const struct thrift_type thrift_type_i32 = {THRIFT_I32, sizeof(int32_t), NULL, NULL, NULL};
const struct thrift_type thrift_type_i64 = {THRIFT_I64, sizeof(int64_t), NULL, NULL, NULL};
const struct thrift_type thrift_type_double = {THRIFT_DOUBLE, sizeof(double), NULL, NULL, NULL};
const struct thrift_type thrift_type_string = {THRIFT_STRING, sizeof(struct thrift_bytes), NULL, NULL, NULL};
const struct thrift_type thrift_type_uuid = {THRIFT_UUID, sizeof(struct thrift_uuid), NULL, NULL, NULL};

const char *thrift_status_string(enum thrift_status status)
{
    switch (status) {
    case THRIFT_OK: return "success";
    case THRIFT_INVALID: return "invalid argument or object state";
    case THRIFT_NOMEM: return "allocation failed";
    case THRIFT_IO: return "transport error";
    case THRIFT_TIMEOUT: return "transport timeout";
    case THRIFT_EOF: return "unexpected end of stream";
    case THRIFT_PROTOCOL: return "invalid protocol message";
    case THRIFT_LIMIT: return "resource limit exceeded";
    case THRIFT_REQUIRED: return "required field missing";
    case THRIFT_REMOTE: return "remote application exception";
    case THRIFT_MISSING_RESULT: return "method result missing";
    case THRIFT_AGAIN: return "operation would block";
    case THRIFT_NULL_CLIENT: return "client pointer is NULL";
    case THRIFT_NULL_PROTOCOL: return "protocol pointer is NULL";
    case THRIFT_PROTOCOL_UNINITIALIZED: return "protocol is not initialized";
    case THRIFT_NULL_ARGS: return "RPC argument record is NULL";
    case THRIFT_NULL_RESULT: return "RPC result record is NULL";
    case THRIFT_INVALID_SEQUENCE: return "automatic client sequence is invalid";
    case THRIFT_NULL_HANDLER: return "server handler pointer is NULL";
    case THRIFT_NULL_METHOD: return "method descriptor or table is NULL";
    case THRIFT_MISSING_CALLBACK: return "method callback is not installed";
    case THRIFT_NOT_IMPLEMENTED: return "server method is not implemented";
    case THRIFT_NULL_VALUE: return "destination value pointer is NULL";
    case THRIFT_NULL_FIELD: return "field value pointer is NULL";
    case THRIFT_NULL_DATA: return "nonempty input has no data storage";
    case THRIFT_INVALID_CAPACITY: return "container size exceeds capacity";
    case THRIFT_NULL_MAP_KEYS: return "nonempty map has no key storage";
    case THRIFT_NULL_MAP_VALUES: return "nonempty map has no value storage";
    case THRIFT_INVALID_OWNERSHIP: return "self-ownership or shared destination storage";
    case THRIFT_OUTPUT_NOT_EMPTY: return "allocation output is not empty";
    default: return "unknown status";
    }
}

enum thrift_status thrift_alloc(size_t count, size_t size, size_t *budget, void **out)
{
    size_t bytes;
    if (!out)
        return THRIFT_INVALID;
    *out = NULL;
    if (!size)
        return THRIFT_INVALID;
    if (count > SIZE_MAX / size)
        return THRIFT_LIMIT;
    bytes = count * size;
    if (budget && bytes > *budget)
        return THRIFT_LIMIT;
    if (bytes) {
        *out = calloc(count, size);
        if (!*out)
            return THRIFT_NOMEM;
    }
    if (budget)
        *budget -= bytes;
    return THRIFT_OK;
}

enum thrift_status thrift_bytes_set_limited(struct thrift_bytes *value,
    const void *data, size_t size, size_t *budget)
{
    void *allocation = NULL;
    uint8_t *copy = NULL;
    enum thrift_status status;
    if (!value || (!data && size))
        return THRIFT_INVALID;
    if (size > INT32_MAX || size == SIZE_MAX)
        return THRIFT_LIMIT;
    if (size) {
        status = thrift_alloc(size + 1, 1, budget, &allocation);
        if (status != THRIFT_OK)
            return status;
        copy = allocation;
        memcpy(copy, data, size);
    }
    free(value->data);
    value->data = copy;
    value->size = size;
    return THRIFT_OK;
}

enum thrift_status thrift_bytes_set(struct thrift_bytes *value, const void *data, size_t size)
{
    return thrift_bytes_set_limited(value, data, size, NULL);
}

enum thrift_status thrift_bytes_set_cstr(struct thrift_bytes *value, const char *text)
{
    if (!value || !text)
        return THRIFT_INVALID;
    return thrift_bytes_set(value, text, strlen(text));
}

enum { THRIFT_CONTAINER_INITIAL_CAPACITY = 4, THRIFT_CONTAINER_GROWTH_FACTOR = 2 };

static enum thrift_status thrift_list_validate(const struct thrift_list *list,
    size_t element_size)
{
    size_t capacity;
    if (!list || !element_size)
        return THRIFT_INVALID;
    capacity = list->capacity ? list->capacity : list->size;
    if (list->size > capacity || (capacity && !list->data))
        return THRIFT_INVALID;
    if (capacity > INT32_MAX || capacity > SIZE_MAX / element_size)
        return THRIFT_LIMIT;
    return THRIFT_OK;
}

/* Copy the appended slot before freeing old storage to allow scalar self-append. */
static enum thrift_status thrift_list_grow(struct thrift_list *list, size_t capacity,
    size_t element_size, size_t *budget, const void *element)
{
    void *storage = NULL;
    enum thrift_status status = thrift_alloc(capacity, element_size, budget, &storage);
    if (status != THRIFT_OK)
        return status;
    if (list->size)
        memcpy(storage, list->data, list->size * element_size);
    if (element)
        memcpy((uint8_t *)storage + list->size * element_size, element, element_size);
    free(list->data);
    list->data = storage;
    list->capacity = capacity;
    if (element)
        ++list->size;
    return THRIFT_OK;
}

enum thrift_status thrift_list_reserve(struct thrift_list *list, size_t count,
    size_t element_size, size_t *budget)
{
    size_t capacity;
    enum thrift_status status = thrift_list_validate(list, element_size);
    if (status != THRIFT_OK)
        return status;
    if (count > INT32_MAX || count > SIZE_MAX / element_size)
        return THRIFT_LIMIT;
    capacity = list->capacity ? list->capacity : list->size;
    if (count <= capacity)
        return THRIFT_OK;
    return thrift_list_grow(list, count, element_size, budget, NULL);
}

enum thrift_status thrift_list_append(struct thrift_list *list, const void *element,
    size_t element_size, size_t *budget)
{
    size_t capacity, maximum;
    enum thrift_status status = thrift_list_validate(list, element_size);
    if (status != THRIFT_OK)
        return status;
    if (!element)
        return THRIFT_INVALID;
    maximum = SIZE_MAX / element_size;
    if (maximum > INT32_MAX)
        maximum = INT32_MAX;
    if (list->size >= maximum)
        return THRIFT_LIMIT;
    capacity = list->capacity ? list->capacity : list->size;
    if (list->size < capacity) {
        memmove((uint8_t *)list->data + list->size * element_size, element, element_size);
        ++list->size;
        return THRIFT_OK;
    }
    if (!capacity)
        capacity = maximum < THRIFT_CONTAINER_INITIAL_CAPACITY ? maximum : THRIFT_CONTAINER_INITIAL_CAPACITY;
    else
        capacity = capacity > maximum / THRIFT_CONTAINER_GROWTH_FACTOR
            ? maximum : capacity * THRIFT_CONTAINER_GROWTH_FACTOR;
    /* A tight budget can still accommodate exactly one additional slot. */
    if (budget && capacity > *budget / element_size)
        capacity = list->size + 1;
    return thrift_list_grow(list, capacity, element_size, budget, element);
}

static enum thrift_status thrift_map_validate(const struct thrift_map *map,
    size_t key_size, size_t value_size)
{
    size_t capacity;
    if (!map || !key_size || !value_size)
        return THRIFT_INVALID;
    capacity = map->capacity ? map->capacity : map->size;
    if (map->size > capacity)
        return THRIFT_INVALID;
    if (capacity && !map->keys)
        return THRIFT_NULL_MAP_KEYS;
    if (capacity && !map->values)
        return THRIFT_NULL_MAP_VALUES;
    if (capacity > INT32_MAX || capacity > SIZE_MAX / key_size || capacity > SIZE_MAX / value_size)
        return THRIFT_LIMIT;
    return THRIFT_OK;
}

/* Copy the appended pair before freeing old storage to allow scalar self-append,
 * same as thrift_list_grow(). Keys and values always share one capacity/size. */
static enum thrift_status thrift_map_grow(struct thrift_map *map, size_t capacity,
    size_t key_size, size_t value_size, size_t *budget, const void *key, const void *value)
{
    void *keys = NULL, *values = NULL;
    enum thrift_status status = thrift_alloc(capacity, key_size, budget, &keys);
    if (status != THRIFT_OK)
        return status;
    status = thrift_alloc(capacity, value_size, budget, &values);
    if (status != THRIFT_OK) {
        free(keys);
        /* The keys allocation already succeeded and charged budget; refund it
         * so a failed grow leaves the budget untouched, matching thrift_list's
         * single-allocation contract (there is nothing to refund because a
         * failed single thrift_alloc() never charges in the first place). */
        if (budget)
            *budget += capacity * key_size;
        return status;
    }
    if (map->size) {
        memcpy(keys, map->keys, map->size * key_size);
        memcpy(values, map->values, map->size * value_size);
    }
    if (key) {
        memcpy((uint8_t *)keys + map->size * key_size, key, key_size);
        memcpy((uint8_t *)values + map->size * value_size, value, value_size);
    }
    free(map->keys);
    free(map->values);
    map->keys = keys;
    map->values = values;
    map->capacity = capacity;
    if (key)
        ++map->size;
    return THRIFT_OK;
}

enum thrift_status thrift_map_reserve(struct thrift_map *map, size_t count,
    size_t key_size, size_t value_size, size_t *budget)
{
    size_t capacity;
    enum thrift_status status = thrift_map_validate(map, key_size, value_size);
    if (status != THRIFT_OK)
        return status;
    if (count > INT32_MAX || count > SIZE_MAX / key_size || count > SIZE_MAX / value_size)
        return THRIFT_LIMIT;
    capacity = map->capacity ? map->capacity : map->size;
    if (count <= capacity)
        return THRIFT_OK;
    return thrift_map_grow(map, count, key_size, value_size, budget, NULL, NULL);
}

enum thrift_status thrift_map_append(struct thrift_map *map, const void *key, const void *value,
    size_t key_size, size_t value_size, size_t *budget)
{
    size_t capacity, maximum, budget_capacity;
    enum thrift_status status = thrift_map_validate(map, key_size, value_size);
    if (status != THRIFT_OK)
        return status;
    if (!key || !value)
        return THRIFT_INVALID;
    maximum = SIZE_MAX / key_size;
    if (SIZE_MAX / value_size < maximum)
        maximum = SIZE_MAX / value_size;
    if (maximum > INT32_MAX)
        maximum = INT32_MAX;
    if (map->size >= maximum)
        return THRIFT_LIMIT;
    capacity = map->capacity ? map->capacity : map->size;
    if (map->size < capacity) {
        memmove((uint8_t *)map->keys + map->size * key_size, key, key_size);
        memmove((uint8_t *)map->values + map->size * value_size, value, value_size);
        ++map->size;
        return THRIFT_OK;
    }
    if (!capacity)
        capacity = maximum < THRIFT_CONTAINER_INITIAL_CAPACITY ? maximum : THRIFT_CONTAINER_INITIAL_CAPACITY;
    else
        capacity = capacity > maximum / THRIFT_CONTAINER_GROWTH_FACTOR
            ? maximum : capacity * THRIFT_CONTAINER_GROWTH_FACTOR;
    if (budget) {
        /* Keys and values are charged together: bound by the combined per-pair
         * cost, not each array's cost alone, or this could still overshoot the
         * budget by up to 2x and fail the values allocation after already
         * spending the keys allocation. A tight budget can still accommodate
         * exactly one additional pair. key_size + value_size can overflow
         * (both are caller-supplied, not just generator sizeof() results);
         * when it does, no capacity can possibly fit the combined cost, so
         * force the minimal fallback rather than divide by the wrapped-to-zero
         * sum. thrift_alloc() inside thrift_map_grow() still rejects it correctly. */
        if (key_size > SIZE_MAX - value_size) {
            capacity = map->size + 1;
        } else {
            budget_capacity = *budget / (key_size + value_size);
            if (capacity > budget_capacity)
                capacity = map->size + 1;
        }
    }
    return thrift_map_grow(map, capacity, key_size, value_size, budget, key, value);
}

void thrift_record_clear(const struct thrift_record *record, void *value)
{
    size_t i;
    if (!record || !value)
        return;
    for (i = 0; i < record->field_count; ++i)
        thrift_value_clear(record->fields[i].type, (uint8_t *)value + record->fields[i].offset);
    memset(value, 0, record->size);
}

void thrift_value_clear(const struct thrift_type *type, void *value)
{
    size_t i;
    if (!type || !value)
        return;
    switch (type->wire) {
    case THRIFT_STRING:
        free(((struct thrift_bytes *)value)->data);
        break;
    case THRIFT_STRUCT: {
        void *object;
        memcpy(&object, value, sizeof(object));
        thrift_record_clear(type->record, object);
        free(object);
        break;
    }
    case THRIFT_LIST:
    case THRIFT_SET: {
        struct thrift_list *list = value;
        if (list->data)
            for (i = 0; i < list->size; ++i)
                thrift_value_clear(type->element, (uint8_t *)list->data + i * type->element->size);
        free(list->data);
        break;
    }
    case THRIFT_MAP: {
        struct thrift_map *map = value;
        for (i = 0; i < map->size; ++i) {
            if (map->keys)
                thrift_value_clear(type->key, (uint8_t *)map->keys + i * type->key->size);
            if (map->values)
                thrift_value_clear(type->element, (uint8_t *)map->values + i * type->element->size);
        }
        free(map->keys);
        free(map->values);
        break;
    }
    default: break;
    }
    memset(value, 0, type->size);
}

enum thrift_status thrift_record_init_limited(const struct thrift_record *record,
                                                     void *value, size_t *budget)
{
    enum thrift_status status;
    if (!record || !value)
        return THRIFT_INVALID;
    memset(value, 0, record->size);
    status = record->defaults ? record->defaults(value, budget) : THRIFT_OK;
    if (status != THRIFT_OK)
        thrift_record_clear(record, value);
    return status;
}

enum thrift_status thrift_record_init(const struct thrift_record *record, void *value)
{
    return thrift_record_init_limited(record, value, NULL);
}
